#include "twm/window_geometry.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

using twm::GeometryReadStatus;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[noreturn]] void win32_failure(const char* operation) {
    const DWORD error = GetLastError();
    throw std::runtime_error(std::string(operation) + ": error " + std::to_string(error));
}

constexpr wchar_t fixture_class[] = L"TwmGeometryFixture";

class WindowClass {
public:
    WindowClass() {
        WNDCLASSW value{};
        value.lpfnWndProc = DefWindowProcW;
        value.hInstance = GetModuleHandleW(nullptr);
        value.lpszClassName = fixture_class;
        if (RegisterClassW(&value) == 0) {
            win32_failure("RegisterClassW");
        }
    }
    ~WindowClass() {
        UnregisterClassW(fixture_class, GetModuleHandleW(nullptr));
    }
    WindowClass(const WindowClass&) = delete;
    WindowClass& operator=(const WindowClass&) = delete;
};

class Fixture {
public:
    Fixture() {
        hwnd_ = CreateWindowExW(
            0, fixture_class, L"TWM geometry test", WS_OVERLAPPEDWINDOW,
            64, 64, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd_ == nullptr) {
            win32_failure("CreateWindowExW");
        }
        identity_ = {hwnd_, GetCurrentProcessId(), GetCurrentThreadId()};
        show();
    }
    ~Fixture() {
        if (IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] HWND get() const noexcept { return hwnd_; }
    [[nodiscard]] twm::WindowIdentity identity() const noexcept { return identity_; }
    void show() const {
        // A fixture may modify only its own HWND and never activate it.
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        require(IsWindowVisible(hwnd_) && !IsIconic(hwnd_), "Fixture is not shown normally");
        require(DwmFlush() == S_OK, "DwmFlush failed");
    }

private:
    HWND hwnd_{};
    twm::WindowIdentity identity_{};
};

void expect_failure(const twm::GeometryReadResult& result,
                    const GeometryReadStatus status, const char* message) {
    require(!result.ok() && !result.snapshot && result.status == status, message);
}

struct Injection {
    enum class Mutation { None, Hide, Minimize, Destroy, Style };

    HWND fixture{};
    HRESULT cloak_result = S_OK;
    HRESULT frame_result = S_OK;
    unsigned cloak_result_call{}; // Zero applies the result to every call.
    unsigned frame_result_call{};
    DWORD cloak{};
    bool rect_failure{};
    bool placement_failure{};
    bool invalid_rect{};
    bool invalid_placement{};
    bool invalid_placement_length{};
    bool invalid_show_command{};
    bool negative_inset{};
    bool changed_outer{};
    bool changed_visible{};
    bool changed_placement{};
    bool arguments_valid = true;
    unsigned outer_calls{};
    unsigned cloak_calls{};
    unsigned frame_calls{};
    unsigned placement_calls{};
    Mutation mutation = Mutation::None;
    unsigned mutation_frame_call = 1;
};

thread_local Injection* active_injection = nullptr;

BOOL WINAPI injected_rect(const HWND hwnd, LPRECT output) {
    auto& injection = *active_injection;
    injection.arguments_valid &= hwnd == injection.fixture && output != nullptr;
    ++injection.outer_calls;
    if (injection.rect_failure) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    *output = RECT{100, 100, 740, 580};
    if (injection.invalid_rect) {
        output->right = output->left;
    }
    if (injection.changed_outer && injection.outer_calls > 1) {
        ++output->left;
    }
    return TRUE;
}

HRESULT WINAPI injected_dwm(
    const HWND hwnd, const DWORD attribute, PVOID output, const DWORD bytes) {
    auto& injection = *active_injection;
    injection.arguments_valid &= hwnd == injection.fixture && output != nullptr;
    if (attribute == DWMWA_CLOAKED) {
        ++injection.cloak_calls;
        injection.arguments_valid &= bytes == sizeof(DWORD);
        *static_cast<DWORD*>(output) = injection.cloak;
        return injection.cloak_result_call == 0 ||
                injection.cloak_result_call == injection.cloak_calls
            ? injection.cloak_result : S_OK;
    }
    injection.arguments_valid &=
        attribute == DWMWA_EXTENDED_FRAME_BOUNDS && bytes == sizeof(RECT);
    ++injection.frame_calls;
    auto& visible = *static_cast<RECT*>(output);
    visible = RECT{108, 100, 732, 572};
    if (injection.negative_inset) {
        visible.left = 99;
    }
    if (injection.changed_visible && injection.frame_calls > 1) {
        ++visible.top;
    }
    if (injection.frame_calls == injection.mutation_frame_call) {
        switch (injection.mutation) {
        case Injection::Mutation::None: break;
        case Injection::Mutation::Hide:
            ShowWindow(hwnd, SW_HIDE);
            break;
        case Injection::Mutation::Minimize:
            ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
            break;
        case Injection::Mutation::Destroy:
            injection.arguments_valid &= DestroyWindow(hwnd) != FALSE;
            break;
        case Injection::Mutation::Style:
            SetLastError(ERROR_SUCCESS);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
            injection.arguments_valid &= GetLastError() == ERROR_SUCCESS;
            break;
        }
    }
    return injection.frame_result_call == 0 ||
            injection.frame_result_call == injection.frame_calls
        ? injection.frame_result : S_OK;
}

BOOL WINAPI injected_placement(const HWND hwnd, WINDOWPLACEMENT* output) {
    auto& injection = *active_injection;
    injection.arguments_valid &= hwnd == injection.fixture &&
        output != nullptr && output->length == sizeof(WINDOWPLACEMENT);
    ++injection.placement_calls;
    if (injection.placement_failure) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    *output = {};
    output->length = sizeof(WINDOWPLACEMENT);
    output->showCmd = SW_SHOWNORMAL;
    // A deliberately different workspace rectangle: no screen-space equality
    // or containment comparison with the outer rectangle is permitted here.
    output->rcNormalPosition = RECT{-700, -300, -60, 180};
    if (injection.invalid_placement) {
        output->rcNormalPosition.bottom = output->rcNormalPosition.top;
    }
    if (injection.invalid_placement_length) {
        output->length = 0;
    }
    if (injection.invalid_show_command) {
        output->showCmd = 0xFFFF;
    }
    if (injection.changed_placement && injection.placement_calls > 1) {
        ++output->rcNormalPosition.left;
    }
    return TRUE;
}

[[nodiscard]] twm::GeometryReadResult inject_read(
    const Fixture& fixture, Injection& injection) {
    injection.fixture = fixture.get();
    active_injection = &injection;
    const twm::WindowGeometryReader reader{
        {&injected_rect, &injected_dwm, &injected_placement}};
    const auto result = reader.read(fixture.identity());
    active_injection = nullptr;
    require(injection.arguments_valid, "Reader passed invalid API arguments");
    return result;
}

void test_native_snapshot() {
    Fixture fixture;
    const twm::WindowGeometryReader reader;
    const auto result = reader.read(fixture.identity());
    require(result.ok(), "Real fixture snapshot failed");
    const auto& snapshot = *result.snapshot;
    require(snapshot.identity == fixture.identity(), "Wrong fixture identity");
    require(snapshot.dpi == GetDpiForWindow(fixture.get()), "Wrong fixture DPI");
    require(snapshot.placement.length == sizeof(WINDOWPLACEMENT), "Wrong placement length");
    require(snapshot.captured_at <= GetTickCount64(), "Invalid capture timestamp");
    // Choose the current visible bounds as an exact synthetic work area: the
    // invisible outer border may extend beyond this area, which is expected.
    const auto outer = twm::outer_from_visible(
        snapshot.visible, snapshot.insets, snapshot.visible);
    require(outer.ok() && outer.value == snapshot.outer, "Native visible-to-outer round trip failed");
    const auto visible = twm::visible_from_outer(
        snapshot.outer, snapshot.insets, snapshot.visible);
    require(visible.ok() && visible.value == snapshot.visible, "Native outer-to-visible round trip failed");

    ShowWindow(fixture.get(), SW_HIDE);
    expect_failure(reader.read(fixture.identity()), GeometryReadStatus::Inactive,
                   "Hidden fixture produced a snapshot");
    fixture.show();
    ShowWindow(fixture.get(), SW_SHOWMINNOACTIVE);
    require(IsIconic(fixture.get()) != FALSE, "Fixture did not minimize");
    expect_failure(reader.read(fixture.identity()), GeometryReadStatus::Inactive,
                   "Minimized sentinel geometry was used");
    fixture.show();

    const BOOL cloak = TRUE;
    require(DwmSetWindowAttribute(fixture.get(), DWMWA_CLOAK, &cloak, sizeof(cloak)) == S_OK,
            "Cannot cloak the fixture");
    require(DwmFlush() == S_OK, "Cannot flush the fixture cloak");
    expect_failure(reader.read(fixture.identity()), GeometryReadStatus::Inactive,
                   "Cloaked fixture produced a snapshot");
    const BOOL uncloak = FALSE;
    require(DwmSetWindowAttribute(fixture.get(), DWMWA_CLOAK, &uncloak, sizeof(uncloak)) == S_OK,
            "Cannot uncloak the fixture");

    auto wrong_identity = fixture.identity();
    ++wrong_identity.process_id;
    expect_failure(reader.read(wrong_identity), GeometryReadStatus::Changed,
                   "Unexpected HWND identity accepted");
    require(DestroyWindow(fixture.get()) != FALSE, "Cannot destroy fixture");
    expect_failure(reader.read(fixture.identity()), GeometryReadStatus::Missing,
                   "Destroyed fixture produced a snapshot");
    expect_failure(reader.read({}), GeometryReadStatus::Missing,
                   "Null HWND produced a snapshot");
    std::puts("PASS: native geometry snapshots and inactive states");
}

void test_injected_failures() {
    Fixture fixture;
    Injection baseline;
    const auto accepted = inject_read(fixture, baseline);
    require(accepted.ok(), "Deterministic stable snapshot rejected");
    require(accepted.snapshot->insets == twm::FrameInsets{8, 0, 8, 8}, "Wrong injected frame insets");
    require(baseline.cloak_calls == 3 && baseline.outer_calls == 2 && baseline.frame_calls == 2 &&
            baseline.placement_calls == 2, "Reader skipped a geometry boundary");

    for (const HRESULT hr : {S_FALSE, E_FAIL}) {
        for (unsigned call = 1; call <= 3; ++call) {
            Injection cloak;
            cloak.cloak_result = hr;
            cloak.cloak_result_call = call;
            const auto cloak_result = inject_read(fixture, cloak);
            expect_failure(cloak_result, GeometryReadStatus::ApiError, "Non-S_OK cloak accepted");
            require(cloak_result.hresult == hr, "Cloak HRESULT lost");
        }
        for (unsigned call = 1; call <= 2; ++call) {
            Injection frame;
            frame.frame_result = hr;
            frame.frame_result_call = call;
            const auto frame_result = inject_read(fixture, frame);
            expect_failure(frame_result, GeometryReadStatus::ApiError, "Non-S_OK frame accepted");
            require(frame_result.hresult == hr, "Frame HRESULT lost");
        }
    }
    Injection rect_error;
    rect_error.rect_failure = true;
    const auto rect_result = inject_read(fixture, rect_error);
    expect_failure(rect_result, GeometryReadStatus::ApiError, "Failed GetWindowRect accepted");
    require(rect_result.error == ERROR_ACCESS_DENIED, "GetWindowRect error lost");
    Injection placement_error;
    placement_error.placement_failure = true;
    const auto placement_result = inject_read(fixture, placement_error);
    expect_failure(placement_result, GeometryReadStatus::ApiError, "Failed GetWindowPlacement accepted");
    require(placement_result.error == ERROR_INVALID_DATA, "GetWindowPlacement error lost");

    Injection bad_rect;
    bad_rect.invalid_rect = true;
    expect_failure(inject_read(fixture, bad_rect), GeometryReadStatus::InvalidGeometry,
                   "Degenerate outer rectangle accepted");
    Injection negative;
    negative.negative_inset = true;
    expect_failure(inject_read(fixture, negative), GeometryReadStatus::InvalidGeometry,
                   "Negative frame inset accepted");
    Injection bad_placement;
    bad_placement.invalid_placement = true;
    expect_failure(inject_read(fixture, bad_placement), GeometryReadStatus::InvalidPlacement,
                   "Degenerate workspace rectangle accepted");
    Injection bad_length;
    bad_length.invalid_placement_length = true;
    expect_failure(inject_read(fixture, bad_length), GeometryReadStatus::InvalidPlacement,
                   "Invalid placement length accepted");
    Injection bad_show;
    bad_show.invalid_show_command = true;
    expect_failure(inject_read(fixture, bad_show), GeometryReadStatus::InvalidPlacement,
                   "Unknown placement showCmd accepted");
    Injection cloaked;
    cloaked.cloak = DWM_CLOAKED_APP;
    expect_failure(inject_read(fixture, cloaked), GeometryReadStatus::Inactive,
                   "Injected cloak accepted");
    require(cloaked.outer_calls == 0, "Inactive geometry was sampled");

    twm::GeometryReadApi null_api;
    null_api.get_window_rect = nullptr;
    const auto null_result = twm::WindowGeometryReader{null_api}.read(fixture.identity());
    expect_failure(null_result, GeometryReadStatus::ApiError, "Null API pointer accepted");
    require(null_result.error == ERROR_INVALID_PARAMETER, "Invalid pointer error lost");
    std::puts("PASS: exact DWM success contract, geometry and placement errors");
}

void test_detected_transitions() {
    Fixture stable;
    Injection outer;
    outer.changed_outer = true;
    expect_failure(inject_read(stable, outer), GeometryReadStatus::Changed,
                   "Changed outer bounds accepted");
    Injection visible;
    visible.changed_visible = true;
    expect_failure(inject_read(stable, visible), GeometryReadStatus::Changed,
                   "Changed visible bounds accepted");
    Injection placement;
    placement.changed_placement = true;
    expect_failure(inject_read(stable, placement), GeometryReadStatus::Changed,
                   "Changed workspace placement accepted");

    for (const auto mutation : {Injection::Mutation::Hide, Injection::Mutation::Minimize,
                                Injection::Mutation::Destroy, Injection::Mutation::Style}) {
        for (unsigned frame_call = 1; frame_call <= 2; ++frame_call) {
            Fixture changing;
            Injection injection;
            injection.mutation = mutation;
            injection.mutation_frame_call = frame_call;
            const auto result = inject_read(changing, injection);
            require(!result.ok() && !result.snapshot &&
                    (result.status == GeometryReadStatus::Changed ||
                     result.status == GeometryReadStatus::Missing),
                    "State changed between outer and DWM reads but snapshot was accepted");
        }
    }
    std::puts("PASS: inter-call bounds, placement, visibility, style and lifetime races");
}

void test_dpi_contract() {
    Fixture fixture;
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    require(previous != nullptr, "Cannot enter unaware DPI context");
    const auto result = twm::WindowGeometryReader{}.read(fixture.identity());
    const auto restored = SetThreadDpiAwarenessContext(previous);
    require(restored != nullptr, "Cannot restore PMv2 DPI context");
    expect_failure(result, GeometryReadStatus::UnsupportedDpiContext,
                   "DPI-virtualized GetWindowRect context was accepted");
    std::puts("PASS: PMv2 physical-coordinate contract");
}

} // namespace

int main() {
    try {
        require(AreDpiAwarenessContextsEqual(
                    GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
                "Geometry tests require an embedded PerMonitorV2 manifest");
        BOOL composed = FALSE;
        require(DwmIsCompositionEnabled(&composed) == S_OK && composed,
                "Geometry tests require a Windows desktop with DWM enabled");
        WindowClass window_class;
        test_native_snapshot();
        test_injected_failures();
        test_detected_transitions();
        test_dpi_contract();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
