#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace twm {

// Уникальный номер жизни окна. Ноль недопустим; закрытые ID не переиспользуются.
using WindowId = std::uint64_t;

struct Rect {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};

    [[nodiscard]] std::int64_t width() const noexcept {
        return std::int64_t{right} - left;
    }
    [[nodiscard]] std::int64_t height() const noexcept {
        return std::int64_t{bottom} - top;
    }
    bool operator==(const Rect&) const = default;
};

// Допустимая длина = base + k * increment, k >= 0. Например, рамка + строки.
// Координаты и ограничения уже переведены адаптером в физические visible bounds.
struct SizeStep {
    std::int32_t increment{};
    std::int32_t base{};
    bool operator==(const SizeStep&) const = default;
};

struct AxisConstraints {
    std::optional<std::int32_t> minimum;
    std::optional<std::int32_t> maximum;
    std::optional<SizeStep> step;
    bool operator==(const AxisConstraints&) const = default;
};

struct SizeConstraints {
    AxisConstraints width;
    AxisConstraints height;
    bool operator==(const SizeConstraints&) const = default;
};

[[nodiscard]] bool valid_constraints(const SizeConstraints& constraints) noexcept;

struct LayoutWindow {
    WindowId id{};
    SizeConstraints constraints;
};

struct MasterStackOptions {
    std::int32_t outer_gap = 8;
    std::int32_t inner_gap = 8;
    std::int32_t master_ratio_per_mille = 600;
};

struct TilePlacement {
    WindowId id;
    Rect slot;   // Ячейка раскладки; step/max могут оставить свободное место.
    Rect target; // Канонический видимый прямоугольник окна, anchor top-left.
    bool operator==(const TilePlacement&) const = default;
};

enum class LayoutError {
    None,
    InvalidWorkArea,
    InvalidOptions,
    InvalidWindowId,
    DuplicateWindowId,
    InvalidConstraints,
    LayoutInfeasible
};

struct LayoutResult {
    LayoutError error = LayoutError::None;
    std::vector<TilePlacement> placements;
    [[nodiscard]] bool ok() const noexcept { return error == LayoutError::None; }
};

// Чистая функция: никаких HWND, DPI API, запросов геометрии и изменения окон.
// Ошибка всегда возвращается с пустым placements, без частичной раскладки.
[[nodiscard]] LayoutResult solve_master_stack(
    Rect work_area, std::span<const LayoutWindow> windows,
    MasterStackOptions options = {});

// Контроллер хранит один бюджет на внешнюю эпоху (topology/набор окон/действие
// пользователя). Новая версия layout или очередное уточнение его НЕ сбрасывает.
// Даже единственному master разрешается одна попытка уточнения.
class ConstraintRefinementBudget {
public:
    explicit ConstraintRefinementBudget(std::size_t stack_windows) noexcept
        : limit_(stack_windows == 0 ? 1 : stack_windows) {}
    [[nodiscard]] bool try_consume() noexcept {
        if (used_ == limit_) {
            return false;
        }
        ++used_;
        return true;
    }
    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
private:
    std::size_t limit_;
    std::size_t used_ = 0;
};

} // namespace twm
