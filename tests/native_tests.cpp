#include "twm/window_classifier.hpp"
#include "twm/win_event_source.hpp"

#include <dwmapi.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"TwmNativeTestWindow";
constexpr UINT fixture_window_message = WM_APP + 70;
using Clock = std::chrono::steady_clock;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[noreturn]] void win32_failure(const char* operation) {
    const DWORD error = GetLastError();
    throw std::runtime_error(std::string(operation) + ": Win32 error " +
                             std::to_string(error));
}

class WindowClass final {
public:
    WindowClass() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        window_class.lpszClassName = window_class_name;
        if (RegisterClassExW(&window_class) == 0) {
            win32_failure("RegisterClassExW");
        }
    }

    ~WindowClass() {
        UnregisterClassW(window_class_name, GetModuleHandleW(nullptr));
    }

    WindowClass(const WindowClass&) = delete;
    WindowClass& operator=(const WindowClass&) = delete;
};

class Window final {
public:
    explicit Window(const wchar_t* title = L"TWM test",
                    const DWORD extended_style = 0,
                    const DWORD style = WS_OVERLAPPEDWINDOW,
                    HWND parent = nullptr) {
        hwnd_ = CreateWindowExW(
            extended_style, window_class_name, title, style,
            20, 20, 160, 100, parent, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (hwnd_ == nullptr) {
            win32_failure("CreateWindowExW");
        }
    }

    ~Window() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
        }
    }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] HWND get() const noexcept { return hwnd_; }

    void show() const {
        // Tests never activate their windows or reposition other applications.
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        require(IsWindowVisible(hwnd_) != FALSE, "Test window did not become visible");
    }

    void hide() const {
        ShowWindow(hwnd_, SW_HIDE);
        require(IsWindowVisible(hwnd_) == FALSE, "Test window did not become hidden");
    }

    void destroy() {
        if (!DestroyWindow(hwnd_)) {
            win32_failure("DestroyWindow");
        }
        hwnd_ = nullptr;
    }

private:
    HWND hwnd_{};
};

void expect_disposition(const twm::WindowClassifier& classifier, HWND hwnd,
                        const twm::WindowDisposition expected, const char* description) {
    const auto observation = classifier.inspect(hwnd);
    if (observation.disposition != expected) {
        throw std::runtime_error(std::string(description) +
                                 ": unexpected classification (" +
                                 std::to_string(static_cast<int>(observation.disposition)) + ")");
    }
}

void test_classifier() {
    BOOL composition_enabled = FALSE;
    require(SUCCEEDED(DwmIsCompositionEnabled(&composition_enabled)) && composition_enabled,
            "Native tests require an interactive Windows desktop with DWM enabled");

    using Disposition = twm::WindowDisposition;
    const twm::WindowClassifier classifier{0};
    Window ordinary;
    expect_disposition(classifier, ordinary.get(), Disposition::Inactive, "Hidden window");
    ordinary.show();
    expect_disposition(classifier, ordinary.get(), Disposition::Eligible, "Ordinary window");

    const auto observation = classifier.inspect(ordinary.get());
    require(observation.title == L"TWM test", "Title was not read correctly");
    require(observation.identity.process_id == GetCurrentProcessId(), "Wrong process identity");
    require(observation.identity.thread_id == GetCurrentThreadId(), "Wrong thread identity");

    const twm::WindowClassifier exclude_own_process{GetCurrentProcessId()};
    expect_disposition(exclude_own_process, ordinary.get(), Disposition::Ignored,
                       "Own process exclusion");

    Window untitled{L""};
    untitled.show();
    expect_disposition(classifier, untitled.get(), Disposition::Eligible, "Empty title window");

    Window tool{L"Tool", WS_EX_TOOLWINDOW};
    tool.show();
    expect_disposition(classifier, tool.get(), Disposition::Ignored, "Tool window");

    Window no_activate{L"No activate", WS_EX_NOACTIVATE};
    no_activate.show();
    expect_disposition(classifier, no_activate.get(), Disposition::Ignored, "No-activate window");

    Window owned{L"Owned", 0, WS_OVERLAPPEDWINDOW, ordinary.get()};
    owned.show();
    expect_disposition(classifier, owned.get(), Disposition::Ignored, "Owned window");

    Window child{L"Child", 0, WS_CHILD | WS_VISIBLE, ordinary.get()};
    expect_disposition(classifier, child.get(), Disposition::Ignored, "Child window");

    Window message_only{L"Message only", 0, WS_POPUP, HWND_MESSAGE};
    expect_disposition(classifier, message_only.get(), Disposition::Ignored,
                       "Message-only window");

    Window cloaked{L"Cloaked"};
    cloaked.show();
    const BOOL cloak = TRUE;
    require(SUCCEEDED(DwmSetWindowAttribute(cloaked.get(), DWMWA_CLOAK, &cloak, sizeof(cloak))),
            "Could not cloak the test's own window with DWM");
    require(SUCCEEDED(DwmFlush()), "DwmFlush failed while testing a cloaked window");
    expect_disposition(classifier, cloaked.get(), Disposition::Inactive, "DWM cloaked window");

    Window destroyed;
    const HWND old_handle = destroyed.get();
    destroyed.destroy();
    expect_disposition(classifier, old_handle, Disposition::Missing, "Destroyed window");
    expect_disposition(classifier, nullptr, Disposition::Missing, "Null handle");
    std::puts("PASS: native window classification");
}

int run_event_fixture(const DWORD parent_thread_id, const bool burst) {
    WindowClass window_class;
    Window window{L"TWM cross-process fixture"};
    if (!PostThreadMessageW(parent_thread_id, fixture_window_message,
                           reinterpret_cast<WPARAM>(window.get()),
                           static_cast<LPARAM>(GetCurrentProcessId()))) {
        win32_failure("PostThreadMessageW fixture announcement");
    }

    if (burst) {
        // Переполняем очередь наблюдателя событиями только собственного HWND.
        // Родитель доставляет callbacks, но намеренно не дренирует очередь.
        for (unsigned index = 0; index < 8192; ++index) {
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, window.get(), OBJID_WINDOW, 0);
        }
        return 0;
    }

    // Delays keep each lifecycle stage observable; the whole fixture lasts < 1 second.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    window.show();
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, window.get(), OBJID_CLIENT, 0);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, window.get(), OBJID_WINDOW, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    window.hide();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    window.show();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    window.destroy();
    return 0;
}

class FixtureProcess final {
public:
    explicit FixtureProcess(const DWORD parent_thread_id,
                            const wchar_t* mode = L"--event-fixture") {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                                static_cast<DWORD>(executable.size()));
        if (length == 0) {
            win32_failure("GetModuleFileNameW");
        }
        require(length < executable.size(), "Executable path was truncated");
        executable.resize(length);
        std::wstring command_line = L"\"" + executable + L"\" " + mode + L" " +
                                    std::to_wstring(parent_thread_id);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information_)) {
            win32_failure("CreateProcessW fixture");
        }
        CloseHandle(information_.hThread);
        information_.hThread = nullptr;
    }

    ~FixtureProcess() {
        if (information_.hProcess != nullptr) {
            // Only the child created by this test can be terminated on timeout/failure.
            if (WaitForSingleObject(information_.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(information_.hProcess, 1);
                WaitForSingleObject(information_.hProcess, 1000);
            }
            CloseHandle(information_.hProcess);
        }
    }

    FixtureProcess(const FixtureProcess&) = delete;
    FixtureProcess& operator=(const FixtureProcess&) = delete;

    [[nodiscard]] HANDLE handle() const noexcept { return information_.hProcess; }
    [[nodiscard]] DWORD process_id() const noexcept { return information_.dwProcessId; }
    [[nodiscard]] DWORD thread_id() const noexcept { return information_.dwThreadId; }

private:
    PROCESS_INFORMATION information_{};
};

void test_win_events() {
    twm::WinEventSource source;
    source.start();
    source.start(); // Repeated start must not install duplicate hooks.

    {
        Window own{L"TWM skipped process fixture"};
        own.show();
        own.hide();
    }

    FixtureProcess fixture{GetCurrentThreadId()};
    HWND fixture_window = nullptr;
    std::vector<twm::WindowEvent> events;
    const auto deadline = Clock::now() + std::chrono::seconds{7};
    auto drain_deadline = deadline;
    bool exited = false;

    while (Clock::now() < deadline && (!exited || Clock::now() < drain_deadline)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == fixture_window_message && message.hwnd == nullptr) {
                require(static_cast<DWORD>(message.lParam) == fixture.process_id(),
                        "Fixture announcement came from the wrong process");
                fixture_window = reinterpret_cast<HWND>(message.wParam);
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        twm::WindowEvent event{};
        while (source.pop(event)) {
            require(event.thread_id != GetCurrentThreadId(),
                    "WINEVENT_SKIPOWNPROCESS leaked a test-process event");
            if (event.thread_id == fixture.thread_id()) {
                if (event.event >= EVENT_OBJECT_CREATE &&
                    event.event <= EVENT_OBJECT_END) {
                    require(event.object_id == OBJID_WINDOW && event.child_id == 0,
                            "Non-window accessibility events leaked through the filter");
                }
                events.push_back(event);
            }
        }
        require(!source.take_rescan_request(), "WinEvent queue unexpectedly lost events");

        if (!exited) {
            const DWORD wait = WaitForSingleObject(fixture.handle(), 0);
            if (wait == WAIT_FAILED) {
                win32_failure("WaitForSingleObject fixture");
            }
            if (wait == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                if (!GetExitCodeProcess(fixture.handle(), &exit_code)) {
                    win32_failure("GetExitCodeProcess");
                }
                require(exit_code == 0, "WinEvent fixture exited unsuccessfully");
                exited = true;
                drain_deadline = Clock::now() + std::chrono::milliseconds{200};
            }
        }

        // Pumping this thread is required for OUTOFCONTEXT WinEvent delivery.
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) {
            win32_failure("MsgWaitForMultipleObjectsEx");
        }
    }

    require(exited, "WinEvent fixture timed out");
    require(fixture_window != nullptr, "Fixture did not announce its window handle");
    constexpr std::array<DWORD, 5> expected{
        EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
        EVENT_OBJECT_SHOW, EVENT_OBJECT_DESTROY};
    std::size_t next = 0;
    unsigned creates = 0;
    unsigned destroys = 0;
    for (const auto& event : events) {
        if (event.hwnd != fixture_window || event.object_id != OBJID_WINDOW || event.child_id != 0) {
            continue;
        }
        creates += event.event == EVENT_OBJECT_CREATE ? 1u : 0u;
        destroys += event.event == EVENT_OBJECT_DESTROY ? 1u : 0u;
        if (next < expected.size() && event.event == expected[next]) {
            ++next;
        }
    }
    require(next == expected.size(), "Missing or reordered CREATE/SHOW/HIDE/SHOW/DESTROY events");
    require(creates == 1 && destroys == 1, "Duplicate lifecycle events / duplicate subscriptions");
    source.stop();
    source.stop();
    std::puts("PASS: cross-process WinEvent lifecycle and own-process exclusion");
}

void test_event_overflow() {
    twm::WinEventSource source;
    source.start();
    FixtureProcess fixture{GetCurrentThreadId(), L"--burst-fixture"};
    const auto deadline = Clock::now() + std::chrono::seconds{7};
    auto drain_deadline = deadline;
    bool exited = false;

    while (Clock::now() < deadline && (!exited || Clock::now() < drain_deadline)) {
        MSG message{};
        while (Clock::now() < deadline &&
               PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // pop() здесь намеренно не вызывается: имитируем отставание потребителя.
        if (!exited) {
            const DWORD wait = WaitForSingleObject(fixture.handle(), 0);
            if (wait == WAIT_FAILED) {
                win32_failure("WaitForSingleObject burst fixture");
            }
            if (wait == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                if (!GetExitCodeProcess(fixture.handle(), &exit_code)) {
                    win32_failure("GetExitCodeProcess burst fixture");
                }
                require(exit_code == 0, "Burst fixture exited unsuccessfully");
                exited = true;
                drain_deadline = Clock::now() + std::chrono::milliseconds{200};
            }
        }
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                       MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
            win32_failure("MsgWaitForMultipleObjectsEx burst fixture");
        }
    }
    require(exited, "Burst fixture timed out");
    require(source.take_rescan_request(), "Queue overflow must request reconciliation");
    require(!source.take_rescan_request(), "Reconciliation request must be consumable");
    unsigned received = 0;
    twm::WindowEvent event{};
    while (source.pop(event)) {
        ++received;
    }
    require(received == 4096, "Overflow must retain exactly the bounded queue capacity");
    source.stop();
    source.start();
    require(!source.pop(event) && !source.take_rescan_request(),
            "Restart must not retain old events or overflow state");
    source.stop();
    std::puts("PASS: bounded WinEvent queue overflow, rescan signal and restart");
}

} // namespace

int main(const int argc, char* argv[]) {
    try {
        if (argc == 3 && (std::strcmp(argv[1], "--event-fixture") == 0 ||
                          std::strcmp(argv[1], "--burst-fixture") == 0)) {
            DWORD parent_thread = 0;
            const char* const end = argv[2] + std::strlen(argv[2]);
            const auto parsed = std::from_chars(argv[2], end, parent_thread);
            require(parsed.ec == std::errc{} && parsed.ptr == end && parent_thread != 0,
                    "Invalid parent thread ID");
            return run_event_fixture(parent_thread,
                                     std::strcmp(argv[1], "--burst-fixture") == 0);
        }

        require(argc == 1, "Unexpected test arguments");
        WindowClass window_class;
        test_classifier();
        test_win_events();
        test_event_overflow();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
