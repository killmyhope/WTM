#include "twm/layout.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using namespace twm;
constexpr MasterStackOptions no_gaps{0, 0, 600};

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::vector<LayoutWindow> windows(std::size_t count) {
    std::vector<LayoutWindow> result;
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back({index + 1, {}});
    }
    return result;
}

bool inside(Rect inner, Rect outer) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool allowed(std::int64_t size, const AxisConstraints& limits) {
    return size > 0 && (!limits.minimum || size >= *limits.minimum) &&
        (!limits.maximum || size <= *limits.maximum) &&
        (!limits.step || (size >= limits.step->base &&
            (size - limits.step->base) % limits.step->increment == 0));
}

void invariants(Rect area, const std::vector<LayoutWindow>& input,
                const LayoutResult& result) {
    require(result.ok() && result.placements.size() == input.size(), "result size/status");
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto& tile = result.placements[index];
        require(tile.id == input[index].id, "solver changed input order");
        require(inside(tile.slot, area) && inside(tile.target, tile.slot), "bounds escaped slot/area");
        require(tile.target.left == tile.slot.left && tile.target.top == tile.slot.top,
                "target must retain top-left anchor");
        require(allowed(tile.target.width(), input[index].constraints.width) &&
                allowed(tile.target.height(), input[index].constraints.height), "constraint violated");
        for (std::size_t other = 0; other < index; ++other) {
            const auto& previous = result.placements[other].target;
            require(tile.target.right <= previous.left || previous.right <= tile.target.left ||
                    tile.target.bottom <= previous.top || previous.bottom <= tile.target.top,
                    "targets overlap");
        }
    }
}

void basic_geometry() {
    require(solve_master_stack({0, 0, 1000, 900}, {}).ok(), "empty layout");
    const auto one = windows(1);
    const auto single = solve_master_stack({-1000, 50, 0, 950}, one, {10, 8, 600});
    invariants({-1000, 50, 0, 950}, one, single);
    require(single.placements[0].target == Rect{-990, 60, -10, 940}, "single tile gaps");

    auto input = windows(4);
    const auto regular = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    invariants({0, 0, 1000, 900}, input, regular);
    require(regular.placements[0].target == Rect{0, 0, 600, 900}, "master ratio");
    require(regular.placements[1].target == Rect{600, 0, 1000, 300}, "first stack tile");
    require(regular.placements[3].target == Rect{600, 600, 1000, 900}, "last stack tile");

    input = windows(5);
    const auto odd = solve_master_stack({-101, -51, 900, 852}, input, {3, 5, 600});
    invariants({-101, -51, 900, 852}, input, odd);
    require(odd.placements.back().target.bottom == 849, "division remainder lost");
    for (std::size_t index = 2; index < input.size(); ++index) {
        require(odd.placements[index].slot.top - odd.placements[index - 1].slot.bottom == 5,
                "inner gap changed");
        require(odd.placements[index - 1].slot.height() >= odd.placements[index].slot.height(),
                "remainders must go to earlier cells");
    }
    require(odd.placements == solve_master_stack({-101, -51, 900, 852}, input,
                                                 {3, 5, 600}).placements, "not deterministic");
}

void minimums_and_cascade() {
    auto input = windows(4);
    ConstraintRefinementBudget budget(input.size() - 1);
    require(budget.try_consume(), "first refinement");
    input[1].constraints.height.minimum = 400;
    auto result = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    invariants({0, 0, 1000, 900}, input, result);
    require(result.placements[1].target.height() == 400 &&
            result.placements[2].target.height() == 250 &&
            result.placements[3].target.height() == 250, "minimum did not redistribute stack");
    const auto odd = solve_master_stack({0, 0, 1000, 901}, input, no_gaps);
    require(odd.ok() && odd.placements[1].target.height() == 400 &&
            odd.placements[2].target.height() == 251 && odd.placements[3].target.height() == 250,
            "remainder must go to a free cell, not a window frozen at its larger minimum");
    require(budget.try_consume(), "second refinement");
    input[2].constraints.height.minimum = 280;
    result = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    require(result.ok() && result.placements[3].target.height() == 220, "cascade did not reflow");
    require(budget.try_consume(), "third refinement");
    input[3].constraints.height.minimum = 240;
    result = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    require(result.error == LayoutError::LayoutInfeasible && result.placements.empty(),
            "impossible minimums must not produce a partial layout");
    require(!budget.try_consume() && !budget.try_consume() && budget.used() == budget.limit(),
            "cascade budget must remain exhausted");
    ConstraintRefinementBudget master_only(0);
    require(master_only.try_consume() && !master_only.try_consume(), "master-only budget");

    input = windows(2);
    input[1].constraints.width.minimum = 550;
    result = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    require(result.ok() && result.placements[0].slot.width() == 450, "width constraint must adjust ratio");
    input[0].constraints.width.minimum = 600;
    require(solve_master_stack({0, 0, 1000, 900}, input, no_gaps).error ==
            LayoutError::LayoutInfeasible, "incompatible column minimums");
}

void maximums_and_steps() {
    auto input = windows(3);
    input[1].constraints.height.maximum = 200;
    auto result = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    invariants({0, 0, 1000, 900}, input, result);
    require(result.placements[1].target.height() == 200 &&
            result.placements[2].target.height() == 700, "max slack not redistributed");
    input[2].constraints.height.maximum = 200;
    result = solve_master_stack({0, 0, 1000, 900}, input, no_gaps);
    require(result.ok() && result.placements.back().target.bottom == 400,
            "all-capped windows should leave unused space");

    input = windows(1);
    input[0].constraints.width.maximum = 800;
    input[0].constraints.height.step = SizeStep{7, 0};
    result = solve_master_stack({0, 0, 1000, 300}, input, no_gaps);
    invariants({0, 0, 1000, 300}, input, result);
    require(result.placements[0].slot.height() == 300 &&
            result.placements[0].target.height() == 294 &&
            result.placements[0].target.width() == 800, "step/max must fit inside stable slot");
    require(result.placements == solve_master_stack({0, 0, 1000, 300}, input, no_gaps).placements,
            "step must not feed a new minimum or cause drift");
    input[0].constraints.height.minimum = 295;
    input[0].constraints.height.maximum = 299;
    require(solve_master_stack({0, 0, 1000, 300}, input, no_gaps).error ==
            LayoutError::LayoutInfeasible, "step grid has no feasible point");
    input[0].constraints.height = {{}, {}, SizeStep{16, 30}};
    result = solve_master_stack({0, 0, 1000, 300}, input, no_gaps);
    require(result.ok() && result.placements[0].target.height() == 286, "step base ignored");

    input = windows(3);
    input[0].constraints.width.step = SizeStep{13, 2};
    input[1].constraints.width.maximum = 150;
    input[2].constraints.width = {121, {}, SizeStep{11, 0}};
    result = solve_master_stack({-1000, -200, 0, 700}, input, {9, 7, 600});
    invariants({-1000, -200, 0, 700}, input, result);
    require(result.placements[1].target.width() == 150, "narrow window must retain common column slot");
}

void invalid_inputs_and_extremes() {
    auto input = windows(2);
    const auto expect = [&](Rect area, MasterStackOptions options, LayoutError error) {
        const auto result = solve_master_stack(area, input, options);
        require(result.error == error && result.placements.empty(), "invalid input accepted/partial output");
    };
    expect({0, 0, 0, 100}, no_gaps, LayoutError::InvalidWorkArea);
    expect({0, 0, 100, 100}, {-1, 0, 600}, LayoutError::InvalidOptions);
    expect({0, 0, 100, 100}, {0, 0, 1000}, LayoutError::InvalidOptions);
    expect({0, 0, 1, 1}, no_gaps, LayoutError::LayoutInfeasible);
    expect({0, 0, 100, 100}, {60, 0, 600}, LayoutError::LayoutInfeasible);
    input[1].id = 1;
    expect({0, 0, 100, 100}, no_gaps, LayoutError::DuplicateWindowId);
    input[1].id = 0;
    expect({0, 0, 100, 100}, no_gaps, LayoutError::InvalidWindowId);
    input[1].id = 2;
    input[1].constraints.height = {300, 200, {}};
    expect({0, 0, 100, 100}, no_gaps, LayoutError::InvalidConstraints);
    input[1].constraints.height = {{}, {}, SizeStep{0, 0}};
    expect({0, 0, 100, 100}, no_gaps, LayoutError::InvalidConstraints);
    input[1].constraints.height = {{}, {}, SizeStep{4, -1}};
    expect({0, 0, 100, 100}, no_gaps, LayoutError::InvalidConstraints);
    input = windows(2);
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    expect({minimum, 0, maximum, 100}, no_gaps, LayoutError::InvalidWorkArea);
    for (const Rect area : {Rect{minimum, minimum, minimum + 1000, minimum + 900},
                            Rect{maximum - 1000, maximum - 900, maximum, maximum}}) {
        invariants(area, input, solve_master_stack(area, input, no_gaps));
    }
    expect({0, 0, 100, 100}, {maximum, maximum, 999}, LayoutError::LayoutInfeasible);
}

void small_domain_oracle() {
    // Независимый перебор допустимых длин проверяет feasibility, в том числе grid.
    std::mt19937 random(0xB5F123u);
    for (unsigned trial = 0; trial < 1500; ++trial) {
        const int height = 1 + static_cast<int>(random() % 50);
        auto input = windows(2 + random() % 5);
        int minimum_sum = 0;
        bool possible = true;
        for (std::size_t index = 1; index < input.size(); ++index) {
            auto& axis = input[index].constraints.height;
            axis.minimum = 1 + static_cast<int>(random() % 10);
            axis.maximum = *axis.minimum + static_cast<int>(random() % 20);
            if (random() % 2 == 0) {
                axis.step = SizeStep{1 + static_cast<int>(random() % 5),
                                     static_cast<int>(random() % 4)};
            }
            int smallest = 0;
            for (int candidate = 1; candidate <= height; ++candidate) {
                if (allowed(candidate, axis)) { smallest = candidate; break; }
            }
            possible = possible && smallest != 0;
            minimum_sum += smallest;
        }
        possible = possible && minimum_sum <= height;
        const Rect area{0, -10, 100, height - 10};
        const auto result = solve_master_stack(area, input, no_gaps);
        require(result.ok() == possible, "feasibility disagrees with brute-force oracle");
        if (possible) {
            invariants(area, input, result);
        } else {
            require(result.error == LayoutError::LayoutInfeasible && result.placements.empty(),
                    "oracle rejection must have no partial result");
        }
    }
}

} // namespace

int main() {
    try {
        basic_geometry();
        minimums_and_cascade();
        maximums_and_steps();
        invalid_inputs_and_extremes();
        small_domain_oracle();
        std::cout << "Master-Stack numeric tests passed (including 1500 oracle scenarios).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
