#include "twm/window_actuator.hpp"
#include "twm/control_window.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <stdexcept>

namespace twm {

WindowActuator::WindowActuator(HWND notify_window, std::size_t capacity, ActuatorApi api)
    : notify_window_(notify_window), capacity_(capacity), api_(api) {
    if (capacity == 0 || api.set_window_pos == nullptr) {
        throw std::invalid_argument("WindowActuator requires capacity and SetWindowPos");
    }
    requests_.reserve(capacity);
    results_.reserve(capacity);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

WindowActuator::~WindowActuator() { stop(); }

void WindowActuator::set_session(std::uint64_t epoch) noexcept {
    session_.store(epoch, std::memory_order_release);
}

SubmitStatus WindowActuator::submit(const WindowCommand& command) {
    std::lock_guard lock(mutex_);
    if (stopped_) { return SubmitStatus::Stopped; }
    for (auto& pending : requests_) {
        if (pending.id == command.id) {
            pending = command;
            return SubmitStatus::Coalesced;
        }
    }
    if (outstanding_ == capacity_) { return SubmitStatus::Backpressure; }
    requests_.push_back(command);
    ++outstanding_;
    ready_.notify_one();
    return SubmitStatus::Accepted;
}

bool WindowActuator::pop_result(ActuationResult& result) {
    std::lock_guard lock(mutex_);
    if (results_.empty()) { return false; }
    result = std::move(results_.front());
    results_.erase(results_.begin());
    --outstanding_;
    return true;
}

void WindowActuator::stop() noexcept {
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        for (auto& command : requests_) {
            results_.push_back({std::move(command), ActuationStatus::Cancelled});
            notify = true;
        }
        requests_.clear();
    }
    worker_.request_stop();
    ready_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
    if (notify && notify_window_) { PostMessageW(notify_window_, wake_message, 0, 0); }
}

ActuationResult WindowActuator::execute(const WindowCommand& command,
                                       std::stop_token stop) const noexcept {
    ActuationResult result{command};
    const auto permitted = [&] {
        return !stop.stop_requested() && command.session_epoch != 0 &&
            command.session_epoch == session_.load(std::memory_order_acquire) &&
            command.lease && command.lease->valid.load(std::memory_order_acquire) &&
            command.lease->id == command.id && command.id != 0 &&
            command.lease->identity == command.identity;
    };
    if (!permitted()) { return result; }
    if (!valid_rectangle(command.target_outer) || !ControlWindow::supported_dpi_context()) {
        result.status = ActuationStatus::Failed;
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    const HWND hwnd = command.identity.hwnd;
    DWORD pid{};
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (tid == 0 || pid != command.identity.process_id || tid != command.identity.thread_id ||
        !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) ||
        GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return result;
    }
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    DWORD cloaked{};
    if ((style & (WS_CHILD | WS_MINIMIZE | WS_MAXIMIZE)) != 0 ||
        (extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0 ||
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) != S_OK ||
        cloaked != 0 || (command.monitor &&
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != command.monitor) ||
        (command.dpi && GetDpiForWindow(hwnd) != command.dpi)) {
        return result;
    }
    // Recheck the lease/epoch and identity last, after potentially slow queries.
    DWORD final_pid{};
    if (!permitted() || GetWindowThreadProcessId(hwnd, &final_pid) != tid ||
        final_pid != pid || IsIconic(hwnd) || IsZoomed(hwnd) || !IsWindowVisible(hwnd)) {
        return result;
    }
    result.issued_at = GetTickCount64();
    SetLastError(ERROR_SUCCESS);
    const BOOL sent = api_.set_window_pos(hwnd, nullptr, command.target_outer.left,
        command.target_outer.top, static_cast<int>(command.target_outer.width()),
        static_cast<int>(command.target_outer.height()), SWP_NOACTIVATE | SWP_NOZORDER |
        SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    result.error = sent ? ERROR_SUCCESS : GetLastError();
    result.submitted_at = GetTickCount64();
    result.status = sent ? ActuationStatus::Submitted : ActuationStatus::Failed;
    return result;
}

void WindowActuator::run(std::stop_token stop) {
    // Awareness is inherited by new threads from the PMv2 executable manifest.
    for (;;) {
        WindowCommand command;
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait(lock, stop, [this] { return !requests_.empty(); })) { return; }
            if (stop.stop_requested()) { return; }
            command = std::move(requests_.front());
            requests_.erase(requests_.begin());
        }
        auto result = execute(command, stop);
        {
            std::lock_guard lock(mutex_);
            // outstanding_ includes this in-flight request: capacity is reserved.
            results_.push_back(std::move(result));
        }
        if (notify_window_) { PostMessageW(notify_window_, wake_message, 0, 0); }
    }
}

} // namespace twm
