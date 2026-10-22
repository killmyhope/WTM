#include "twm/monitor_context.hpp"

#include "twm/geometry.hpp"

#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

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

twm::Rect from_native(const RECT& rectangle) noexcept {
    return {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
}

struct MonitorEnumeration {
    std::vector<HMONITOR> monitors;
    std::exception_ptr error;
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) noexcept {
    auto& enumeration = *reinterpret_cast<MonitorEnumeration*>(parameter);
    try {
        enumeration.monitors.push_back(monitor);
        return TRUE;
    } catch (...) {
        enumeration.error = std::current_exception();
        return FALSE;
    }
}

std::vector<HMONITOR> enumerate_monitors() {
    MonitorEnumeration enumeration;
    const BOOL result = EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                                           reinterpret_cast<LPARAM>(&enumeration));
    if (enumeration.error) {
        std::rethrow_exception(enumeration.error);
    }
    if (!result) {
        win32_failure("EnumDisplayMonitors");
    }
    require(!enumeration.monitors.empty(), "Tests require an interactive desktop with a monitor");
    return enumeration.monitors;
}

// Dispatch only messages belonging to the test's hidden control window. No other
// application's HWND, input queue, focus, work area or display mode is changed.
void dispatch_control_messages(const HWND hwnd) {
    MSG message{};
    while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void expect_ready_and_pinned(const twm::MonitorContext& context,
                             const HMONITOR expected_monitor) {
    const auto& state = context.state();
    require(state.status == twm::MonitorStatus::Ready && state.snapshot.has_value(),
            "Monitor context is not ready");
    const auto& snapshot = *state.snapshot;
    require(snapshot.monitor == expected_monitor, "Selected monitor unexpectedly changed");
    require(twm::valid_rectangle(snapshot.monitor_area) &&
            twm::valid_rectangle(snapshot.work_area), "Invalid monitor rectangles");
    require(twm::contains_rectangle(snapshot.monitor_area, snapshot.work_area),
            "Work area is outside the monitor");

    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(expected_monitor, &information)) {
        win32_failure("GetMonitorInfoW");
    }
    require(snapshot.monitor_area == from_native(information.rcMonitor),
            "Monitor rectangle does not match Win32");
    require(snapshot.work_area == from_native(information.rcWork),
            "Work area does not match Win32");

    const HWND hwnd = context.control_hwnd();
    require(hwnd != nullptr && IsWindow(hwnd), "Control HWND is missing");
    require(!IsWindowVisible(hwnd), "Control window must stay hidden");
    require(GetForegroundWindow() != hwnd, "Hidden control window must not take focus");
    require(GetAncestor(hwnd, GA_ROOT) == hwnd && GetParent(hwnd) != HWND_MESSAGE,
            "Control HWND must be a top-level broadcast recipient");
    const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    require((style & WS_EX_NOACTIVATE) != 0, "Control window must have WS_EX_NOACTIVATE");
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(hwnd, &process_id);
    require(process_id == GetCurrentProcessId() && thread_id == GetCurrentThreadId(),
            "Control HWND must belong to the context thread");

    RECT control{};
    if (!GetWindowRect(hwnd, &control)) {
        win32_failure("GetWindowRect(control window)");
    }
    const auto control_rectangle = from_native(control);
    require(twm::valid_rectangle(control_rectangle) &&
            twm::contains_rectangle(snapshot.work_area, control_rectangle),
            "Control window is not wholly inside the selected work area");
    if (snapshot.work_area.width() > 2 && snapshot.work_area.height() > 2) {
        require(control_rectangle.left > snapshot.work_area.left &&
                control_rectangle.top > snapshot.work_area.top &&
                control_rectangle.right < snapshot.work_area.right &&
                control_rectangle.bottom < snapshot.work_area.bottom,
                "Control window must stay strictly inside work area boundaries");
    }
    require(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == expected_monitor,
            "Control window is assigned to a different monitor");
    require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(hwnd),
                                        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            "Control HWND must be Per-Monitor V2 aware");
    const UINT actual_dpi = GetDpiForWindow(hwnd);
    require(actual_dpi != 0 && snapshot.dpi == actual_dpi,
            "Snapshot DPI must come from the actual control HWND");
}

void test_monitor_selection_and_notifications() {
    const auto monitors = enumerate_monitors();
    twm::MonitorContext context;
    require(context.state().status == twm::MonitorStatus::Unselected &&
            !context.state().snapshot, "New context must be unselected");

    for (const HMONITOR monitor : monitors) {
        require(context.select_monitor(monitor), "Could not select a real monitor");
        dispatch_control_messages(context.control_hwnd());
        (void)context.process_notifications();
        expect_ready_and_pinned(context, monitor);

        // An observation with identical state must not invalidate geometry caches.
        const auto stable_revision = context.state().revision;
        require(context.refresh(), "Stable monitor refresh failed");
        require(context.state().revision == stable_revision,
                "Unchanged refresh must preserve the monitor revision");
        expect_ready_and_pinned(context, monitor);

        const auto work = context.state().snapshot->work_area;
        require(work.right <= std::numeric_limits<LONG>::max() - 32 &&
                work.bottom <= std::numeric_limits<LONG>::max() - 32,
                "Fixture monitor coordinates cannot represent an outside suggested rect");
        RECT suggested{work.right, work.bottom, work.right + 32, work.bottom + 32};
        const UINT actual_dpi = GetDpiForWindow(context.control_hwnd());
        const UINT synthetic_dpi = actual_dpi == 96 ? 144 : 96;
        const auto before_dpi = context.state().revision;
        SendMessageW(context.control_hwnd(), WM_DPICHANGED,
                     MAKELONG(synthetic_dpi, synthetic_dpi),
                     reinterpret_cast<LPARAM>(&suggested));
        // lParam is a synchronous borrowed pointer. The handler must copy it,
        // constrain placement to the selected work area and read actual HWND DPI.
        suggested = {};
        (void)context.process_notifications();
        expect_ready_and_pinned(context, monitor);
        require(context.state().revision > before_dpi,
                "DPI notification must invalidate the monitor revision");
        require(context.state().snapshot->dpi == GetDpiForWindow(context.control_hwnd()),
                "Forged WM_DPICHANGED wParam must not become authoritative DPI");

        SendMessageW(context.control_hwnd(), WM_DPICHANGED,
                     MAKELONG(actual_dpi, actual_dpi), 0);
        require(!context.process_notifications() && !context.state().snapshot &&
                context.state().status == twm::MonitorStatus::ApiError &&
                context.state().error == ERROR_INVALID_PARAMETER,
                "DPI handler error must invalidate the snapshot, not be silently recovered");
        require(context.refresh(), "A later explicit refresh should recover after a bad notification");
        expect_ready_and_pinned(context, monitor);

        const auto before_display = context.state().revision;
        SendMessageW(context.control_hwnd(), WM_DISPLAYCHANGE, 0, 0);
        (void)context.process_notifications();
        expect_ready_and_pinned(context, monitor);
        require(context.state().revision > before_display,
                "Display notification must invalidate cached geometry");

        const auto before_work = context.state().revision;
        SendMessageW(context.control_hwnd(), WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
        (void)context.process_notifications();
        expect_ready_and_pinned(context, monitor);
        require(context.state().revision > before_work,
                "Work-area notification must invalidate cached geometry");

        dispatch_control_messages(context.control_hwnd());
        (void)context.process_notifications();
        const auto settled_revision = context.state().revision;
        require(context.refresh(), "Settled monitor refresh failed");
        require(context.state().revision == settled_revision,
                "DPI correction must settle without a revision feedback loop");
    }

    // An invalid pinned monitor is not silently replaced during reconciliation.
    const auto invalid_monitor = reinterpret_cast<HMONITOR>(static_cast<UINT_PTR>(1));
    require(!context.select_monitor(invalid_monitor), "Invalid HMONITOR was accepted");
    require(context.state().status != twm::MonitorStatus::Ready &&
            !context.state().snapshot, "Failed selection must discard the prior snapshot");
    require(!context.refresh(), "Invalid pinned monitor must not fall back during refresh");
    require(context.state().status != twm::MonitorStatus::Ready &&
            !context.state().snapshot, "Refresh silently selected another monitor");

    const POINT origin{0, 0};
    const HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    require(primary != nullptr, "Primary monitor is unavailable");
    const auto invalid_window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
    require(context.select_for_window(invalid_window), "Explicit primary fallback failed");
    expect_ready_and_pinned(context, primary);

    // The caller passes GetForegroundWindow explicitly. nullptr deliberately
    // selects primary, so reconciliation never reads foreground implicitly.
    require(context.select_for_window(nullptr), "Null HWND primary selection failed");
    expect_ready_and_pinned(context, primary);

    const HWND foreground = GetForegroundWindow();
    if (foreground != nullptr) {
        const HMONITOR foreground_monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
        require(context.select_for_window(foreground), "Explicit foreground HWND selection failed");
        expect_ready_and_pinned(context, foreground_monitor == nullptr ? primary : foreground_monitor);
    }
    std::printf("PASS: monitor pinning, actual DPI and notifications on %zu monitor(s)\n",
                monitors.size());
}

class ThreadDpiContext final {
public:
    explicit ThreadDpiContext(DPI_AWARENESS_CONTEXT context)
        : previous_(SetThreadDpiAwarenessContext(context)) {
        if (previous_ == nullptr) {
            win32_failure("SetThreadDpiAwarenessContext(test thread)");
        }
    }
    ~ThreadDpiContext() { SetThreadDpiAwarenessContext(previous_); }
    ThreadDpiContext(const ThreadDpiContext&) = delete;
    ThreadDpiContext& operator=(const ThreadDpiContext&) = delete;
private:
    DPI_AWARENESS_CONTEXT previous_;
};

void test_unsupported_thread_context() {
    std::exception_ptr worker_error;
    std::thread worker([&worker_error] {
        try {
            ThreadDpiContext unaware{DPI_AWARENESS_CONTEXT_UNAWARE};
            twm::MonitorContext context;
            require(!context.select_for_window(nullptr),
                    "DPI-unaware context must not report a usable monitor");
            require(context.state().status == twm::MonitorStatus::UnsupportedDpiContext &&
                    !context.state().snapshot,
                    "DPI-unaware thread must expose UnsupportedDpiContext");
            require(context.control_hwnd() == nullptr,
                    "Unsupported context must not create a DPI-unaware control window");
        } catch (...) {
            worker_error = std::current_exception();
        }
    });
    worker.join();
    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
    require(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            "Worker test changed the main thread DPI awareness");
    std::puts("PASS: unsupported DPI context is rejected without a stale snapshot");
}

} // namespace

int main() {
    try {
        require(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
                "Monitor tests require the embedded Per-Monitor V2 manifest");
        test_monitor_selection_and_notifications();
        test_unsupported_thread_context();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
