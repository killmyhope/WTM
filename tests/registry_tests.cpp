#include "twm/window_registry.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

twm::WindowObservation observation(twm::WindowDisposition disposition,
                                  DWORD pid = 100,
                                  std::wstring title = L"Test") {
    return {{reinterpret_cast<HWND>(std::uintptr_t{42}), pid, 200},
            disposition, std::move(title), L"reason"};
}

void transitions() {
    using D = twm::WindowDisposition;
    using N = twm::NotificationKind;
    std::vector<twm::RegistryNotification> log;
    twm::WindowRegistry registry([&](const auto& event) { log.push_back(event); });
    const auto observe = [&](const auto& item) { registry.observe(item, 1000); };

    observe(observation(D::Ignored));
    observe(observation(D::Inactive));
    observe(observation(D::Pending));
    require(log.empty() && registry.size() == 0,
            "unqualified windows must not enter the log or registry");

    observe(observation(D::Eligible));
    observe(observation(D::Eligible));
    observe(observation(D::Eligible, 100, L"Renamed"));
    require(log.size() == 1 && log[0].kind == N::Found,
            "duplicate observations and title changes must not duplicate discovery");

    observe(observation(D::Pending));
    require(log.size() == 1 && registry.size() == 1,
            "a query failure must not close or hide a known window");
    observe(observation(D::Inactive, 100, L"Renamed"));
    observe(observation(D::Inactive, 100, L"Renamed"));
    require(log.size() == 2 && log.back().kind == N::Hidden,
            "hide/minimize must be logged once, not as destruction");
    require(log.back().title == L"Renamed", "retain the latest known title");
    observe(observation(D::Eligible));
    require(log.size() == 3 && log.back().kind == N::Found,
            "restoring visibility must rediscover the window");
    require(log.front().generation == log.back().generation,
            "visibility changes must preserve lifetime identity");

    observe(observation(D::Ignored));
    require(log.back().kind == N::Excluded && registry.size() == 1,
            "a style change must exclude without destroying the record");
    observe(observation(D::Missing));
    observe(observation(D::Missing));
    require(log.size() == 5 && log.back().kind == N::Closed && registry.size() == 0,
            "destruction must remove even an inactive record exactly once");
}

void handle_reuse() {
    using D = twm::WindowDisposition;
    using N = twm::NotificationKind;
    std::vector<twm::RegistryNotification> log;
    twm::WindowRegistry registry([&](const auto& event) { log.push_back(event); });
    const auto observe = [&](const auto& item) { registry.observe(item, 1000); };
    observe(observation(D::Eligible, 100, L"Old"));
    observe(observation(D::Pending, 101, L"Unknown"));
    require(log.size() == 1, "uncertain identity must not destroy known state");
    observe(observation(D::Eligible, 101, L"New"));
    require(log.size() == 3 && log[1].kind == N::Closed && log[2].kind == N::Found,
            "reused HWND with new identity must close the old lifetime first");
    require(log[0].generation != log[2].generation,
            "new lifetime must receive a new generation");
    require(log[1].title == L"Old" && log[2].title == L"New",
            "identity reuse must not mix window titles");
    observe(observation(D::Missing));
    observe(observation(D::Eligible, 101));
    require(log.back().generation != log[2].generation,
            "reappearance after destruction needs a new generation");
}

void delayed_destroy_and_same_thread_reuse() {
    using D = twm::WindowDisposition;
    using N = twm::NotificationKind;
    std::vector<twm::RegistryNotification> log;
    twm::WindowRegistry registry([&](const auto& event) { log.push_back(event); });
    const auto window = observation(D::Eligible);
    registry.observe(window, 1000);
    registry.on_destroy(window.identity.hwnd, 900);
    registry.on_destroy(window.identity.hwnd, 1000);
    require(log.size() == 1, "stale snapshot events must not close a new window");
    registry.on_destroy(window.identity.hwnd, 1100);
    registry.observe(window, 1200);
    require(log.size() == 3 && log[1].kind == N::Closed && log[2].kind == N::Found,
            "DESTROY must end a lifetime even when HWND/PID/TID are reused");
    require(log[0].generation != log[2].generation, "reuse needs a new generation");
    registry.on_destroy(window.identity.hwnd, 1100);
    require(log.size() == 3, "duplicate delayed DESTROY must not close replacement");
}

void hidden_window_rename() {
    using D = twm::WindowDisposition;
    std::vector<twm::RegistryNotification> log;
    twm::WindowRegistry registry([&](const auto& event) { log.push_back(event); });
    registry.observe(observation(D::Eligible, 100, L"Visible title"), 1000);
    registry.observe(observation(D::Inactive, 100, L"Hidden title"), 1100);
    require(log.back().title == L"Hidden title",
            "hide must report the title from the current successful observation");
    registry.observe(observation(D::Inactive, 100, L"Renamed while hidden"), 1200);
    registry.observe(observation(D::Pending, 100, L"Unreliable title"), 1300);
    require(log.size() == 2, "hidden rename must not create a visibility transition");
    registry.observe(observation(D::Missing), 1400);
    require(log.back().title == L"Renamed while hidden",
            "closing a hidden window must use its latest observed title");
}

void managed_lifecycle_integration() {
    using D = twm::WindowDisposition;
    twm::WindowRegistry registry([](const auto&) {});
    auto first = observation(D::Eligible);
    auto second = observation(D::Eligible);
    second.identity.hwnd = reinterpret_cast<HWND>(std::uintptr_t{43});
    registry.observe(first, 1000);
    registry.observe(second, 1001);
    const auto initial = registry.managed_windows().layout_windows();
    require(initial.size() == 2 && initial[0].id < initial[1].id, "candidate admission order");
    first.disposition = D::Pending;
    registry.observe(first, 1100);
    const auto pending = registry.managed_windows().layout_windows();
    require(pending.size() == 1 && pending[0].id == initial[1].id,
            "registry Pending must suspend candidate without fake hide/close");
    first.disposition = D::Eligible;
    registry.observe(first, 1200);
    require(registry.managed_windows().layout_windows()[0].id == initial[0].id,
            "candidate recovery must retain order");
    first.disposition = D::Missing;
    registry.observe(first, 1300);
    require(registry.managed_windows().find(initial[0].id) == nullptr, "destroy must remove candidate");
    first.disposition = D::Eligible;
    first.identity.process_id = 101;
    registry.observe(first, 1400);
    const auto replacement = registry.managed_windows().layout_windows();
    require(replacement.size() == 2 && replacement[0].id == initial[1].id &&
            replacement[1].id > initial[1].id, "reused HWND must append a new candidate life");
}

void interactive_move_bridge() {
    using D = twm::WindowDisposition;
    twm::WindowRegistry registry([](const auto&) {});
    const auto window = observation(D::Eligible);
    registry.observe(window, 1000);
    const auto id = registry.managed_windows().layout_windows()[0].id;
    registry.on_interactive_move(window.identity.hwnd, true, 900);
    require(registry.managed_windows().layout_windows().size() == 1, "stale interactive event");
    registry.on_interactive_move(window.identity.hwnd, true, 1100);
    require(registry.managed_windows().layout_windows().empty(), "move start must suspend commands");
    registry.observe(window, 1150);
    require(registry.managed_windows().layout_windows().empty(), "rescan must not reset interactive state");
    registry.on_interactive_move(window.identity.hwnd, false, 1200);
    registry.observe(window, 1300);
    require(registry.managed_windows().find(id)->mode == twm::PlacementMode::Floating &&
            registry.managed_windows().layout_windows().empty(), "move end must remain floating across rescan");
}

} // namespace

int main() {
    try {
        transitions();
        handle_reuse();
        delayed_destroy_and_same_thread_reuse();
        hidden_window_rename();
        managed_lifecycle_integration();
        interactive_move_bridge();
        std::cout << "Registry transition tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
