#include "twm/geometry.hpp"

#include <algorithm>
#include <limits>

namespace twm {
namespace {
using Coordinate = std::int64_t;
constexpr Coordinate minimum = std::numeric_limits<std::int32_t>::min();
constexpr Coordinate maximum = std::numeric_limits<std::int32_t>::max();

bool valid_insets(FrameInsets insets) noexcept {
    return insets.left >= 0 && insets.top >= 0 && insets.right >= 0 && insets.bottom >= 0;
}

RectConversion checked_rect(Coordinate left, Coordinate top,
                            Coordinate right, Coordinate bottom) noexcept {
    if (left < minimum || left > maximum || top < minimum || top > maximum ||
        right < minimum || right > maximum || bottom < minimum || bottom > maximum ||
        right - left > maximum || bottom - top > maximum) {
        return {GeometryError::Overflow, {}};
    }
    const Rect result{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                      static_cast<std::int32_t>(right), static_cast<std::int32_t>(bottom)};
    return valid_rectangle(result) ? RectConversion{GeometryError::None, result}
                                   : RectConversion{GeometryError::InvalidRectangle, {}};
}
} // namespace

bool valid_rectangle(Rect rectangle) noexcept {
    return rectangle.width() > 0 && rectangle.height() > 0 &&
           rectangle.width() <= maximum && rectangle.height() <= maximum;
}

bool contains_rectangle(Rect outer, Rect inner) noexcept {
    return valid_rectangle(outer) && valid_rectangle(inner) &&
           inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

InsetsMeasurement measure_frame_insets(Rect outer, Rect visible) noexcept {
    if (!valid_rectangle(outer) || !valid_rectangle(visible)) {
        return {GeometryError::InvalidRectangle, {}};
    }
    if (!contains_rectangle(outer, visible)) {
        return {GeometryError::InvalidInsets, {}};
    }
    return {GeometryError::None,
            {static_cast<std::int32_t>(Coordinate{visible.left} - outer.left),
             static_cast<std::int32_t>(Coordinate{visible.top} - outer.top),
             static_cast<std::int32_t>(Coordinate{outer.right} - visible.right),
             static_cast<std::int32_t>(Coordinate{outer.bottom} - visible.bottom)}};
}

RectConversion outer_from_visible(Rect visible, FrameInsets insets, Rect work) noexcept {
    if (!valid_rectangle(work) || !valid_rectangle(visible)) {
        return {GeometryError::InvalidRectangle, {}};
    }
    if (!valid_insets(insets)) {
        return {GeometryError::InvalidInsets, {}};
    }
    if (!contains_rectangle(work, visible)) {
        return {GeometryError::OutsideWorkArea, {}};
    }
    // Только видимые границы обязаны лежать в rcWork. Invisible borders могут
    // выступать за рабочую область; арифметика не saturate-ится и не wrap-ится.
    return checked_rect(Coordinate{visible.left} - insets.left,
                        Coordinate{visible.top} - insets.top,
                        Coordinate{visible.right} + insets.right,
                        Coordinate{visible.bottom} + insets.bottom);
}

RectConversion visible_from_outer(Rect outer, FrameInsets insets, Rect work) noexcept {
    if (!valid_rectangle(work) || !valid_rectangle(outer)) {
        return {GeometryError::InvalidRectangle, {}};
    }
    if (!valid_insets(insets)) {
        return {GeometryError::InvalidInsets, {}};
    }
    const auto result = checked_rect(Coordinate{outer.left} + insets.left,
                                     Coordinate{outer.top} + insets.top,
                                     Coordinate{outer.right} - insets.right,
                                     Coordinate{outer.bottom} - insets.bottom);
    if (!result.ok()) {
        return result;
    }
    if (!contains_rectangle(work, result.value)) {
        return {GeometryError::OutsideWorkArea, {}};
    }
    return result;
}

RectConversion fit_control_rectangle(Rect suggested, Rect work) noexcept {
    if (!valid_rectangle(work) || !valid_rectangle(suggested)) {
        return {GeometryError::InvalidRectangle, {}};
    }
    // Оставляем 1 px до границ, когда площадь это позволяет. Даже для
    // искусственной области 1x1 возвращается целиком содержащийся HWND rect.
    const Coordinate margin_x = work.width() > 2 ? 1 : 0;
    const Coordinate margin_y = work.height() > 2 ? 1 : 0;
    const Coordinate left_bound = Coordinate{work.left} + margin_x;
    const Coordinate top_bound = Coordinate{work.top} + margin_y;
    const Coordinate right_bound = Coordinate{work.right} - margin_x;
    const Coordinate bottom_bound = Coordinate{work.bottom} - margin_y;
    const Coordinate width = std::min(suggested.width(), right_bound - left_bound);
    const Coordinate height = std::min(suggested.height(), bottom_bound - top_bound);
    const Coordinate x = std::clamp(Coordinate{suggested.left}, left_bound, right_bound - width);
    const Coordinate y = std::clamp(Coordinate{suggested.top}, top_bound, bottom_bound - height);
    return checked_rect(x, y, x + width, y + height);
}

PixelConversion dip_to_pixels(std::int32_t dip, std::uint32_t dpi) noexcept {
    if (dip < 0 || dpi == 0) {
        return {GeometryError::InvalidScale, 0};
    }
    // Максимальное int32 * uint32 + 48 помещается в int64.
    const Coordinate pixels = (Coordinate{dip} * dpi + 48) / 96;
    if (pixels > maximum) {
        return {GeometryError::Overflow, 0};
    }
    return {GeometryError::None, static_cast<std::int32_t>(pixels)};
}

} // namespace twm
