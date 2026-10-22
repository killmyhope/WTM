#include "twm/layout_controller.hpp"
#include "twm/monitor_context.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace twm;
constexpr UINT delay_message = WM_APP + 90;
constexpr UINT state_message = WM_APP + 91;
constexpr wchar_t fixture_class[] = L"TwmActuatorChildFixture";
struct Shared {
    std::array<HWND, 5> windows{};
    DWORD pid{}, tid{};
    volatile LONG delay_state{};
};
Shared* shared{}; // Child-process state only; parent uses its separately mapped view.
void require(bool condition, const char* text) { if (!condition) { throw std::runtime_error(text); } }

LRESULT CALLBACK fixture_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto mode = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_GETMINMAXINFO && mode == 1) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.y = 420;
        return 0;
    }
    if (message == WM_WINDOWPOSCHANGING && mode == 2) {
        // A test terminal: always rounds outer height down to a 10px grid.
        const auto result = DefWindowProcW(hwnd, message, wp, lp);
        auto* position = reinterpret_cast<WINDOWPOS*>(lp);
        if ((position->flags & SWP_NOSIZE) == 0 && position->cy >= 100) {
            position->cy -= position->cy % 10;
        }
        return result;
    }
    if (message == delay_message) {
        InterlockedExchange(&shared->delay_state, 1);
        Sleep(static_cast<DWORD>(wp));
        InterlockedExchange(&shared->delay_state, 2);
        return 0;
    }
    if (message == state_message) { ShowWindow(hwnd, static_cast<int>(wp)); return 0; }
    return DefWindowProcW(hwnd, message, wp, lp);
}

int fixture_main(HANDLE mapping, HANDLE ready) {
    shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) { return 2; }
    WNDCLASSW klass{};
    klass.lpfnWndProc = fixture_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = fixture_class;
    if (!RegisterClassW(&klass)) { return 3; }
    shared->pid = GetCurrentProcessId(); shared->tid = GetCurrentThreadId();
    for (std::size_t i = 0; i < shared->windows.size(); ++i) {
        const auto mode = i == 1 ? 1 : (i == 2 ? 2 : 0);
        const auto hwnd = CreateWindowExW(0, fixture_class, L"TWM isolated child fixture",
            WS_OVERLAPPEDWINDOW, 40 + static_cast<int>(i) * 12, 50, 640, 480,
            nullptr, nullptr, GetModuleHandleW(nullptr), reinterpret_cast<void*>(static_cast<INT_PTR>(mode)));
        if (!hwnd) { return 4; }
        shared->windows[i] = hwnd;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    if (DwmFlush() != S_OK) { return 5; }
    SetEvent(ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    for (const auto hwnd : shared->windows) { if (IsWindow(hwnd)) { DestroyWindow(hwnd); } }
    UnmapViewOfFile(shared); CloseHandle(mapping); CloseHandle(ready);
    return 0;
}

void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
template<class Predicate> void wait_for(Predicate predicate, const char* reason, ULONGLONG ms = 4000) {
    const auto start = GetTickCount64();
    while (!predicate()) {
        require(GetTickCount64() - start < ms, reason);
        pump(); Sleep(10);
    }
}

class FixtureProcess {
public:
    FixtureProcess() {
        try {
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
            ready_ = CreateEventW(&security, TRUE, FALSE, nullptr);
            require(mapping_ && ready_, "fixture IPC creation");
            view = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
            require(view != nullptr, "fixture map view");
            wchar_t path[32768]{};
            require(GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))) != 0, "test exe path");
            std::wstring command = L"\"" + std::wstring(path) + L"\" --fixture " +
                std::to_wstring(reinterpret_cast<UINT_PTR>(mapping_)) + L" " +
                std::to_wstring(reinterpret_cast<UINT_PTR>(ready_));
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            require(CreateProcessW(path, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &startup, &process_) != FALSE, "start child fixture");
            wait_for([&] { return WaitForSingleObject(ready_, 0) == WAIT_OBJECT_0 ||
                                  WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0; }, "fixture startup");
            require(WaitForSingleObject(ready_, 0) == WAIT_OBJECT_0 && view->pid == process_.dwProcessId,
                    "child fixture identity handshake");
            for (const auto hwnd : view->windows) {
                DWORD pid{};
                require(GetWindowThreadProcessId(hwnd, &pid) == view->tid && pid == process_.dwProcessId,
                        "fixture HWND not owned by the spawned child");
            }
        } catch (...) { cleanup(); throw; }
    }
    ~FixtureProcess() { cleanup(); }
    WindowIdentity identity(std::size_t index) const { return {view->windows.at(index), view->pid, view->tid}; }
    Shared* view{};
private:
    void cleanup() noexcept {
        if (process_.hProcess) {
            PostThreadMessageW(process_.dwThreadId, WM_QUIT, 0, 0);
            if (WaitForSingleObject(process_.hProcess, 3000) == WAIT_TIMEOUT) {
                // Only our explicitly spawned fixture process, never user apps.
                TerminateProcess(process_.hProcess, 7);
                WaitForSingleObject(process_.hProcess, 1000);
            }
            CloseHandle(process_.hProcess); CloseHandle(process_.hThread);
        }
        if (view) { UnmapViewOfFile(view); }
        if (mapping_) { CloseHandle(mapping_); }
        if (ready_) { CloseHandle(ready_); }
    }
    HANDLE mapping_{}, ready_{};
    PROCESS_INFORMATION process_{};
};

// Every real SetWindowPos in these tests passes this process+HWND allowlist.
FixtureProcess* allowed{};
std::atomic<bool> block_next{}, entered{}, release_gate{};
std::atomic<unsigned> call_count{};
std::atomic<bool> invalid_call{};
BOOL WINAPI fixture_only_set_pos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags) {
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    const auto& windows = allowed->view->windows;
    const UINT expected = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
    if (pid != allowed->view->pid || std::find(windows.begin(), windows.end(), hwnd) == windows.end() ||
        flags != expected || after != nullptr) {
        invalid_call.store(true); SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    ++call_count;
    if (block_next.exchange(false)) {
        entered.store(true);
        while (!release_gate.load()) { Sleep(1); }
    }
    return SetWindowPos(hwnd, after, x, y, w, h, flags);
}

WindowCommand command(WindowId id, WindowIdentity identity, Rect outer, std::uint64_t request) {
    auto lease = std::make_shared<WindowLease>(); lease->id = id; lease->identity = identity;
    return {request, 1, 1, id, identity, lease, outer, MonitorFromWindow(identity.hwnd, MONITOR_DEFAULTTONULL),
            GetDpiForWindow(identity.hwnd)};
}
ActuationResult take_result(WindowActuator& actuator) {
    ActuationResult result;
    wait_for([&] { return actuator.pop_result(result); }, "actuator reply timeout");
    return result;
}

void test_queue_protocol(FixtureProcess& fixture, HWND control) {
    WindowActuator actuator(control, 3, {fixture_only_set_pos}); actuator.set_session(1);
    block_next = true; entered = false; release_gate = false;
    struct Release { ~Release() { release_gate = true; } } release;
    const auto first = command(1, fixture.identity(0), {100, 100, 600, 410}, 1);
    require(actuator.submit(first) == SubmitStatus::Accepted, "first accepted");
    wait_for([] { return entered.load(); }, "worker gate not entered");
    auto second = command(2, fixture.identity(3), {200, 100, 700, 410}, 2);
    require(actuator.submit(second) == SubmitStatus::Accepted, "queued second accepted");
    second.request_id = 3; second.layout_version = 2; second.target_outer.right += 20;
    require(actuator.submit(second) == SubmitStatus::Coalesced, "queued same WindowId must coalesce latest metadata");
    require(actuator.submit(command(3, fixture.identity(4), {200, 100, 700, 410}, 4)) == SubmitStatus::Accepted,
            "queued closing fixture");
    require(actuator.submit(command(4, fixture.identity(2), {200, 100, 700, 410}, 5)) == SubmitStatus::Backpressure,
            "in-flight+queued must exhaust reserved credits");
    // Destroy a HWND while its command is still in the bounded queue.
    PostMessageW(fixture.identity(4).hwnd, WM_CLOSE, 0, 0);
    wait_for([&] { return !IsWindow(fixture.identity(4).hwnd); }, "queued fixture close");
    release_gate = true;
    auto result = take_result(actuator);
    require(result.status == ActuationStatus::Submitted && result.command.request_id == 1, "first submission result");
    result = take_result(actuator);
    require(result.status == ActuationStatus::Submitted && result.command.request_id == 3 &&
            result.command.layout_version == 2, "coalesced request must preserve latest metadata");
    wait_for([&] { RECT rect{}; return GetWindowRect(second.identity.hwnd, &rect) && rect.right == 720; },
             "coalesced request must apply the latest target");
    result = take_result(actuator);
    require(result.status == ActuationStatus::Cancelled && result.command.request_id == 4,
            "closed queued target must cancel before calling SetWindowPos");
    // Replies occupy credits until drained even when no requests are queued.
    WindowActuator undrained(control, 1, {fixture_only_set_pos}); undrained.set_session(1);
    const auto before = call_count.load();
    require(undrained.submit(first) == SubmitStatus::Accepted, "result credit setup");
    wait_for([&] { return call_count.load() > before; }, "submitted call");
    Sleep(30);
    require(undrained.submit(first) == SubmitStatus::Backpressure, "undrained result must retain its reserved credit");
    (void)take_result(undrained);
    auto stale = first; stale.session_epoch = 99;
    require(undrained.submit(stale) == SubmitStatus::Accepted, "stale request accepted into transport");
    require(take_result(undrained).status == ActuationStatus::Cancelled, "stale epoch must not reach Win32");
    auto bad = first; ++bad.identity.process_id;
    bad.lease = std::make_shared<WindowLease>(); bad.lease->id = bad.id; bad.lease->identity = bad.identity;
    require(undrained.submit(bad) == SubmitStatus::Accepted, "identity mismatch setup");
    require(take_result(undrained).status == ActuationStatus::Cancelled, "PID mismatch must cancel");
    first.lease->valid.store(false);
    require(undrained.submit(first) == SubmitStatus::Accepted, "revoked lease setup");
    require(take_result(undrained).status == ActuationStatus::Cancelled, "revoked generation must cancel");
    auto valid = command(88, fixture.identity(0), {100, 100, 600, 410}, 90);
    PostMessageW(valid.identity.hwnd, state_message, SW_SHOWMINNOACTIVE, 0);
    wait_for([&] { return IsIconic(valid.identity.hwnd) != FALSE; }, "minimize own fixture");
    require(undrained.submit(valid) == SubmitStatus::Accepted, "minimized request setup");
    require(take_result(undrained).status == ActuationStatus::Cancelled, "minimized fixture must not be moved");
    PostMessageW(valid.identity.hwnd, state_message, SW_SHOWNOACTIVATE, 0);
    wait_for([&] { return IsIconic(valid.identity.hwnd) == FALSE; }, "restore own fixture");
    undrained.stop();
    require(undrained.submit(first) == SubmitStatus::Stopped, "stop rejects new commands");
}

void test_stop_results(FixtureProcess& fixture, HWND control) {
    WindowActuator actuator(control, 3, {fixture_only_set_pos}); actuator.set_session(1);
    block_next = true; entered = false; release_gate = false;
    struct Release { ~Release() { release_gate = true; } } release;
    require(actuator.submit(command(1, fixture.identity(0), {100, 100, 600, 410}, 1)) == SubmitStatus::Accepted,
            "stop in-flight setup");
    wait_for([] { return entered.load(); }, "stop gate");
    require(actuator.submit(command(2, fixture.identity(3), {100, 100, 600, 410}, 2)) == SubmitStatus::Accepted,
            "stop queued setup");
    // stop joins in-flight work; another test-owned thread releases the gate.
    std::jthread releaser([] { Sleep(50); release_gate = true; });
    actuator.stop();
    unsigned results = 0, cancelled = 0;
    ActuationResult result;
    while (actuator.pop_result(result)) { ++results; cancelled += result.status == ActuationStatus::Cancelled; }
    require(results == 2 && cancelled >= 1, "stop must preserve a result for every accepted queued/in-flight request");
}

void test_delayed_ui(FixtureProcess& fixture, HWND control) {
    WindowActuator actuator(control, 4, {fixture_only_set_pos}); actuator.set_session(1);
    InterlockedExchange(&fixture.view->delay_state, 0);
    PostMessageW(fixture.identity(3).hwnd, delay_message, 1000, 0);
    wait_for([&] { return InterlockedCompareExchange(&fixture.view->delay_state, 0, 0) == 1; }, "delay did not start");
    const auto target = command(9, fixture.identity(3), {130, 130, 670, 460}, 1);
    const auto start = GetTickCount64();
    require(actuator.submit(target) == SubmitStatus::Accepted, "delayed submit");
    const auto result = take_result(actuator);
    require(result.status == ActuationStatus::Submitted && GetTickCount64() - start < 600 &&
            InterlockedCompareExchange(&fixture.view->delay_state, 0, 0) == 1,
            "async submission must not wait for delayed fixture UI processing");
    wait_for([&] { return InterlockedCompareExchange(&fixture.view->delay_state, 0, 0) == 2; }, "delay did not end");
    wait_for([&] { RECT rect{}; return GetWindowRect(target.identity.hwnd, &rect) && rect.left == 130 && rect.bottom == 460; },
             "posted command was not eventually applied");
}

void test_native_controller(FixtureProcess& fixture, MonitorContext& monitor) {
    const auto state = *monitor.state().snapshot;
    const int width = static_cast<int>(std::min<std::int64_t>(1000, state.work_area.width() - 40));
    const int height = static_cast<int>(std::min<std::int64_t>(850, state.work_area.height() - 40));
    require(width >= 600 && height >= 650, "native fixture test needs a work area of at least 640x690 physical pixels");
    const LayoutContext context{state.monitor,
        {state.work_area.left + 20, state.work_area.top + 20,
         state.work_area.left + 20 + width, state.work_area.top + 20 + height}, state.dpi,
        monitor.state().revision, {10, 10, 600}};
    WindowActuator actuator(monitor.control_hwnd(), 16, {fixture_only_set_pos});
    LayoutController controller(actuator, [](std::wstring_view text) {
        std::fwprintf(stderr, L"%.*ls\n", static_cast<int>(text.size()), text.data());
    });
    WindowGeometryReader reader;
    const auto start = GetTickCount64();
    bool saw_minimum = false;
    while (GetTickCount64() - start < 9000) {
        pump();
        std::vector<LayoutInput> inputs;
        for (std::size_t i = 0; i < 4; ++i) {
            auto geometry = reader.read(fixture.identity(i));
            if (!geometry.ok()) { break; }
            inputs.push_back({i + 1, *geometry.snapshot, {}});
        }
        if (inputs.size() == 4) { controller.update(context, inputs); }
        controller.tick(GetTickCount64());
        saw_minimum |= controller.refinements_used() != 0;
        if (controller.status() == ControllerStatus::Settled) { break; }
        require(controller.status() == ControllerStatus::Applying || controller.status() == ControllerStatus::Paused,
                "native layout suspended unexpectedly");
        Sleep(20);
    }
    require(controller.status() == ControllerStatus::Settled && saw_minimum,
            "native min-size/normal/grid fixtures must converge with a learned minimum");
    require(controller.refinements_used() <= 3, "native cascade exceeded stack budget");
    require(controller.targets()[2].phase == TargetPhase::Constrained,
            "native grid fixture must exercise smaller-size settlement");
    const auto before = call_count.load();
    for (int i = 0; i < 10; ++i) { pump(); controller.tick(GetTickCount64()); Sleep(10); }
    require(call_count.load() == before, "settled layout repeated commands");
}
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--fixture") {
        const auto mapping = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(std::stoull(argv[2])));
        const auto ready = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(std::stoull(argv[3])));
        return fixture_main(mapping, ready);
    }
    try {
        FixtureProcess fixture; allowed = &fixture;
        MonitorContext monitor;
        require(monitor.select_for_window(fixture.identity(0).hwnd), "fixture monitor context");
        test_queue_protocol(fixture, monitor.control_hwnd());
        test_stop_results(fixture, monitor.control_hwnd());
        test_delayed_ui(fixture, monitor.control_hwnd());
        test_native_controller(fixture, monitor);
        require(!invalid_call.load(), "a command escaped the fixture process/HWND allowlist or used unsafe flags");
        std::puts("actuator and native controller tests passed (only spawned child fixture HWNDs moved)");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
