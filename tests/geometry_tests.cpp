#include "twm/geometry.hpp"

#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using namespace twm;
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void conversions() {
    const Rect outer{-1008, -500, -192, 108};
    const Rect visible{-1000, -500, -200, 100};
    const auto measurement = measure_frame_insets(outer, visible);
    require(measurement.ok() && measurement.value == FrameInsets{8, 0, 8, 8}, "normal frame insets");
    const Rect work{-1920, -200, 0, 880};
    const Rect target{-1920, -200, -1000, 880};
    const auto expanded = outer_from_visible(target, measurement.value, work);
    require(expanded.ok() && expanded.value == Rect{-1928, -200, -992, 888}, "expand physical visible");
    require(!contains_rectangle(work, expanded.value), "outer is allowed beyond rcWork");
    const auto restored = visible_from_outer(expanded.value, measurement.value, work);
    require(restored.ok() && restored.value == target, "roundtrip must be exact");
    require(!measure_frame_insets({0, 0, 100, 100}, {-1, 0, 100, 100}).ok(), "negative/custom inset accepted");
    require(!outer_from_visible(target, {-1, 0, 0, 0}, work).ok(), "negative inset expanded");
    require(outer_from_visible({-1921, 0, -100, 100}, {}, work).error ==
            GeometryError::OutsideWorkArea, "visible escaped rcWork");
    require(!visible_from_outer({0, 0, 10, 10}, {6, 0, 6, 0}, {0, 0, 100, 100}).ok(),
            "degenerate visible width accepted");
    require(!measure_frame_insets({0, 0, 0, 10}, {0, 0, 1, 1}).ok(), "empty outer accepted");
    constexpr auto low = std::numeric_limits<std::int32_t>::min();
    constexpr auto high = std::numeric_limits<std::int32_t>::max();
    require(outer_from_visible({low, 0, low + 100, 100}, {1, 0, 0, 0},
                               {low, 0, low + 200, 200}).error == GeometryError::Overflow,
            "left overflow wrapped");
    require(outer_from_visible({high - 100, 0, high, 100}, {0, 0, 1, 0},
                               {high - 200, 0, high, 200}).error == GeometryError::Overflow,
            "right overflow wrapped");
    require(!outer_from_visible({0, 0, 100, 100}, {high, 0, high, 0},
                                {0, 0, 200, 200}).ok(), "excessive outer width accepted");
    require(!valid_rectangle({low, 0, high, 100}), "Win32-incompatible extent accepted");
}

void control_and_dpi() {
    for (const Rect work : {Rect{-1920, -1080, -20, -40}, Rect{40, 70, 1600, 900},
                            Rect{0, 0, 1, 1}}) {
        for (const Rect suggestion : {Rect{30000, 30000, 30100, 30100},
                                      Rect{-30000, -30000, -20000, -20000},
                                      Rect{-100, -100, 0, 0}}) {
            const auto fitted = fit_control_rectangle(suggestion, work);
            require(fitted.ok() && contains_rectangle(work, fitted.value), "control escaped target work area");
        }
    }
    require(!fit_control_rectangle({0, 0, 0, 1}, {0, 0, 100, 100}).ok(), "invalid suggested rect");
    for (const auto [dpi, pixels] : {std::pair{96u, 8}, {120u, 10}, {144u, 12},
                                     {168u, 14}, {192u, 16}}) {
        const auto converted = dip_to_pixels(8, dpi);
        require(converted.ok() && converted.value == pixels, "DIP rounding wrong");
    }
    require(dip_to_pixels(1, 144).value == 2, "half pixel must round consistently");
    require(dip_to_pixels(0, 96).ok() && !dip_to_pixels(-1, 96).ok() &&
            !dip_to_pixels(8, 0).ok(), "invalid DIP/DPI accepted");
    require(dip_to_pixels(std::numeric_limits<std::int32_t>::max(),
                          std::numeric_limits<std::uint32_t>::max()).error == GeometryError::Overflow,
            "DIP overflow wrapped");
}

void roundtrip_oracle() {
    std::mt19937 random(0xF2A6u);
    for (unsigned index = 0; index < 2000; ++index) {
        const std::int32_t x = -10000 + static_cast<std::int32_t>(random() % 20000);
        const std::int32_t y = -10000 + static_cast<std::int32_t>(random() % 20000);
        const std::int32_t width = 1 + static_cast<std::int32_t>(random() % 2000);
        const std::int32_t height = 1 + static_cast<std::int32_t>(random() % 2000);
        const FrameInsets insets{static_cast<std::int32_t>(random() % 40),
            static_cast<std::int32_t>(random() % 40), static_cast<std::int32_t>(random() % 40),
            static_cast<std::int32_t>(random() % 40)};
        const Rect target{x, y, x + width, y + height};
        const Rect work{x - 50, y - 50, x + width + 50, y + height + 50};
        const auto outer = outer_from_visible(target, insets, work);
        require(outer.ok(), "random expansion failed");
        const auto visible = visible_from_outer(outer.value, insets, work);
        const auto measured = measure_frame_insets(outer.value, target);
        require(visible.ok() && visible.value == target && measured.ok() && measured.value == insets,
                "random roundtrip failed");
    }
}
} // namespace

int main() {
    try {
        conversions();
        control_and_dpi();
        roundtrip_oracle();
        std::cout << "Geometry numeric tests passed (2000 roundtrips).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
