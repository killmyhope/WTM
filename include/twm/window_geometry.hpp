#pragma once

#include "twm/geometry.hpp"
#include "twm/window_classifier.hpp"

#include <dwmapi.h>

#include <optional>

namespace twm {

enum class GeometryReadStatus {
    Ready,
    Missing,
    Inactive,
    Changed,
    ApiError,
    InvalidGeometry,
    InvalidPlacement,
    UnsupportedDpiContext
};

struct WindowGeometrySnapshot {
    WindowIdentity identity;
    Rect outer;
    Rect visible;
    FrameInsets insets;
    // rcNormalPosition сохраняет workspace-координаты WINDOWPLACEMENT: их нельзя
    // сравнивать с virtual-screen rcWork или передавать напрямую в SetWindowPos.
    WINDOWPLACEMENT placement{};
    UINT dpi{};
    DWORD style{};
    DWORD extended_style{};
    ULONGLONG captured_at{};
};

struct GeometryReadResult {
    GeometryReadStatus status = GeometryReadStatus::ApiError;
    std::optional<WindowGeometrySnapshot> snapshot;
    DWORD error = ERROR_SUCCESS;
    HRESULT hresult = S_OK;

    [[nodiscard]] bool ok() const noexcept {
        return status == GeometryReadStatus::Ready && snapshot.has_value();
    }
};

// Узкая точка подмены для детерминированных тестов ошибок и гонок. По умолчанию
// вызываются настоящие Win32 API; наблюдаемое окно никогда не перемещается.
struct GeometryReadApi {
    decltype(&::GetWindowRect) get_window_rect = &::GetWindowRect;
    decltype(&::DwmGetWindowAttribute) dwm_get_window_attribute = &::DwmGetWindowAttribute;
    decltype(&::GetWindowPlacement) get_window_placement = &::GetWindowPlacement;
};

class WindowGeometryReader {
public:
    explicit WindowGeometryReader(GeometryReadApi api = {}) noexcept : api_(api) {}

    // Вызывающий поток обязан быть PMv2, HWND — классифицированным top-level.
    // Повторное чтение отсекает замеченные переходы, но не делает DWM/user32
    // атомарными и не исключает ABA/HWND reuse тем же потоком. Контроллер также
    // проверяет поколение записи и инвалидирует снимки по WinEvents.
    [[nodiscard]] GeometryReadResult read(WindowIdentity expected) const noexcept;

private:
    GeometryReadApi api_;
};

} // namespace twm
