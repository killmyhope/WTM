#pragma once

#include "twm/layout.hpp"

#include <span>

namespace twm {

enum class PlacementMode { Tiled, Floating };
enum class CandidateAvailability { Ready, Inactive, Pending, Ineligible };

struct ManagedWindow {
    WindowId id;
    PlacementMode mode = PlacementMode::Tiled;
    CandidateAvailability availability = CandidateAvailability::Ready;
    bool interactive_move = false;
    SizeConstraints constraints;

    [[nodiscard]] bool participates() const noexcept {
        return mode == PlacementMode::Tiled &&
               availability == CandidateAvailability::Ready && !interactive_move;
    }
};

// Один поток-владелец. ID выдаёт lifecycle-реестр монотонно, при первом admission.
// Модель не знает HWND и не восстанавливает закрытые жизни из stale observations.
class ManagedWindows {
public:
    [[nodiscard]] bool admit(WindowId id);
    [[nodiscard]] bool remove(WindowId id);
    [[nodiscard]] bool set_availability(WindowId id, CandidateAvailability availability);
    [[nodiscard]] bool set_constraints(WindowId id, const SizeConstraints& constraints);
    [[nodiscard]] bool toggle_floating(WindowId id);
    [[nodiscard]] bool promote_to_master(WindowId id);
    [[nodiscard]] bool begin_interactive_move(WindowId id);
    [[nodiscard]] bool end_interactive_move(WindowId id);

    [[nodiscard]] const ManagedWindow* find(WindowId id) const noexcept;
    // Представление действительно до следующей модификации модели.
    [[nodiscard]] std::span<const ManagedWindow> ordered() const noexcept { return windows_; }
    [[nodiscard]] std::vector<LayoutWindow> layout_windows() const;

private:
    [[nodiscard]] ManagedWindow* mutable_find(WindowId id) noexcept;
    std::vector<ManagedWindow> windows_;
    WindowId highest_admitted_id_ = 0;
};

} // namespace twm
