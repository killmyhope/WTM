#include "twm/managed_windows.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace twm;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::vector<WindowId> ids(const ManagedWindows& model) {
    std::vector<WindowId> result;
    for (const auto& window : model.layout_windows()) { result.push_back(window.id); }
    return result;
}

void stable_order_and_lifetimes() {
    ManagedWindows model;
    require(!model.admit(0), "zero ID must be rejected");
    require(model.admit(1) && model.admit(2) && model.admit(3), "admission failed");
    require(!model.admit(2), "duplicate admission must preserve state/order");
    require(ids(model) == std::vector<WindowId>{1, 2, 3}, "initial order");
    require(model.set_availability(1, CandidateAvailability::Inactive), "hide");
    require(ids(model) == std::vector<WindowId>{2, 3}, "hidden window must leave active layout");
    require(model.set_availability(2, CandidateAvailability::Pending), "pending");
    require(ids(model) == std::vector<WindowId>{3}, "pending must forbid new placements");
    require(model.set_availability(1, CandidateAvailability::Ready) &&
            model.set_availability(2, CandidateAvailability::Ready), "restore");
    require(ids(model) == std::vector<WindowId>{1, 2, 3}, "restore changed stable slots");
    const auto snapshot = model.layout_windows();
    require(model.remove(2) && !model.remove(2), "duplicate removal");
    require(!model.admit(2) && !model.toggle_floating(2) && !model.promote_to_master(2),
            "stale lifetime must not resurrect/mutate");
    require(model.admit(4), "new lifetime admission");
    require(ids(model) == std::vector<WindowId>{1, 3, 4}, "new life must append");
    require(snapshot.size() == 3 && snapshot[1].id == 2, "snapshot must own its data");
}

void floating_and_master() {
    ManagedWindows model;
    require(model.admit(1) && model.admit(2) && model.admit(3), "admission");
    require(model.toggle_floating(2) && !model.admit(2), "floating duplicate admission");
    require(model.set_availability(2, CandidateAvailability::Inactive) &&
            model.set_availability(2, CandidateAvailability::Ready), "floating rescan");
    require(ids(model) == std::vector<WindowId>{1, 3}, "rescan must not undo user floating");
    require(model.toggle_floating(2), "return to tiling");
    require(ids(model) == std::vector<WindowId>{1, 2, 3}, "return must retain position");
    require(model.promote_to_master(3), "swap master");
    require(ids(model) == std::vector<WindowId>{3, 2, 1}, "swap must exchange positions");
    require(model.promote_to_master(3) && ids(model).front() == 3, "current master no-op");
    require(model.toggle_floating(3), "float earlier than master");
    require(model.promote_to_master(3) && ids(model).front() == 3,
            "floating before first tile must become master");
    require(model.toggle_floating(1), "float later than master");
    require(model.promote_to_master(1) && ids(model).front() == 1,
            "floating after master must be promoted and retiled");
    for (const auto id : {1u, 2u, 3u}) {
        require(model.set_availability(id, CandidateAvailability::Inactive), "deactivate all");
    }
    require(!model.promote_to_master(2) && !model.toggle_floating(2), "inactive hotkey target");
    require(model.set_availability(2, CandidateAvailability::Ready) &&
            model.promote_to_master(2) && ids(model).front() == 2, "single eligible master");
}

void moves_and_constraints() {
    ManagedWindows model;
    require(model.admit(10) && model.admit(11), "admission");
    require(model.begin_interactive_move(10), "move start");
    require(ids(model) == std::vector<WindowId>{11}, "interactive window must be suspended");
    require(!model.toggle_floating(10) && !model.promote_to_master(10), "hotkey during move");
    require(model.end_interactive_move(10) && !model.end_interactive_move(10), "move end");
    require(model.find(10)->mode == PlacementMode::Floating, "manual move should persist floating");
    require(model.toggle_floating(10), "explicit return");
    SizeConstraints constraints;
    constraints.height.minimum = 400;
    require(model.set_constraints(11, constraints), "set min");
    require(model.layout_windows()[1].constraints.height.minimum == 400, "constraint snapshot");
    constraints.height.maximum = 300;
    require(!model.set_constraints(11, constraints), "inconsistent constraints must be rejected");
    require(!model.find(11)->constraints.height.maximum, "failed update changed valid constraints");
    require(model.set_constraints(11, {}), "context invalidation should allow clearing constraints");
    require(!model.set_constraints(9, {}) && !model.begin_interactive_move(9), "unknown ID mutation");
    require(model.remove(10) && !model.admit(10), "removed ID cannot be readmitted");
    require(model.remove(11) && model.admit(12), "monotonic ID must survive empty model");
}

} // namespace

int main() {
    try {
        stable_order_and_lifetimes();
        floating_and_master();
        moves_and_constraints();
        std::cout << "Managed window ordering and policy tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
