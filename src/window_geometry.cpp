#include "twm/window_geometry.hpp"

namespace twm {
namespace {

[[nodiscard]] Rect rectangle(const RECT value) noexcept {
    return {value.left, value.top, value.right, value.bottom};
}

[[nodiscard]] GeometryReadResult failure(
    const GeometryReadStatus status, const DWORD error = ERROR_SUCCESS,
    const HRESULT hresult = S_OK) noexcept {
    return {status, std::nullopt, error, hresult};
}

struct WindowState {
    WindowIdentity identity;
    bool visible{};
    bool iconic{};
    bool zoomed{};
    DWORD style{};
    DWORD extended_style{};
    UINT dpi{};
    DWORD cloaked{};
    HWND root{};
    HWND parent{};
    HWND owner{};

    bool operator==(const WindowState&) const = default;

    [[nodiscard]] bool active() const noexcept {
        return visible && !iconic && !zoomed && cloaked == 0 &&
            (style & WS_CHILD) == 0 &&
            (extended_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == 0 &&
            root == identity.hwnd && owner == nullptr &&
            (parent == nullptr || parent == GetDesktopWindow());
    }
};

[[nodiscard]] bool read_style(
    const HWND hwnd, const int index, DWORD& value, DWORD& error) noexcept {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR native = GetWindowLongPtrW(hwnd, index);
    error = GetLastError();
    if (native == 0 && error != ERROR_SUCCESS) {
        return false;
    }
    value = static_cast<DWORD>(native);
    return true;
}

// Ready здесь — внутренний маркер успеха: snapshot создаётся только после
// проверки всех границ состояния, placement и прямоугольников.
[[nodiscard]] GeometryReadResult read_state(
    const WindowIdentity expected, const GeometryReadApi& api,
    WindowState& value) noexcept {
    value.identity.hwnd = expected.hwnd;
    if (expected.hwnd == nullptr || !IsWindow(expected.hwnd)) {
        return failure(GeometryReadStatus::Missing);
    }

    SetLastError(ERROR_SUCCESS);
    value.identity.thread_id =
        GetWindowThreadProcessId(expected.hwnd, &value.identity.process_id);
    const DWORD identity_error = GetLastError();
    if (value.identity.thread_id == 0) {
        return failure(GeometryReadStatus::ApiError, identity_error);
    }
    if (value.identity != expected) {
        return failure(GeometryReadStatus::Changed);
    }

    value.visible = IsWindowVisible(expected.hwnd) != FALSE;
    value.iconic = IsIconic(expected.hwnd) != FALSE;
    value.zoomed = IsZoomed(expected.hwnd) != FALSE;
    DWORD error = ERROR_SUCCESS;
    if (!read_style(expected.hwnd, GWL_STYLE, value.style, error) ||
        !read_style(expected.hwnd, GWL_EXSTYLE, value.extended_style, error)) {
        return failure(GeometryReadStatus::ApiError, error);
    }
    value.root = GetAncestor(expected.hwnd, GA_ROOT);
    if (value.root == nullptr) {
        return failure(GeometryReadStatus::ApiError, GetLastError());
    }
    value.parent = GetAncestor(expected.hwnd, GA_PARENT);
    value.owner = GetWindow(expected.hwnd, GW_OWNER);
    value.dpi = GetDpiForWindow(expected.hwnd);
    if (value.dpi == 0) {
        // GetDpiForWindow документирует ноль, но не GetLastError.
        return failure(GeometryReadStatus::ApiError);
    }
    const HRESULT hr = api.dwm_get_window_attribute(
        expected.hwnd, DWMWA_CLOAKED, &value.cloaked, sizeof(value.cloaked));
    if (hr != S_OK) {
        return failure(GeometryReadStatus::ApiError, ERROR_SUCCESS, hr);
    }

    DWORD final_pid = 0;
    const DWORD final_tid = GetWindowThreadProcessId(expected.hwnd, &final_pid);
    if (final_tid != expected.thread_id || final_pid != expected.process_id) {
        return failure(GeometryReadStatus::Changed);
    }
    return failure(GeometryReadStatus::Ready);
}

struct Bounds {
    Rect outer;
    Rect visible;
    WINDOWPLACEMENT placement{};
};

template <typename Placement>
[[nodiscard]] bool same_optional_device(
    const Placement& left, const Placement& right) noexcept {
    // Некоторые новые SDK дополняют WINDOWPLACEMENT полем rcDevice.
    if constexpr (requires { left.rcDevice; right.rcDevice; }) {
        return rectangle(left.rcDevice) == rectangle(right.rcDevice);
    }
    return true;
}

[[nodiscard]] bool same_placement(
    const WINDOWPLACEMENT& left, const WINDOWPLACEMENT& right) noexcept {
    // Сравниваем поля, а не padding структуры; workspace-координаты остаются
    // в исходной системе координат и никуда не преобразуются.
    return left.length == right.length && left.flags == right.flags &&
        left.showCmd == right.showCmd &&
        left.ptMinPosition.x == right.ptMinPosition.x &&
        left.ptMinPosition.y == right.ptMinPosition.y &&
        left.ptMaxPosition.x == right.ptMaxPosition.x &&
        left.ptMaxPosition.y == right.ptMaxPosition.y &&
        rectangle(left.rcNormalPosition) == rectangle(right.rcNormalPosition) &&
        same_optional_device(left, right);
}

[[nodiscard]] GeometryReadResult read_bounds(
    const HWND hwnd, const GeometryReadApi& api, Bounds& bounds) noexcept {
    RECT outer{};
    if (!api.get_window_rect(hwnd, &outer)) {
        const DWORD error = GetLastError();
        return failure(GeometryReadStatus::ApiError, error);
    }
    bounds.outer = rectangle(outer);

    RECT visible{};
    const HRESULT hr = api.dwm_get_window_attribute(
        hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible));
    if (hr != S_OK) {
        return failure(GeometryReadStatus::ApiError, ERROR_SUCCESS, hr);
    }
    bounds.visible = rectangle(visible);

    bounds.placement = {};
    bounds.placement.length = sizeof(WINDOWPLACEMENT);
    if (!api.get_window_placement(hwnd, &bounds.placement)) {
        const DWORD error = GetLastError();
        return failure(GeometryReadStatus::ApiError, error);
    }
    if (bounds.placement.length != sizeof(WINDOWPLACEMENT) ||
        !valid_rectangle(rectangle(bounds.placement.rcNormalPosition))) {
        return failure(GeometryReadStatus::InvalidPlacement);
    }
    // Отступы нельзя выводить из специальных координат свёрнутого окна.
    // IsIconic проверяется на каждой границе состояния, но между ними есть гонки.
    if (bounds.placement.showCmd == SW_SHOWMINIMIZED ||
        bounds.placement.showCmd == SW_MINIMIZE ||
        bounds.placement.showCmd == SW_SHOWMINNOACTIVE ||
        bounds.placement.showCmd == SW_SHOWMAXIMIZED) {
        return failure(GeometryReadStatus::Changed);
    }
    // GetWindowPlacement нормализует состояние в NORMAL, MINIMIZED/MAXIMIZED.
    // Неизвестное значение нельзя превращать в готовый к управлению снимок.
    if (bounds.placement.showCmd != SW_SHOWNORMAL) {
        return failure(GeometryReadStatus::InvalidPlacement);
    }
    if (!valid_rectangle(bounds.outer) || !valid_rectangle(bounds.visible)) {
        return failure(GeometryReadStatus::InvalidGeometry);
    }
    return failure(GeometryReadStatus::Ready);
}

} // namespace

GeometryReadResult WindowGeometryReader::read(const WindowIdentity expected) const noexcept {
    if (!AreDpiAwarenessContextsEqual(
            GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return failure(GeometryReadStatus::UnsupportedDpiContext);
    }
    if (api_.get_window_rect == nullptr || api_.dwm_get_window_attribute == nullptr ||
        api_.get_window_placement == nullptr) {
        return failure(GeometryReadStatus::ApiError, ERROR_INVALID_PARAMETER, E_INVALIDARG);
    }

    WindowState first_state{};
    auto result = read_state(expected, api_, first_state);
    if (result.status != GeometryReadStatus::Ready) {
        return result;
    }
    if (!first_state.active()) {
        return failure(GeometryReadStatus::Inactive);
    }

    Bounds first{};
    result = read_bounds(expected.hwnd, api_, first);
    if (result.status != GeometryReadStatus::Ready) {
        return result;
    }
    WindowState middle_state{};
    result = read_state(expected, api_, middle_state);
    if (result.status != GeometryReadStatus::Ready) {
        return result;
    }
    if (first_state != middle_state) {
        return failure(GeometryReadStatus::Changed);
    }

    Bounds second{};
    result = read_bounds(expected.hwnd, api_, second);
    if (result.status != GeometryReadStatus::Ready) {
        return result;
    }
    WindowState final_state{};
    result = read_state(expected, api_, final_state);
    if (result.status != GeometryReadStatus::Ready) {
        return result;
    }
    if (first_state != final_state || first.outer != second.outer ||
        first.visible != second.visible || !same_placement(first.placement, second.placement)) {
        return failure(GeometryReadStatus::Changed);
    }

    const auto measured = measure_frame_insets(second.outer, second.visible);
    if (!measured.ok()) {
        return failure(GeometryReadStatus::InvalidGeometry);
    }
    WindowGeometrySnapshot snapshot{
        expected, second.outer, second.visible, measured.value, second.placement,
        final_state.dpi, final_state.style, final_state.extended_style, GetTickCount64()};
    return {GeometryReadStatus::Ready, snapshot, ERROR_SUCCESS, S_OK};
}

} // namespace twm
