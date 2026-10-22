#include "twm/layout_controller.hpp"

#include <cstdio>
#include <deque>
#include <stdexcept>

namespace {
using namespace twm;
void require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }

// This transport NEVER invokes Win32; all identities in these tests are null.
class FakeTransport final : public ActuatorTransport {
public:
    std::uint64_t epoch{};
    bool pressure{};
    std::vector<WindowCommand> commands;
    std::deque<ActuationResult> replies;
    void set_session(std::uint64_t value) noexcept override { epoch = value; }
    SubmitStatus submit(const WindowCommand& command) override {
        if (pressure) { return SubmitStatus::Backpressure; }
        commands.push_back(command);
        return SubmitStatus::Accepted;
    }
    bool pop_result(ActuationResult& output) override {
        if (replies.empty()) { return false; }
        output = replies.front(); replies.pop_front(); return true;
    }
    void reply_all(ULONGLONG at) {
        for (std::size_t i = answered; i < commands.size(); ++i) {
            replies.push_back({commands[i], ActuationStatus::Submitted, 0, at, at});
        }
        answered = commands.size();
    }
private:
    std::size_t answered{};
};

LayoutContext context() { return {nullptr, {0, 0, 1000, 900}, 120, 1, {0, 0, 600}}; }
std::vector<LayoutInput> inputs(std::size_t count) {
    std::vector<LayoutInput> result;
    for (std::size_t i = 0; i < count; ++i) {
        LayoutInput input;
        input.id = i + 1;
        input.geometry.outer = input.geometry.visible = {100, 100, 600, 500};
        input.geometry.dpi = 120;
        input.geometry.captured_at = 1;
        result.push_back(input);
    }
    return result;
}
void observe_targets(LayoutController& controller, std::vector<LayoutInput>& windows, ULONGLONG at) {
    const auto targets = controller.targets();
    for (std::size_t i = 0; i < windows.size(); ++i) {
        windows[i].geometry.outer = targets[i].outer;
        windows[i].geometry.visible = targets[i].visible;
        windows[i].geometry.captured_at = at;
    }
    controller.update(context(), windows);
}

void test_race_and_tolerance() {
    FakeTransport transport;
    LayoutController controller(transport);
    auto windows = inputs(1);
    controller.update(context(), windows);
    controller.tick(10);
    const auto canonical = controller.targets()[0].outer;
    // LOCATIONCHANGE precedes worker reply; queue waiting time is unbounded.
    observe_targets(controller, windows, 20);
    controller.tick(9000);
    require(transport.commands.size() == 1 && controller.status() == ControllerStatus::Applying,
            "queue waiting must not start the 500ms timer");
    transport.reply_all(15);
    controller.tick(9001);
    require(controller.status() == ControllerStatus::Settled, "early observation must acknowledge without another event");
    for (ULONGLONG time = 9002; time < 9012; ++time) {
        windows[0].geometry.outer = windows[0].geometry.visible = canonical;
        --windows[0].geometry.outer.right;
        --windows[0].geometry.visible.right;
        windows[0].geometry.captured_at = time;
        controller.update(context(), windows); controller.tick(time);
    }
    require(controller.targets()[0].outer == canonical && transport.commands.size() == 1,
            "observations must neither drift the canonical target nor resend it");

    FakeTransport second;
    LayoutController tolerant(second);
    auto shifted = inputs(1);
    tolerant.update(context(), shifted); tolerant.tick(1);
    observe_targets(tolerant, shifted, 3);
    --shifted[0].geometry.outer.right; --shifted[0].geometry.visible.right;
    shifted[0].geometry.captured_at = 4;
    tolerant.update(context(), shifted); second.reply_all(2); tolerant.tick(4);
    require(tolerant.status() == ControllerStatus::Settled, "one pixel per edge tolerance");
}

void test_backpressure_and_stale() {
    FakeTransport transport; transport.pressure = true;
    LayoutController controller(transport);
    auto windows = inputs(2);
    controller.update(context(), windows); controller.tick(1); controller.tick(9999);
    require(transport.commands.empty(), "backpressure must retain goals without a timeout");
    transport.pressure = false; controller.tick(10000);
    require(transport.commands.size() == 2, "backpressure recovery");
    const auto old = transport.commands;
    controller.pause();
    controller.update(context(), windows); controller.tick(10001);
    for (const auto& command : old) { transport.replies.push_back({command, ActuationStatus::Submitted, 0, 1, 1}); }
    controller.tick(10002);
    require(controller.targets()[0].phase == TargetPhase::Queued, "stale session must not acknowledge new work");
    transport.reply_all(10003);
    observe_targets(controller, windows, 10004); controller.tick(10004);
    require(controller.status() == ControllerStatus::Settled, "current session should settle");
}

void test_retry_then_stop() {
    FakeTransport transport;
    LayoutController controller(transport);
    auto windows = inputs(1);
    controller.update(context(), windows); controller.tick(1);
    transport.reply_all(1000); controller.tick(1000); controller.tick(1499);
    require(transport.commands.size() == 1, "deadline must begin at submission");
    controller.tick(1500);
    require(transport.commands.size() == 2 && controller.targets()[0].retries == 1, "one retry");
    transport.reply_all(1501); controller.tick(1501);
    windows[0].geometry.captured_at = 2002; controller.update(context(), windows); controller.tick(2002);
    require(controller.status() == ControllerStatus::Unconfirmed && controller.refinements_used() == 0,
            "wrong position/ignored command must not infer minimum");
    controller.tick(99999); controller.pause(); controller.update(context(), windows); controller.tick(100000);
    require(transport.commands.size() == 2, "same suspended configuration must not restart");
}

// Feed stable observations through both attempts; clamps are deliberately applied
// as observed geometry rather than conflated with a successful transport reply.
void finish_attempts(LayoutController& controller, FakeTransport& transport,
                     std::vector<LayoutInput>& windows, ULONGLONG start) {
    transport.reply_all(start); controller.tick(start);
    for (auto& window : windows) { window.geometry.captured_at = start + 250; }
    controller.update(context(), windows); controller.tick(start + 250);
    for (auto& window : windows) { window.geometry.captured_at = start + 501; }
    controller.update(context(), windows); controller.tick(start + 501);
    transport.reply_all(start + 502); controller.tick(start + 502);
    for (auto& window : windows) { window.geometry.captured_at = start + 750; }
    controller.update(context(), windows); controller.tick(start + 750);
    for (auto& window : windows) { window.geometry.captured_at = start + 1003; }
    controller.update(context(), windows); controller.tick(start + 1003);
}

void test_cascade() {
    FakeTransport transport;
    LayoutController controller(transport);
    auto windows = inputs(4);
    controller.update(context(), windows); controller.tick(1);
    observe_targets(controller, windows, 3);
    // Three stack tiles initially 300px; first requires 400.
    windows[1].geometry.outer.bottom = windows[1].geometry.visible.bottom = 400;
    finish_attempts(controller, transport, windows, 10);
    require(controller.refinements_used() == 1, "first minimum must reflow");
    auto targets = controller.targets();
    require(targets[1].visible.height() == 400 && targets[2].visible.height() == 250,
            "first reflow 400/250/250");
    controller.tick(1100); observe_targets(controller, windows, 1101);
    windows[2].geometry.outer.bottom = windows[2].geometry.visible.bottom = windows[2].geometry.visible.top + 280;
    finish_attempts(controller, transport, windows, 1110);
    require(controller.refinements_used() == 2, "second minimum must consume same epoch budget");
    targets = controller.targets();
    require(targets[1].visible.height() == 400 && targets[2].visible.height() == 280 &&
            targets[3].visible.height() == 220, "cascade 400/280/220");
    controller.tick(2200); observe_targets(controller, windows, 2202);
    transport.reply_all(2201); controller.tick(2202);
    require(controller.status() == ControllerStatus::Settled, "constrained stack must converge");
}

void test_step_size_and_budget() {
    FakeTransport transport;
    LayoutController controller(transport);
    auto windows = inputs(4);
    controller.update(context(), windows); controller.tick(1);
    observe_targets(controller, windows, 2);
    const auto canonical = controller.targets()[1].outer;
    windows[1].geometry.outer.bottom -= 6; windows[1].geometry.visible.bottom -= 6;
    finish_attempts(controller, transport, windows, 10);
    require(controller.status() == ControllerStatus::Settled && controller.refinements_used() == 0 &&
            controller.targets()[1].phase == TargetPhase::Constrained && controller.targets()[1].outer == canonical,
            "smaller grid size must settle without minimum inference or target drift");
    const auto requests = transport.commands.size(); controller.tick(100000);
    require(transport.commands.size() == requests, "grid size must not resend indefinitely");

    FakeTransport limited;
    LayoutController limit(limited);
    windows = inputs(2); limit.update(context(), windows); limit.tick(1);
    observe_targets(limit, windows, 2);
    windows[0].geometry.outer.right = windows[0].geometry.visible.right = 650;
    finish_attempts(limit, limited, windows, 10);
    require(limit.refinements_used() == 1, "first clamp consumes budget");
    limit.tick(1100); observe_targets(limit, windows, 1101);
    windows[0].geometry.outer.right = windows[0].geometry.visible.right = 700;
    finish_attempts(limit, limited, windows, 1110);
    require(limit.status() == ControllerStatus::RefinementLimitReached && limit.refinements_used() == 1,
            "hard cascade cap within same external epoch");

    FakeTransport impossible;
    LayoutController infeasible(impossible);
    windows = inputs(2); windows[0].constraints.width.minimum = 9999;
    infeasible.update(context(), windows); infeasible.tick(1);
    require(infeasible.status() == ControllerStatus::LayoutInfeasible && impossible.commands.empty(),
            "infeasible solver must issue no partial plan");
}

void test_visible_safety() {
    FakeTransport transport;
    LayoutController controller(transport);
    auto windows = inputs(1);
    controller.update(context(), windows); controller.tick(1); observe_targets(controller, windows, 2);
    windows[0].geometry.outer.left = windows[0].geometry.visible.left = -1;
    windows[0].geometry.captured_at = 3; controller.update(context(), windows);
    transport.reply_all(2); controller.tick(3);
    require(controller.status() != ControllerStatus::Settled, "outer tolerance cannot allow rcWork escape");

    FakeTransport overlapping;
    LayoutController overlap(overlapping);
    auto pair = inputs(2);
    overlap.update(context(), pair); overlap.tick(1); observe_targets(overlap, pair, 2);
    // Both outer goals are within tolerance, but visible edges overlap by 1px.
    --pair[1].geometry.outer.left; --pair[1].geometry.visible.left;
    pair[1].geometry.captured_at = 3;
    overlap.update(context(), pair); overlapping.reply_all(2); overlap.tick(3);
    require(overlap.status() != ControllerStatus::Settled, "tolerance cannot permit visible overlap");
}

void test_invalidation_and_failure() {
    FakeTransport transport;
    LayoutController controller(transport);
    auto windows = inputs(2);
    controller.update(context(), windows); controller.tick(1);
    const auto lease = transport.commands[0].lease;
    const auto old_version = controller.layout_version();
    auto changed = context(); changed.dpi = 144; ++changed.revision;
    for (auto& window : windows) { window.geometry.dpi = 144; window.geometry.captured_at = 2; }
    controller.update(changed, windows); controller.tick(2);
    require(!lease->valid.load() && controller.layout_version() > old_version,
            "DPI/context change must revoke old leases and targets");
    transport.replies.push_back({transport.commands.back(), ActuationStatus::Failed, ERROR_ACCESS_DENIED, 2, 2});
    controller.tick(3);
    require(controller.status() == ControllerStatus::ActuatorFailure, "API failure is distinct from size constraints");
    const auto count = transport.commands.size();
    controller.pause(); controller.update(changed, windows); controller.tick(9999);
    require(transport.commands.size() == count && controller.refinements_used() == 0,
            "transient pause must not restart an API-failed configuration");
}
}

int main() {
    try {
        test_race_and_tolerance(); test_backpressure_and_stale(); test_retry_then_stop();
        test_cascade(); test_step_size_and_budget(); test_visible_safety(); test_invalidation_and_failure();
        std::puts("layout_controller tests passed (no Win32 movement)"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
