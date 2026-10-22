#pragma once

#include "twm/window_classifier.hpp"
#include "twm/managed_windows.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace twm {

enum class NotificationKind { Found, Hidden, Excluded, Closed };

struct RegistryNotification {
    NotificationKind kind;
    WindowIdentity identity;
    std::uint64_t generation;
    std::wstring title;
    std::wstring reason;
};

// Один владелец состояния. Методы вызываются вне WinEvent callback.
class WindowRegistry {
public:
    using Sink = std::function<void(const RegistryNotification&)>;

    explicit WindowRegistry(Sink sink);
    void observe(const WindowObservation& observation, std::uint64_t observed_at);
    void on_destroy(HWND hwnd, std::uint64_t event_at);
    void on_interactive_move(HWND hwnd, bool starting, std::uint64_t event_at);
    [[nodiscard]] std::vector<HWND> tracked_handles() const;
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] const ManagedWindows& managed_windows() const noexcept { return managed_; }
    [[nodiscard]] std::optional<WindowIdentity> find_identity(WindowId id) const noexcept;

private:
    struct Record {
        WindowIdentity identity;
        std::uint64_t generation;
        std::wstring title;
        bool eligible;
        std::uint64_t first_observed_at;
    };

    void emit(NotificationKind kind, const Record& record,
              const std::wstring& reason = {});

    Sink sink_;
    std::unordered_map<HWND, Record> records_;
    std::uint64_t next_generation_ = 1;
    ManagedWindows managed_;
};

} // namespace twm
