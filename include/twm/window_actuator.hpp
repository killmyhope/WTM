#pragma once

#include "twm/geometry.hpp"
#include "twm/window_classifier.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace twm {

// A lifetime lease is never recycled for another WindowId/HWND generation.
struct WindowLease {
    WindowId id{};
    WindowIdentity identity;
    std::atomic<bool> valid{true};
};

struct WindowCommand {
    std::uint64_t request_id{}, session_epoch{}, layout_version{};
    WindowId id{};
    WindowIdentity identity;
    std::shared_ptr<WindowLease> lease;
    Rect target_outer;
    HMONITOR monitor{};
    UINT dpi{};
};

enum class SubmitStatus { Accepted, Coalesced, Backpressure, Stopped };
enum class ActuationStatus { Submitted, Failed, Cancelled };

struct ActuationResult {
    WindowCommand command;
    ActuationStatus status = ActuationStatus::Cancelled;
    DWORD error{};
    ULONGLONG issued_at{}, submitted_at{};
};

class ActuatorTransport {
public:
    virtual ~ActuatorTransport() = default;
    virtual void set_session(std::uint64_t epoch) noexcept = 0;
    [[nodiscard]] virtual SubmitStatus submit(const WindowCommand&) = 0;
    [[nodiscard]] virtual bool pop_result(ActuationResult&) = 0;
};

struct ActuatorApi {
    decltype(&::SetWindowPos) set_window_pos = &::SetWindowPos;
};

// Only queues are locked. No user32/DWM call or PostMessage runs under mutex_.
// Each accepted request holds a credit until its result is drained; thus a slow
// message loop cannot overflow the result queue. Coalescing transfers the credit
// and implicitly supersedes the old, not-yet-executed request (no separate reply).
class WindowActuator final : public ActuatorTransport {
public:
    static constexpr UINT wake_message = WM_APP + 3;
    explicit WindowActuator(HWND notify_window, std::size_t capacity = 128,
                            ActuatorApi api = {});
    ~WindowActuator() override;
    WindowActuator(const WindowActuator&) = delete;
    WindowActuator& operator=(const WindowActuator&) = delete;
    void set_session(std::uint64_t epoch) noexcept override;
    [[nodiscard]] SubmitStatus submit(const WindowCommand&) override;
    [[nodiscard]] bool pop_result(ActuationResult&) override;
    void stop() noexcept;

private:
    void run(std::stop_token stop);
    [[nodiscard]] ActuationResult execute(const WindowCommand&, std::stop_token) const noexcept;
    HWND notify_window_{};
    const std::size_t capacity_;
    ActuatorApi api_;
    std::atomic<std::uint64_t> session_{0};
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::vector<WindowCommand> requests_;
    std::vector<ActuationResult> results_;
    std::size_t outstanding_{};
    bool stopped_{};
    // Last member: every dependency exists before the thread starts.
    std::jthread worker_;
};

} // namespace twm
