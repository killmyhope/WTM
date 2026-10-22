#include "twm/control_window.hpp"

#include <algorithm>
#include <climits>
#include <exception>

namespace twm {
namespace {

constexpr wchar_t control_class[] = L"Twm.ControlWindow.Stage3.v1";

[[nodiscard]] Rect from_native(const RECT& value) noexcept {
    return {value.left, value.top, value.right, value.bottom};
}

[[nodiscard]] DWORD api_error() noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

// Один физический пиксель отделяет control HWND от каждой грани rcWork.
// Поэтому даже MonitorFromWindow для очень маленького окна однозначен.
[[nodiscard]] RectConversion fit_inside(Rect suggested, Rect work) noexcept {
    if (!valid_rectangle(work) || work.width() <= 2 || work.height() <= 2) {
        return {GeometryError::InvalidRectangle, {}};
    }
    return fit_control_rectangle(suggested, work);
}

} // namespace

ControlWindow::ControlWindow() noexcept : owner_thread_id_(GetCurrentThreadId()) {}

ControlWindow::~ControlWindow() noexcept {
    if (hwnd_ != nullptr) {
        // DestroyWindow чужого потока не разрешён Win32. Продолжать уничтожение
        // C++ объекта с живым GWLP_USERDATA означало бы use-after-free.
        if (!on_owner_thread()) {
            std::terminate();
        }
        destroying_ = true;
        if (!DestroyWindow(hwnd_)) {
            std::terminate();
        }
    }
}

bool ControlWindow::supported_dpi_context() noexcept {
    return AreDpiAwarenessContextsEqual(
        GetThreadDpiAwarenessContext(),
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

bool ControlWindow::on_owner_thread() const noexcept {
    return GetCurrentThreadId() == owner_thread_id_;
}

void ControlWindow::notify(ControlChange change) noexcept {
    notifications_.changes |= static_cast<std::uint32_t>(change);
}

void ControlWindow::record_error(DWORD error) noexcept {
    last_error_ = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
    notifications_.error = last_error_;
    notify(ControlChange::Error);
}

ControlNotifications ControlWindow::take_notifications() noexcept {
    if (!on_owner_thread()) {
        return {static_cast<std::uint32_t>(ControlChange::Error), ERROR_INVALID_THREAD_ID};
    }
    const ControlNotifications result = notifications_;
    notifications_ = {};
    return result;
}

bool ControlWindow::create(Rect position) noexcept {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        record_error(api_error());
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &ControlWindow::window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = control_class;
    if (RegisterClassExW(&window_class) == 0) {
        const DWORD error = GetLastError();
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (error != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(instance, control_class, &existing) ||
            existing.lpfnWndProc != &ControlWindow::window_proc ||
            existing.hInstance != instance) {
            record_error(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            return false;
        }
    }
    // Класс живёт до завершения процесса: несколько MonitorContext не должны
    // снимать его регистрацию друг у друга. HWND при этом строго RAII.
    const HWND created = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, control_class, L"TWM Control",
        WS_POPUP, position.left, position.top,
        static_cast<int>(position.width()), static_cast<int>(position.height()),
        nullptr, nullptr, instance, this);
    if (created == nullptr) {
        record_error(api_error());
        return false;
    }
    hwnd_ = created;
    return true;
}

bool ControlWindow::place(Rect position) noexcept {
    if (!valid_rectangle(position) || position.width() > INT_MAX ||
        position.height() > INT_MAX) {
        record_error(ERROR_INVALID_DATA);
        return false;
    }
    if (!SetWindowPos(hwnd_, nullptr, position.left, position.top,
                      static_cast<int>(position.width()),
                      static_cast<int>(position.height()),
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)) {
        record_error(api_error());
        return false;
    }
    return true;
}

bool ControlWindow::apply_pending_dpi() noexcept {
    if (placing_) {
        return true;
    }
    // Вложенный WM_DPICHANGED обновляет pending_suggested_, не входит повторно
    // в SetWindowPos. Лимит защищает message loop при нестабильной topology.
    constexpr unsigned maximum_passes = 4;
    for (unsigned pass = 0; pending_dpi_ && pass < maximum_passes; ++pass) {
        const Rect suggested = pending_suggested_;
        pending_dpi_ = false;
        const auto fitted = fit_inside(suggested, work_area_);
        if (!fitted.ok()) {
            record_error(ERROR_INVALID_DATA);
            return false;
        }
        placing_ = true;
        const bool success = place(fitted.value);
        placing_ = false;
        if (!success) {
            return false;
        }
    }
    if (pending_dpi_) {
        // Последний suggested сохраняется: следующий refresh попробует снова.
        record_error(ERROR_RETRY);
        return false;
    }
    return true;
}

bool ControlWindow::pin(HMONITOR monitor, Rect work_area) noexcept {
    last_error_ = ERROR_SUCCESS;
    if (!on_owner_thread()) {
        record_error(ERROR_INVALID_THREAD_ID);
        return false;
    }
    if (!supported_dpi_context()) {
        record_error(ERROR_NOT_SUPPORTED);
        return false;
    }
    if (placing_) {
        record_error(ERROR_BUSY);
        return false;
    }
    if (monitor == nullptr || !valid_rectangle(work_area)) {
        record_error(ERROR_INVALID_PARAMETER);
        return false;
    }

    Rect current{};
    if (hwnd_ != nullptr) {
        RECT native{};
        if (!GetWindowRect(hwnd_, &native)) {
            record_error(api_error());
            return false;
        }
        current = from_native(native);
    } else {
        // Расчёт в 64 битах допускает отрицательное начало virtual screen.
        const auto width = std::min<std::int64_t>(16, work_area.width());
        const auto height = std::min<std::int64_t>(16, work_area.height());
        current = {work_area.left, work_area.top,
                   static_cast<std::int32_t>(std::int64_t{work_area.left} + width),
                   static_cast<std::int32_t>(std::int64_t{work_area.top} + height)};
    }
    const auto fitted = fit_inside(current, work_area);
    if (!fitted.ok()) {
        record_error(ERROR_INVALID_DATA);
        return false;
    }

    monitor_ = monitor;
    work_area_ = work_area;
    placing_ = true;
    const bool success = hwnd_ == nullptr ? create(fitted.value)
        : (current == fitted.value || place(fitted.value));
    placing_ = false;
    if (!success || !apply_pending_dpi()) {
        return false;
    }

    RECT actual{};
    if (!GetWindowRect(hwnd_, &actual)) {
        record_error(api_error());
        return false;
    }
    const Rect observed = from_native(actual);
    if (!valid_rectangle(observed) || observed.left <= work_area.left ||
        observed.top <= work_area.top || observed.right >= work_area.right ||
        observed.bottom >= work_area.bottom ||
        MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONULL) != monitor_) {
        record_error(ERROR_INVALID_MONITOR_HANDLE);
        return false;
    }
    return true;
}

LRESULT CALLBACK ControlWindow::window_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* self = reinterpret_cast<ControlWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        if (creation == nullptr || creation->lpCreateParams == nullptr) {
            return FALSE;
        }
        self = static_cast<ControlWindow*>(creation->lpCreateParams);
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            return FALSE;
        }
        self->hwnd_ = hwnd;
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_DPICHANGED:
        self->notify(ControlChange::Dpi);
        if (lparam == 0) {
            self->record_error(ERROR_INVALID_PARAMETER);
            self->notify(ControlChange::Error);
            return 0;
        }
        // Указатель действителен только в этом сообщении: сохраняем значение.
        self->pending_suggested_ = from_native(*reinterpret_cast<const RECT*>(lparam));
        self->pending_dpi_ = true;
        if (!self->apply_pending_dpi()) {
            self->notify(ControlChange::Error);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        self->notify(ControlChange::Display);
        return 0;
    case WM_SETTINGCHANGE:
        // SPI_SETWORKAREA обязателен; прочие settings консервативно инвалидируют
        // снимок, т.к. taskbar и пользовательские оболочки бывают неоднородны.
        self->notify(ControlChange::Settings);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->pending_dpi_ = false;
        if (!self->destroying_) {
            self->record_error(ERROR_INVALID_WINDOW_HANDLE);
            self->notify(ControlChange::Error);
        }
        break;
    default:
        break;
    }
    // Путь WndProc не выделяет память и не вызывает код, бросающий исключения.
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace twm
