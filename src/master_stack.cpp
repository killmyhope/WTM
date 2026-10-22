#include "twm/layout.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace twm {
namespace {

using Length = std::int64_t;
constexpr Length max_length = std::numeric_limits<std::int32_t>::max();

bool valid_axis(const AxisConstraints& axis) noexcept {
    return (!axis.minimum || *axis.minimum > 0) &&
           (!axis.maximum || *axis.maximum > 0) &&
           (!axis.minimum || !axis.maximum || *axis.minimum <= *axis.maximum) &&
           (!axis.step || (axis.step->increment > 0 && axis.step->base >= 0));
}

struct Bounds {
    Length minimum;
    Length maximum;
};

Bounds bounds(const AxisConstraints& axis) noexcept {
    Length minimum = axis.minimum.value_or(1);
    Length maximum = axis.maximum.value_or(static_cast<std::int32_t>(max_length));
    if (axis.step) {
        const Length base = axis.step->base;
        const Length step = axis.step->increment;
        const Length delta = std::max(Length{0}, minimum - base);
        minimum = base + ((delta + step - 1) / step) * step;
        if (maximum < base) {
            return {1, 0};
        }
        maximum = base + ((maximum - base) / step) * step;
    }
    return {minimum, maximum};
}

Length fit(Length slot, const AxisConstraints& axis) noexcept {
    const auto limit = bounds(axis);
    Length result = std::min(slot, limit.maximum);
    if (axis.step && result >= axis.step->base) {
        result = axis.step->base +
            ((result - axis.step->base) / axis.step->increment) * axis.step->increment;
    }
    return result >= limit.minimum ? result : 0;
}

LayoutResult failure(LayoutError error) { return {error, {}}; }

// Bounded water filling: подбираем общий уровень, сохраняя min/max каждого окна.
// Всегда <= 31 шагов поиска для размеров, допустимых Win32. Нет циклов ожидания
// сходимости приложений. Известный step применяется к target после выделения slot.
std::vector<Length> allocate_heights(Length available, std::span<const Bounds> limits) {
    const auto sum_at = [&](Length level) {
        Length sum = 0;
        for (const auto& limit : limits) {
            sum += std::clamp(level, limit.minimum, limit.maximum);
            if (sum > available) {
                return available + 1;
            }
        }
        return sum;
    };

    Length low = 0;
    Length high = available;
    while (low < high) {
        const Length middle = low + (high - low + 1) / 2;
        if (sum_at(middle) <= available) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }

    std::vector<Length> heights;
    heights.reserve(limits.size());
    Length remaining = available;
    for (const auto& limit : limits) {
        const Length height = std::clamp(low, limit.minimum, limit.maximum);
        heights.push_back(height);
        remaining -= height;
    }
    // При достаточной capacity остаток меньше числа незаполненных ячеек.
    // Если все maximum достигнуты, свободная область остаётся снизу стека.
    for (std::size_t index = 0; index < heights.size() && remaining > 0; ++index) {
        if (heights[index] == low && heights[index] < limits[index].maximum) {
            ++heights[index];
            --remaining;
        }
    }
    return heights;
}

Rect rectangle(Length left, Length top, Length width, Length height) noexcept {
    // Вызывается лишь для подпрямоугольников валидированного work_area.
    return {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
            static_cast<std::int32_t>(left + width),
            static_cast<std::int32_t>(top + height)};
}

TilePlacement placement(const LayoutWindow& window, Rect slot) {
    return {window.id, slot,
            rectangle(slot.left, slot.top,
                      fit(slot.width(), window.constraints.width),
                      fit(slot.height(), window.constraints.height))};
}

} // namespace

bool valid_constraints(const SizeConstraints& constraints) noexcept {
    return valid_axis(constraints.width) && valid_axis(constraints.height);
}

LayoutResult solve_master_stack(Rect work_area, std::span<const LayoutWindow> windows,
                                MasterStackOptions options) {
    if (work_area.width() <= 0 || work_area.height() <= 0 ||
        work_area.width() > max_length || work_area.height() > max_length) {
        return failure(LayoutError::InvalidWorkArea);
    }
    if (options.outer_gap < 0 || options.inner_gap < 0 ||
        options.master_ratio_per_mille <= 0 || options.master_ratio_per_mille >= 1000 ||
        windows.size() > static_cast<std::size_t>(max_length)) {
        return failure(LayoutError::InvalidOptions);
    }
    std::unordered_set<WindowId> ids;
    for (const auto& window : windows) {
        if (window.id == 0) {
            return failure(LayoutError::InvalidWindowId);
        }
        if (!ids.insert(window.id).second) {
            return failure(LayoutError::DuplicateWindowId);
        }
        if (!valid_constraints(window.constraints)) {
            return failure(LayoutError::InvalidConstraints);
        }
        for (const auto* axis : {&window.constraints.width, &window.constraints.height}) {
            const auto limit = bounds(*axis);
            if (limit.minimum > limit.maximum) {
                return failure(LayoutError::LayoutInfeasible);
            }
        }
    }
    if (windows.empty()) {
        return {};
    }

    const Length width = work_area.width() - Length{2} * options.outer_gap;
    const Length height = work_area.height() - Length{2} * options.outer_gap;
    const Length left = Length{work_area.left} + options.outer_gap;
    const Length top = Length{work_area.top} + options.outer_gap;
    if (width <= 0 || height <= 0) {
        return failure(LayoutError::LayoutInfeasible);
    }
    if (windows.size() == 1) {
        if (fit(width, windows.front().constraints.width) == 0 ||
            fit(height, windows.front().constraints.height) == 0) {
            return failure(LayoutError::LayoutInfeasible);
        }
        return {LayoutError::None,
                {placement(windows.front(), rectangle(left, top, width, height))}};
    }

    const Length stack_count = static_cast<Length>(windows.size() - 1);
    const Length usable_width = width - options.inner_gap;
    const Length usable_height = height - (stack_count - 1) * options.inner_gap;
    if (usable_width < 2 || usable_height < stack_count ||
        fit(height, windows.front().constraints.height) == 0) {
        return failure(LayoutError::LayoutInfeasible);
    }

    const Length master_minimum = bounds(windows.front().constraints.width).minimum;
    Length stack_minimum = 1;
    Length minimum_height_sum = 0;
    std::vector<Bounds> stack_limits;
    stack_limits.reserve(windows.size() - 1);
    for (std::size_t index = 1; index < windows.size(); ++index) {
        stack_minimum = std::max(stack_minimum, bounds(windows[index].constraints.width).minimum);
        const auto limit = bounds(windows[index].constraints.height);
        minimum_height_sum += limit.minimum;
        stack_limits.push_back(limit);
    }
    if (master_minimum + stack_minimum > usable_width || minimum_height_sum > usable_height) {
        return failure(LayoutError::LayoutInfeasible);
    }
    const Length ideal_master = usable_width * options.master_ratio_per_mille / 1000;
    const Length master_width = std::clamp(ideal_master, master_minimum,
                                          usable_width - stack_minimum);
    const Length stack_width = usable_width - master_width;
    const auto heights = allocate_heights(usable_height, stack_limits);

    LayoutResult result;
    result.placements.reserve(windows.size());
    result.placements.push_back(placement(windows.front(),
                                          rectangle(left, top, master_width, height)));
    const Length stack_left = left + master_width + options.inner_gap;
    Length y = top;
    for (std::size_t index = 1; index < windows.size(); ++index) {
        const Length cell_height = heights[index - 1];
        result.placements.push_back(placement(windows[index],
            rectangle(stack_left, y, stack_width, cell_height)));
        y += cell_height + options.inner_gap;
    }
    return result;
}

} // namespace twm
