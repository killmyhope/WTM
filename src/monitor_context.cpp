#include "twm/monitor_context.hpp"

#include <algorithm>
#include <cwchar>
#include <limits>

namespace twm {
namespace {

[[nodiscard]] Rect from_native(const RECT& value) noexcept {
    return {value.left, value.top, value.right, value.bottom};
}

[[nodiscard]] DWORD api_error() noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

[[nodiscard]] bool read_monitor(HMONITOR monitor, MONITORINFOEXW& info) noexcept {
    info = {};
    info.cbSize = sizeof(info);
    SetLastError(ERROR_SUCCESS);
    return GetMonitorInfoW(monitor, &info) != FALSE;
}

[[nodiscard]] bool valid_info(const MONITORINFOEXW& info) noexcept {
    const Rect monitor = from_native(info.rcMonitor);
    const Rect work = from_native(info.rcWork);
    return valid_rectangle(monitor) && valid_rectangle(work) &&
           contains_rectangle(monitor, work) && info.szDevice[0] != L'\0' &&
           std::find(std::begin(info.szDevice), std::end(info.szDevice), L'\0') !=
               std::end(info.szDevice);
}

[[nodiscard]] bool same_device(const wchar_t* left, const wchar_t* right) noexcept {
    return _wcsicmp(left, right) == 0;
}

[[nodiscard]] bool same_topology(
    const MONITORINFOEXW& before, const MONITORINFOEXW& after) noexcept {
    return same_device(before.szDevice, after.szDevice) &&
           from_native(before.rcMonitor) == from_native(after.rcMonitor) &&
           from_native(before.rcWork) == from_native(after.rcWork);
}

} // namespace

void MonitorContext::advance_revision() noexcept {
    // Нулевое поколение остаётся признаком отсутствия первого снимка.
    if (state_.revision != std::numeric_limits<std::uint64_t>::max()) {
        ++state_.revision;
    }
}

bool MonitorContext::fail(MonitorStatus status, DWORD error) noexcept {
    if (state_.status != status || state_.snapshot.has_value() || state_.error != error) {
        advance_revision();
    }
    state_.status = status;
    state_.snapshot.reset();
    state_.error = error;
    return false;
}

bool MonitorContext::select_for_window(HWND window) noexcept {
    if (!control_.on_owner_thread()) {
        return fail(MonitorStatus::ApiError, ERROR_INVALID_THREAD_ID);
    }
    HMONITOR monitor = window == nullptr ? nullptr :
        MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) {
        // Только явный выбор имеет право использовать primary fallback.
        const POINT origin{};
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    return select_monitor(monitor);
}

bool MonitorContext::select_monitor(HMONITOR monitor) noexcept {
    if (!control_.on_owner_thread()) {
        return fail(MonitorStatus::ApiError, ERROR_INVALID_THREAD_ID);
    }
    if (!ControlWindow::supported_dpi_context()) {
        return fail(MonitorStatus::UnsupportedDpiContext, ERROR_NOT_SUPPORTED);
    }
    if (monitor == nullptr) {
        selected_ = nullptr;
        selected_device_ = {};
        return fail(MonitorStatus::Unselected, ERROR_INVALID_MONITOR_HANDLE);
    }
    MONITORINFOEXW info{};
    if (!read_monitor(monitor, info)) {
        // Не сохраняем старый Ready snapshot после неудачной команды выбора.
        selected_ = monitor;
        selected_device_ = {};
        return fail(MonitorStatus::Unavailable, api_error());
    }
    if (!valid_info(info)) {
        selected_ = monitor;
        selected_device_ = {};
        return fail(MonitorStatus::ApiError, ERROR_INVALID_DATA);
    }
    const bool changed_selection = selected_ != monitor ||
        !same_device(selected_device_.data(), info.szDevice);
    selected_ = monitor;
    std::copy(std::begin(info.szDevice), std::end(info.szDevice), selected_device_.begin());
    // Если handle переиспользован для другого устройства, явный select
    // разрешает смену, но обязан инвалидировать прежние сведения о рамках.
    if (changed_selection) {
        advance_revision();
    }
    return refresh();
}

bool MonitorContext::refresh() noexcept {
    if (!control_.on_owner_thread()) {
        return fail(MonitorStatus::ApiError, ERROR_INVALID_THREAD_ID);
    }
    if (!ControlWindow::supported_dpi_context()) {
        return fail(MonitorStatus::UnsupportedDpiContext, ERROR_NOT_SUPPORTED);
    }
    if (selected_ == nullptr) {
        return fail(MonitorStatus::Unselected, ERROR_SUCCESS);
    }

    MONITORINFOEXW before{};
    if (!read_monitor(selected_, before)) {
        return fail(MonitorStatus::Unavailable, api_error());
    }
    if (!valid_info(before)) {
        return fail(MonitorStatus::ApiError, ERROR_INVALID_DATA);
    }
    if (selected_device_[0] == L'\0' ||
        !same_device(selected_device_.data(), before.szDevice)) {
        return fail(MonitorStatus::Unavailable, ERROR_DEVICE_NOT_CONNECTED);
    }

    if (!control_.pin(selected_, from_native(before.rcWork))) {
        const DWORD error = control_.last_error();
        return fail(error == ERROR_INVALID_MONITOR_HANDLE ?
                    MonitorStatus::ControlWindowMismatch : MonitorStatus::ApiError,
                    error);
    }
    const UINT dpi_before = GetDpiForWindow(control_.hwnd());
    if (dpi_before == 0) {
        return fail(MonitorStatus::DpiUnavailable, ERROR_INVALID_DATA);
    }

    MONITORINFOEXW after{};
    if (!read_monitor(selected_, after)) {
        return fail(MonitorStatus::Unavailable, api_error());
    }
    if (!valid_info(after)) {
        return fail(MonitorStatus::ApiError, ERROR_INVALID_DATA);
    }
    if (!same_device(selected_device_.data(), after.szDevice)) {
        return fail(MonitorStatus::Unavailable, ERROR_DEVICE_NOT_CONNECTED);
    }
    if (!same_topology(before, after)) {
        // Снимок до/после pin разошёлся. Не публикуем смешанные rcWork и DPI.
        return fail(MonitorStatus::Unavailable, ERROR_RETRY);
    }
    if (MonitorFromWindow(control_.hwnd(), MONITOR_DEFAULTTONULL) != selected_) {
        return fail(MonitorStatus::ControlWindowMismatch, ERROR_INVALID_MONITOR_HANDLE);
    }
    const UINT dpi_after = GetDpiForWindow(control_.hwnd());
    if (dpi_after == 0 || dpi_after != dpi_before) {
        return fail(MonitorStatus::DpiUnavailable, ERROR_RETRY);
    }

    const MonitorSnapshot snapshot{
        selected_, from_native(after.rcMonitor), from_native(after.rcWork), dpi_after};
    if (state_.status != MonitorStatus::Ready || state_.snapshot != snapshot ||
        state_.error != ERROR_SUCCESS) {
        advance_revision();
    }
    state_.status = MonitorStatus::Ready;
    state_.snapshot = snapshot;
    state_.error = ERROR_SUCCESS;
    return true;
}

bool MonitorContext::process_notifications() noexcept {
    if (!control_.on_owner_thread()) {
        return fail(MonitorStatus::ApiError, ERROR_INVALID_THREAD_ID);
    }
    if (!ControlWindow::supported_dpi_context()) {
        return fail(MonitorStatus::UnsupportedDpiContext, ERROR_NOT_SUPPORTED);
    }
    const auto notifications = control_.take_notifications();
    if (!notifications.any() && notifications.error == ERROR_SUCCESS) {
        return state_.status == MonitorStatus::Ready && state_.snapshot.has_value();
    }
    // Даже смена DPI туда-обратно между drains инвалидирует frame insets.
    // Реальное состояние перечитывается; synthetic wParam не является DPI API.
    advance_revision();
    if (notifications.error != ERROR_SUCCESS) {
        // Ошибку применения suggested rect нельзя маскировать Ready-снимком.
        // Более поздний явный/периодический refresh может восстановить контекст.
        return fail(MonitorStatus::ApiError, notifications.error);
    }
    return refresh();
}

} // namespace twm
