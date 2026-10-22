#pragma once

#include "twm/control_window.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace twm {

enum class MonitorStatus {
    Ready,
    Unselected,
    Unavailable,
    ApiError,
    DpiUnavailable,
    ControlWindowMismatch,
    UnsupportedDpiContext
};

struct MonitorSnapshot {
    HMONITOR monitor{};
    Rect monitor_area{};
    Rect work_area{};
    UINT dpi{};
    bool operator==(const MonitorSnapshot&) const = default;
};

struct MonitorState {
    MonitorStatus status = MonitorStatus::Unselected;
    std::optional<MonitorSnapshot> snapshot;
    DWORD error{};
    // Меняется при изменении состояния или системном уведомлении; может
    // служить поколением кэша frame insets и физического размера DIP gaps.
    std::uint64_t revision{};
};

// Единственный владелец — поток message loop. Выбор дисплея явный: refresh
// никогда не переключает пропавший монитор на primary/foreground автоматически.
class MonitorContext final {
public:
    MonitorContext() noexcept = default;
    // Вызывающий передаёт foreground HWND явно. nullptr/невалидный HWND
    // означает явный выбор primary fallback, а не повторное чтение foreground.
    [[nodiscard]] bool select_for_window(HWND window) noexcept;
    [[nodiscard]] bool select_monitor(HMONITOR monitor) noexcept;
    [[nodiscard]] bool refresh() noexcept;
    [[nodiscard]] bool process_notifications() noexcept;
    [[nodiscard]] const MonitorState& state() const noexcept { return state_; }
    [[nodiscard]] HWND control_hwnd() const noexcept { return control_.hwnd(); }

private:
    [[nodiscard]] bool fail(MonitorStatus status, DWORD error) noexcept;
    void advance_revision() noexcept;

    ControlWindow control_;
    HMONITOR selected_{};
    std::array<wchar_t, CCHDEVICENAME> selected_device_{};
    MonitorState state_{};
};

} // namespace twm
