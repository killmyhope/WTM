#include "twm/layout_controller.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace twm {
namespace {
constexpr ULONGLONG confirmation_ms = 500;
constexpr ULONGLONG stable_ms = 100;

bool close_edge(std::int32_t a, std::int32_t b) noexcept {
    return std::abs(std::int64_t{a} - b) <= 1;
}
bool bounds_near(Rect a, Rect b) noexcept {
    return close_edge(a.left, b.left) && close_edge(a.top, b.top) &&
        close_edge(a.right, b.right) && close_edge(a.bottom, b.bottom);
}
bool overlaps(Rect a, Rect b) noexcept {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
SizeConstraints merged(SizeConstraints configured, const SizeConstraints& learned) {
    const auto merge_axis = [](AxisConstraints& output, const AxisConstraints& other) {
        if (other.minimum) { output.minimum = std::max(output.minimum.value_or(0), *other.minimum); }
    };
    merge_axis(configured.width, learned.width);
    merge_axis(configured.height, learned.height);
    return configured;
}
}

LayoutController::LayoutController(ActuatorTransport& transport, Log log)
    : transport_(transport), log_(std::move(log)) {}

LayoutController::~LayoutController() {
    transport_.set_session(0);
    for (auto& entry : entries_) { entry.lease->valid.store(false, std::memory_order_release); }
}

void LayoutController::write(std::wstring_view message) const {
    if (log_) { log_(message); }
}

void LayoutController::advance_session() {
    if (session_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("actuator session exhausted");
    }
    transport_.set_session(++session_);
}

bool LayoutController::same_configuration(const LayoutContext& context,
                                          std::span<const LayoutInput> inputs) const {
    if (!configured_ || entries_.size() != inputs.size() ||
        context_.monitor != context.monitor || context_.revision != context.revision ||
        context_.work_area != context.work_area || context_.dpi != context.dpi ||
        context_.options.outer_gap != context.options.outer_gap ||
        context_.options.inner_gap != context.options.inner_gap ||
        context_.options.master_ratio_per_mille != context.options.master_ratio_per_mille) {
        return false;
    }
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& old = entries_[index].input;
        const auto& input = inputs[index];
        if (old.id != input.id || old.geometry.identity != input.geometry.identity ||
            old.constraints != input.constraints || old.geometry.dpi != input.geometry.dpi ||
            old.geometry.style != input.geometry.style ||
            old.geometry.extended_style != input.geometry.extended_style ||
            !bounds_near({entries_[index].frame_insets.left, entries_[index].frame_insets.top,
                   entries_[index].frame_insets.right, entries_[index].frame_insets.bottom},
                  {input.geometry.insets.left, input.geometry.insets.top,
                   input.geometry.insets.right, input.geometry.insets.bottom})) { return false; }
    }
    return true;
}

void LayoutController::observe(Entry& entry, const WindowGeometrySnapshot& geometry) {
    if (geometry.captured_at < entry.input.geometry.captured_at) { return; }
    if (entry.input.geometry.outer != geometry.outer || entry.input.geometry.visible != geometry.visible) {
        entry.stable_since = geometry.captured_at;
        entry.stable_samples = 1;
    } else if (geometry.captured_at > entry.input.geometry.captured_at) {
        entry.stable_samples = std::min(entry.stable_samples + 1, 3u);
    }
    entry.input.geometry = geometry;
}

void LayoutController::update(const LayoutContext& context, std::span<const LayoutInput> inputs) {
    for (const auto& input : inputs) {
        const auto measured = measure_frame_insets(input.geometry.outer, input.geometry.visible);
        if (!measured.ok() || measured.value != input.geometry.insets || input.geometry.dpi == 0) {
            pause();
            return;
        }
    }
    if (!same_configuration(context, inputs)) {
        advance_session();
        for (auto& entry : entries_) { entry.lease->valid.store(false, std::memory_order_release); }
        entries_.clear();
        entries_.reserve(inputs.size());
        for (const auto& input : inputs) {
            Entry entry;
            entry.input = input;
            entry.frame_insets = input.geometry.insets;
            entry.lease = std::make_shared<WindowLease>();
            entry.lease->id = input.id;
            entry.lease->identity = input.geometry.identity;
            entry.stable_since = input.geometry.captured_at;
            entry.stable_samples = 1;
            entries_.push_back(std::move(entry));
        }
        context_ = context;
        configured_ = true;
        budget_ = ConstraintRefinementBudget(inputs.empty() ? 0 : inputs.size() - 1);
        solve();
    } else {
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            observe(entries_[index], inputs[index].geometry);
        }
        if (status_ == ControllerStatus::Paused) { solve(); }
    }
}

void LayoutController::pause() {
    // A transient snapshot failure must not clear a terminal stop for the same
    // configuration. Only a new external configuration may release that latch.
    if (status_ == ControllerStatus::Applying || status_ == ControllerStatus::Settled) {
        advance_session();
        status_ = ControllerStatus::Paused;
        write(L"Layout paused: no coherent window/monitor snapshot.");
    }
}

void LayoutController::suspend(ControllerStatus status, std::wstring_view reason) {
    advance_session(); // Fence queued work, including requests for other windows.
    status_ = status;
    write(reason);
}

void LayoutController::solve() {
    advance_session();
    if (version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("layout version exhausted");
    }
    ++version_;
    std::vector<LayoutWindow> windows;
    for (const auto& entry : entries_) {
        windows.push_back({entry.input.id, merged(entry.input.constraints, entry.learned)});
    }
    auto layout = solve_master_stack(context_.work_area, windows, context_.options);
    if (!layout.ok()) {
        suspend(ControllerStatus::LayoutInfeasible, L"LayoutInfeasible: solver rejected the configuration; issuance stopped.");
        return;
    }
    // A height learned at one width is not a minimum at a different width.
    bool invalidated = false;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].learned.height.minimum && entries_[i].height_width_context != layout.placements[i].target.width()) {
            entries_[i].learned.height = {};
            windows[i].constraints = merged(entries_[i].input.constraints, entries_[i].learned);
            invalidated = true;
        }
    }
    if (invalidated) { layout = solve_master_stack(context_.work_area, windows, context_.options); }
    if (!layout.ok()) {
        suspend(ControllerStatus::LayoutInfeasible, L"LayoutInfeasible after invalidating width-dependent constraints.");
        return;
    }
    // Validate the complete plan before issuing any partial plan.
    std::vector<Rect> outer;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto converted = outer_from_visible(layout.placements[i].target,
            entries_[i].frame_insets, context_.work_area);
        if (!converted.ok()) {
            suspend(ControllerStatus::LayoutInfeasible, L"LayoutInfeasible: invalid targetOuter.");
            return;
        }
        outer.push_back(converted.value);
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].target = {entries_[i].input.id, layout.placements[i].slot,
                             layout.placements[i].target, outer[i]};
        entries_[i].issued_at = entries_[i].submitted_at = 0;
    }
    status_ = ControllerStatus::Applying;
    backpressure_reported_ = false;
    write(L"Layout: new canonical plan, version=" + std::to_wstring(version_) +
          L", refinement=" + std::to_wstring(budget_.used()) + L"/" + std::to_wstring(budget_.limit()));
}

bool LayoutController::safe_visible(const Entry& entry, bool require_slot) const noexcept {
    const auto visible = entry.input.geometry.visible;
    if (!contains_rectangle(context_.work_area, visible) ||
        (require_slot && !contains_rectangle(entry.target.slot, visible))) { return false; }
    for (const auto& other : entries_) {
        if (other.input.id != entry.input.id && overlaps(visible, other.input.geometry.visible)) { return false; }
    }
    return true;
}

void LayoutController::tick(ULONGLONG now) {
    ActuationResult result;
    while (transport_.pop_result(result)) {
        const auto& command = result.command;
        if (command.session_epoch != session_ || command.layout_version != version_) { continue; }
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return entry.input.id == command.id && entry.target.request_id == command.request_id &&
                entry.input.geometry.identity == command.identity;
        });
        if (found == entries_.end() || found->target.phase != TargetPhase::Queued) { continue; }
        if (result.status != ActuationStatus::Submitted) {
            suspend(ControllerStatus::ActuatorFailure, L"Actuator cancelled/failed: error=" +
                    std::to_wstring(result.error) + L"; issuance stopped until configuration changes.");
            continue;
        }
        found->issued_at = result.issued_at;
        found->submitted_at = result.submitted_at;
        found->target.phase = TargetPhase::Awaiting;
    }
    if (status_ != ControllerStatus::Applying && status_ != ControllerStatus::Settled) { return; }
    bool refined = false;
    bool unconfirmed = false;
    for (auto& entry : entries_) {
        auto& target = entry.target;
        const auto& observed = entry.input.geometry;
        if (target.phase == TargetPhase::Confirmed || target.phase == TargetPhase::Constrained) {
            const bool still_valid = target.phase == TargetPhase::Confirmed
                ? bounds_near(observed.outer, target.outer) && bounds_near(observed.visible, target.visible)
                : bounds_near(observed.outer, entry.accepted_outer) && bounds_near(observed.visible, entry.accepted_visible);
            if (still_valid && safe_visible(entry, target.phase == TargetPhase::Constrained)) { continue; }
            target.phase = TargetPhase::Awaiting;
            status_ = ControllerStatus::Applying;
        }
        if (target.phase != TargetPhase::Awaiting) { continue; }
        const bool fresh = observed.captured_at >= entry.issued_at;
        // A LOCATIONCHANGE received before the worker reply is already in input.
        if (fresh && bounds_near(observed.outer, target.outer) && bounds_near(observed.visible, target.visible) &&
            safe_visible(entry, false)) {
            target.phase = TargetPhase::Confirmed;
            continue;
        }
        if (now < entry.submitted_at || now - entry.submitted_at < confirmation_ms) { continue; }
        if (target.retries == 0) {
            target.retries = 1;
            target.phase = TargetPhase::Ready;
            write(L"Layout: one retry for WindowId=" + std::to_wstring(entry.input.id));
            continue;
        }
        const bool stable = fresh && observed.captured_at >= entry.submitted_at &&
            entry.stable_samples >= 2 && now >= entry.stable_since &&
            now - entry.stable_since >= stable_ms && now >= observed.captured_at &&
            now - observed.captured_at <= confirmation_ms;
        const bool anchored = close_edge(observed.visible.left, target.visible.left) &&
                              close_edge(observed.visible.top, target.visible.top);
        if (stable && anchored) {
            const bool larger_width = observed.visible.width() > target.visible.width() + 1;
            const bool larger_height = observed.visible.height() > target.visible.height() + 1;
            if (larger_width || larger_height) {
                if (larger_width) { entry.learned.width.minimum = static_cast<std::int32_t>(observed.visible.width()); }
                if (larger_height) {
                    entry.learned.height.minimum = static_cast<std::int32_t>(observed.visible.height());
                    entry.height_width_context = target.visible.width();
                }
                refined = true;
                continue;
            }
            if (observed.visible.width() <= target.visible.width() + 1 &&
                observed.visible.height() <= target.visible.height() + 1 && safe_visible(entry, true)) {
                target.phase = TargetPhase::Constrained;
                entry.accepted_outer = observed.outer;
                entry.accepted_visible = observed.visible;
                write(L"Constrained/unknown step: WindowId=" + std::to_wstring(entry.input.id) +
                      L"; canonical target retained, no minimum inferred.");
                continue;
            }
        }
        unconfirmed = true;
    }
    if (refined) {
        if (!budget_.try_consume()) {
            suspend(ControllerStatus::RefinementLimitReached, L"LayoutInfeasible: RefinementLimitReached; issuance stopped.");
        } else { solve(); }
        return; // Reflow must finish before any new commands are sent.
    }
    if (unconfirmed) {
        suspend(ControllerStatus::Unconfirmed, L"Layout unconfirmed after one retry; issuance stopped (no minimum inferred).");
        return;
    }
    bool all_settled = true;
    for (auto& entry : entries_) {
        auto& target = entry.target;
        if (target.phase == TargetPhase::Ready) {
            if (next_request_ == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("actuator request id exhausted");
            }
            const auto request = ++next_request_;
            const WindowCommand command{request, session_, version_, entry.input.id,
                entry.input.geometry.identity, entry.lease, target.outer, context_.monitor, entry.input.geometry.dpi};
            const auto accepted = transport_.submit(command);
            if (accepted == SubmitStatus::Accepted || accepted == SubmitStatus::Coalesced) {
                target.request_id = request;
                target.phase = TargetPhase::Queued;
            } else if (accepted == SubmitStatus::Stopped) {
                suspend(ControllerStatus::ActuatorFailure, L"Actuator stopped; issuance stopped.");
                return;
            } else if (!backpressure_reported_) {
                write(L"Actuator Backpressure: retaining desired targets for next drain.");
                backpressure_reported_ = true;
            }
        }
        all_settled &= target.phase == TargetPhase::Confirmed || target.phase == TargetPhase::Constrained;
    }
    if (all_settled && status_ != ControllerStatus::Settled) {
        status_ = ControllerStatus::Settled;
        write(L"Layout settled: all visible bounds are inside rcWork and do not overlap.");
    }
}

std::vector<WindowTarget> LayoutController::targets() const {
    std::vector<WindowTarget> result;
    for (const auto& entry : entries_) { result.push_back(entry.target); }
    return result;
}

} // namespace twm

