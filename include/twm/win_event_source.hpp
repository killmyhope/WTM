#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

namespace twm {

struct WindowEvent {
    DWORD event{};
    HWND hwnd{};
    LONG object_id{};
    LONG child_id{};
    DWORD thread_id{};
    DWORD timestamp{};
};

// Объект принадлежит потоку, в котором был создан. Этот поток должен
// выполнять message loop; все методы вызываются только из этого потока.
class WinEventSource final {
public:
    static constexpr UINT wake_message = WM_APP + 1;

    WinEventSource();
    ~WinEventSource() noexcept;

    WinEventSource(const WinEventSource&) = delete;
    WinEventSource& operator=(const WinEventSource&) = delete;
    WinEventSource(WinEventSource&&) = delete;
    WinEventSource& operator=(WinEventSource&&) = delete;

    // Повторный start активного источника безопасен. При частичной ошибке
    // регистрации уже установленные hooks снимаются до выхода с исключением.
    void start();
    void stop() noexcept;

    [[nodiscard]] bool pop(WindowEvent& event) noexcept;
    [[nodiscard]] bool take_rescan_request() noexcept;

private:
    static constexpr std::size_t queue_capacity = 4096;
    static constexpr std::size_t subscription_count = 6;

    static void CALLBACK callback(
        HWINEVENTHOOK hook,
        DWORD event,
        HWND hwnd,
        LONG object_id,
        LONG child_id,
        DWORD event_thread,
        DWORD timestamp) noexcept;

    void enqueue(const WindowEvent& event) noexcept;
    void request_wake() noexcept;

    static thread_local WinEventSource* active_source_;

    const DWORD owner_thread_id_;
    std::array<HWINEVENTHOOK, subscription_count> hooks_{};
    std::array<WindowEvent, queue_capacity> queue_{};
    std::size_t read_position_{};
    std::size_t write_position_{};
    std::size_t queue_size_{};
    bool started_{};
    bool wake_pending_{};
    bool rescan_requested_{};
};

} // namespace twm
