#pragma once

#include "twm/geometry.hpp"

#include <Windows.h>

#include <cstdint>

namespace twm {

enum class ControlChange : std::uint32_t {
    None = 0,
    Dpi = 1,
    Display = 2,
    Settings = 4,
    Error = 8
};

struct ControlNotifications {
    std::uint32_t changes{};
    DWORD error{};
    [[nodiscard]] bool any() const noexcept { return changes != 0; }
    [[nodiscard]] bool has(ControlChange change) const noexcept {
        return (changes & static_cast<std::uint32_t>(change)) != 0;
    }
};

// Скрытое top-level окно принадлежит создавшему объект потоку. Все методы,
// включая деструктор, вызываются на этом потоке с message loop и PMv2 context.
// Объект не меняет DPI awareness процесса или потока и не активирует окно.
class ControlWindow final {
public:
    ControlWindow() noexcept;
    ~ControlWindow() noexcept;
    ControlWindow(const ControlWindow&) = delete;
    ControlWindow& operator=(const ControlWindow&) = delete;
    ControlWindow(ControlWindow&&) = delete;
    ControlWindow& operator=(ControlWindow&&) = delete;

    [[nodiscard]] static bool supported_dpi_context() noexcept;
    [[nodiscard]] bool on_owner_thread() const noexcept;
    // Создаёт окно при необходимости и закрепляет целиком внутри work_area,
    // с отступом от каждой грани. Ошибка не означает смену выбранного монитора.
    [[nodiscard]] bool pin(HMONITOR monitor, Rect work_area) noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] DWORD last_error() const noexcept { return last_error_; }
    [[nodiscard]] ControlNotifications take_notifications() noexcept;

private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM) noexcept;
    [[nodiscard]] bool create(Rect position) noexcept;
    [[nodiscard]] bool place(Rect position) noexcept;
    [[nodiscard]] bool apply_pending_dpi() noexcept;
    void record_error(DWORD error) noexcept;
    void notify(ControlChange change) noexcept;

    const DWORD owner_thread_id_;
    HWND hwnd_{};
    HMONITOR monitor_{};
    Rect work_area_{};
    Rect pending_suggested_{};
    bool pending_dpi_{};
    bool placing_{};
    bool destroying_{};
    DWORD last_error_{};
    ControlNotifications notifications_{};
};

} // namespace twm
