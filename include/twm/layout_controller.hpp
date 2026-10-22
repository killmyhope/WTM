#pragma once

#include "twm/window_actuator.hpp"
#include "twm/window_geometry.hpp"

#include <functional>
#include <span>
#include <string_view>

namespace twm {

struct LayoutContext {
    HMONITOR monitor{};
    Rect work_area;
    UINT dpi{};
    std::uint64_t revision{};
    MasterStackOptions options;
};

struct LayoutInput {
    WindowId id{};
    WindowGeometrySnapshot geometry;
    SizeConstraints constraints;
};

enum class ControllerStatus {
    Paused, Applying, Settled, LayoutInfeasible, RefinementLimitReached,
    Unconfirmed, ActuatorFailure
};
enum class TargetPhase { Ready, Queued, Awaiting, Confirmed, Constrained };

struct WindowTarget {
    WindowId id{};
    Rect slot, visible, outer; // Immutable canonical targets for this version.
    TargetPhase phase = TargetPhase::Ready;
    unsigned retries{};
    std::uint64_t request_id{};
};

// Single owner: message-loop thread. Geometry is read by the caller BEFORE tick,
// on LOCATIONCHANGE and on the fallback timer. Explicit monotonic ms timestamps
// make event/reply races and timeouts testable without moving real applications.
class LayoutController final {
public:
    using Log = std::function<void(std::wstring_view)>;
    explicit LayoutController(ActuatorTransport& transport, Log log = {});
    ~LayoutController();
    LayoutController(const LayoutController&) = delete;
    LayoutController& operator=(const LayoutController&) = delete;

    void update(const LayoutContext&, std::span<const LayoutInput> inputs);
    void tick(ULONGLONG now);
    // Invalidates queued commands, preserves refinement budget across transient
    // read failures. No automatic restore is attempted by this stage's executor.
    void pause();
    [[nodiscard]] ControllerStatus status() const noexcept { return status_; }
    [[nodiscard]] std::uint64_t layout_version() const noexcept { return version_; }
    [[nodiscard]] std::size_t refinements_used() const noexcept { return budget_.used(); }
    [[nodiscard]] std::vector<WindowTarget> targets() const;

private:
    struct Entry {
        LayoutInput input;
        std::shared_ptr<WindowLease> lease;
        WindowTarget target;
        FrameInsets frame_insets; // Fixed for the context; +/-1 DWM rounding is not a new epoch.
        Rect accepted_outer, accepted_visible;
        SizeConstraints learned;
        std::int64_t height_width_context{};
        ULONGLONG stable_since{}, issued_at{}, submitted_at{};
        unsigned stable_samples{};
    };
    [[nodiscard]] bool same_configuration(const LayoutContext&, std::span<const LayoutInput>) const;
    void observe(Entry&, const WindowGeometrySnapshot&);
    void solve();
    void advance_session();
    void suspend(ControllerStatus, std::wstring_view);
    [[nodiscard]] bool safe_visible(const Entry&, bool require_slot) const noexcept;
    void write(std::wstring_view message) const;

    ActuatorTransport& transport_;
    Log log_;
    LayoutContext context_{};
    std::vector<Entry> entries_;
    ConstraintRefinementBudget budget_{0};
    ControllerStatus status_ = ControllerStatus::Paused;
    std::uint64_t session_{}, version_{}, next_request_{};
    bool configured_{};
    bool backpressure_reported_{};
};

} // namespace twm
