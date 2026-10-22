#include "twm/managed_windows.hpp"

#include <algorithm>

namespace twm {

ManagedWindow* ManagedWindows::mutable_find(WindowId id) noexcept {
    const auto it = std::find_if(windows_.begin(), windows_.end(),
                                [id](const auto& window) { return window.id == id; });
    return it == windows_.end() ? nullptr : &*it;
}

const ManagedWindow* ManagedWindows::find(WindowId id) const noexcept {
    const auto it = std::find_if(windows_.begin(), windows_.end(),
                                [id](const auto& window) { return window.id == id; });
    return it == windows_.end() ? nullptr : &*it;
}

bool ManagedWindows::admit(WindowId id) {
    if (id == 0 || id <= highest_admitted_id_) {
        return false;
    }
    windows_.push_back({id, PlacementMode::Tiled, CandidateAvailability::Ready, false, {}});
    highest_admitted_id_ = id;
    return true;
}

bool ManagedWindows::remove(WindowId id) {
    return std::erase_if(windows_, [id](const auto& window) { return window.id == id; }) != 0;
}

bool ManagedWindows::set_availability(WindowId id, CandidateAvailability availability) {
    if (auto* window = mutable_find(id)) {
        window->availability = availability;
        return true;
    }
    return false;
}

bool ManagedWindows::set_constraints(WindowId id, const SizeConstraints& constraints) {
    auto* window = mutable_find(id);
    if (window == nullptr || !valid_constraints(constraints)) {
        return false;
    }
    window->constraints = constraints;
    return true;
}

bool ManagedWindows::toggle_floating(WindowId id) {
    auto* window = mutable_find(id);
    if (window == nullptr || window->availability != CandidateAvailability::Ready ||
        window->interactive_move) {
        return false;
    }
    window->mode = window->mode == PlacementMode::Tiled
        ? PlacementMode::Floating : PlacementMode::Tiled;
    return true;
}

bool ManagedWindows::promote_to_master(WindowId id) {
    const auto selected = std::find_if(windows_.begin(), windows_.end(),
                                      [id](const auto& window) { return window.id == id; });
    if (selected == windows_.end() || selected->availability != CandidateAvailability::Ready ||
        selected->interactive_move) {
        return false;
    }
    const auto master = std::find_if(windows_.begin(), windows_.end(),
                                    [](const auto& window) { return window.participates(); });
    selected->mode = PlacementMode::Tiled;
    if (master == windows_.end()) {
        // Неактивные записи сохраняют места, active master сейчас единственный.
        return true;
    }
    // Floating до первого tiled уже станет первым участником после включения;
    // обмен с более поздним master, наоборот, вернул бы старый master вперёд.
    if (selected > master) {
        std::iter_swap(selected, master);
    }
    return true;
}

bool ManagedWindows::begin_interactive_move(WindowId id) {
    auto* window = mutable_find(id);
    if (window == nullptr || window->availability != CandidateAvailability::Ready) {
        return false;
    }
    window->interactive_move = true;
    return true;
}

bool ManagedWindows::end_interactive_move(WindowId id) {
    auto* window = mutable_find(id);
    if (window == nullptr || !window->interactive_move) {
        return false;
    }
    window->interactive_move = false;
    window->mode = PlacementMode::Floating;
    return true;
}

std::vector<LayoutWindow> ManagedWindows::layout_windows() const {
    std::vector<LayoutWindow> result;
    result.reserve(windows_.size());
    for (const auto& window : windows_) {
        if (window.participates()) {
            result.push_back({window.id, window.constraints});
        }
    }
    return result;
}

} // namespace twm
