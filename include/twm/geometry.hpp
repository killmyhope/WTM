#pragma once

#include "twm/layout.hpp"

#include <cstdint>

namespace twm {

enum class GeometryError { None, InvalidRectangle, InvalidInsets, OutsideWorkArea, Overflow, InvalidScale };

struct FrameInsets {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
    bool operator==(const FrameInsets&) const = default;
};

struct RectConversion {
    GeometryError error = GeometryError::None;
    Rect value{};
    [[nodiscard]] bool ok() const noexcept { return error == GeometryError::None; }
};

struct InsetsMeasurement {
    GeometryError error = GeometryError::None;
    FrameInsets value{};
    [[nodiscard]] bool ok() const noexcept { return error == GeometryError::None; }
};

struct PixelConversion {
    GeometryError error = GeometryError::None;
    std::int32_t value{};
    [[nodiscard]] bool ok() const noexcept { return error == GeometryError::None; }
};

[[nodiscard]] bool valid_rectangle(Rect rectangle) noexcept;
[[nodiscard]] bool contains_rectangle(Rect outer, Rect inner) noexcept;
// Отрицательные дельты/custom frames консервативно отклоняются, не clamp-ятся.
[[nodiscard]] InsetsMeasurement measure_frame_insets(Rect outer, Rect visible) noexcept;
// Outer может выступать за rcWork невидимой рамкой; visible обязан оставаться внутри.
[[nodiscard]] RectConversion outer_from_visible(Rect visible, FrameInsets insets, Rect work) noexcept;
[[nodiscard]] RectConversion visible_from_outer(Rect outer, FrameInsets insets, Rect work) noexcept;
[[nodiscard]] RectConversion fit_control_rectangle(Rect suggested, Rect work) noexcept;
[[nodiscard]] PixelConversion dip_to_pixels(std::int32_t dip, std::uint32_t dpi) noexcept;

} // namespace twm
