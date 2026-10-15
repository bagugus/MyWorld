#pragma once

#include "sim/Grid.h"
#include "sim/Map.h"

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace lw {

// Направления шага: сначала четыре по сторонам, затем четыре по диагонали.
inline constexpr int kNavDx[8] = {1, 0, -1, 0, 1, -1, -1, 1};
inline constexpr int kNavDy[8] = {0, 1, 0, -1, 1, 1, -1, -1};

// Вид клетки для графа навигации; определяется стоимостью клетки.
enum class NavClass : std::uint8_t { Blocked, Road, Land, Water };

// Проходимость и стоимость движения пешком по клеткам карты.
//
// Производные данные: собираются из слоёв карты и зданий, в хеш мира и в
// сохранения не входят. Пока здания не меняются, сетка строится один раз.
//
// Стоимость клетки — примерно во сколько раз идти по ней дольше, чем по дороге,
// умноженное на kRoadCost; у воды к времени добавлен штраф: холод, мокрые вещи,
// течение. Ноль — пройти нельзя: море, озеро, здание. Шаг между соседними
// клетками стоит
//   по стороне    kStraightFactor · (a + b),
//   по диагонали  kDiagonalFactor · (a + b + c + d),
// где a и b — клетки шага, c и d — клетки по бокам от угла, через который он
// проходит. Поэтому путь туда и обратно стоит одинаково, мимо угла непроходимой
// клетки по диагонали не протиснуться, а ручей шириной в клетку не перешагнуть
// даром. Отношение множителей (2 · 17 / 24 ≈ 1,4167) чуть больше √2: диагональ
// не дешевле прямого отрезка той же длины — на этом держится сглаживание пути.
class NavGrid {
public:
    static constexpr std::uint8_t kBlocked = 0;
    // Дорога и мост — самые дешёвые клетки.
    static constexpr std::uint8_t kRoadCost = 10;
    static constexpr std::uint32_t kStraightFactor = 24;
    static constexpr std::uint32_t kDiagonalFactor = 17;
    // Шаги по дороге — самые дешёвые: на них держится допустимая оценка A*.
    static constexpr std::uint32_t kRoadStraightStep = 2 * kStraightFactor * kRoadCost;
    static constexpr std::uint32_t kRoadDiagonalStep = 4 * kDiagonalFactor * kRoadCost;
    // Стоимость метра пути по дороге.
    static constexpr std::uint32_t kCostPerRoadMeter =
        kRoadStraightStep / static_cast<std::uint32_t>(Map::kCellSize);
    static_assert(kRoadStraightStep % static_cast<std::uint32_t>(Map::kCellSize) == 0);
    // Клетки не дешевле этого — вода (реки); дешевле — суша.
    static constexpr std::uint8_t kWaterCost = 30;

    static NavClass classOf(std::uint8_t cost) {
        return cost == kBlocked    ? NavClass::Blocked
               : cost == kRoadCost ? NavClass::Road
               : cost < kWaterCost ? NavClass::Land
                                   : NavClass::Water;
    }

    // Область — связный по сторонам набор проходимых клеток; из одной области в
    // другую не дойти. Связность по углам ничего не добавляет: диагональный шаг
    // требует проходимых клеток по бокам.
    using Region = std::uint16_t;
    static constexpr Region kNoRegion = 0; // у непроходимых клеток
    // Областей больше, чем вмещает тип: связность клетки неизвестна.
    static constexpr Region kUnknownRegion = 0xFFFF;

    // Собрать стоимости из слоёв карты и компонентов Building и разметить области.
    void build(const Map& map, const entt::registry& registry);

    int width() const { return cost_.width(); }
    int height() const { return cost_.height(); }
    bool contains(int x, int y) const { return cost_.contains(x, y); }
    bool contains(Cell cell) const { return contains(cell.x, cell.y); }

    // Стоимость клетки; за краем карты — kBlocked.
    std::uint8_t cost(int x, int y) const { return contains(x, y) ? cost_.at(x, y) : kBlocked; }
    std::uint8_t cost(Cell cell) const { return cost(cell.x, cell.y); }
    bool passable(Cell cell) const { return cost(cell) != kBlocked; }

    Region region(Cell cell) const {
        return contains(cell) ? region_.at(cell.x, cell.y) : kNoRegion;
    }
    std::size_t regionCount() const { return regionCount_; }
    // false — пути между клетками точно нет; true — есть (или неизвестно,
    // если одна из клеток в kUnknownRegion).
    bool connected(Cell a, Cell b) const;

    // Стоимость шага из проходимой клетки (x, y) в направлении d; 0 — шаг невозможен.
    std::uint32_t stepCost(int x, int y, int d) const;
    // Стоимость шага между соседними клетками; 0 — не соседи или шаг невозможен.
    std::uint32_t stepCost(Cell from, Cell to) const;

    // Все ли клетки под отрезком между центрами клеток проходимы.
    bool segmentPassable(Cell a, Cell b) const;

    // Ближайшая к cell проходимая клетка не дальше maxRadius клеток по каждой оси;
    // при равных расстояниях — верхняя, затем левая.
    std::optional<Cell> nearestPassable(Cell cell, int maxRadius) const;

private:
    void labelRegions();

    Grid<std::uint8_t> cost_;
    Grid<Region> region_;
    std::size_t regionCount_ = 0;
};

// Клетки, которых касается отрезок между центрами клеток a и b, по порядку от a
// к b. Если отрезок проходит ровно через угол, задеты обе клетки по бокам от него.
// visit(x, y) возвращает false, чтобы прервать обход; тогда и результат — false.
template <typename Visit>
bool forEachCellOnSegment(Cell a, Cell b, Visit&& visit) {
    const int dx = std::abs(b.x - a.x);
    const int dy = std::abs(b.y - a.y);
    const int sx = b.x > a.x ? 1 : -1;
    const int sy = b.y > a.y ? 1 : -1;
    int x = a.x;
    int y = a.y;
    if (!visit(x, y)) {
        return false;
    }
    // error — удвоенное смещение отрезка относительно угла следующей клетки.
    int error = dx - dy;
    for (int left = dx + dy; left > 0;) {
        if (error > 0) {
            x += sx;
            error -= 2 * dy;
            --left;
        } else if (error < 0) {
            y += sy;
            error += 2 * dx;
            --left;
        } else {
            if (!visit(x + sx, y) || !visit(x, y + sy)) {
                return false;
            }
            x += sx;
            y += sy;
            error += 2 * (dx - dy);
            left -= 2;
        }
        if (!visit(x, y)) {
            return false;
        }
    }
    return true;
}

inline std::uint32_t NavGrid::stepCost(int x, int y, int d) const {
    const int nx = x + kNavDx[d];
    const int ny = y + kNavDy[d];
    const std::uint32_t to = cost(nx, ny);
    if (to == kBlocked) {
        return 0;
    }
    const std::uint32_t from = cost_.at(x, y);
    if (d < 4) {
        return kStraightFactor * (from + to);
    }
    const std::uint32_t side1 = cost(nx, y);
    const std::uint32_t side2 = cost(x, ny);
    if (side1 == kBlocked || side2 == kBlocked) {
        return 0;
    }
    return kDiagonalFactor * (from + to + side1 + side2);
}

} // namespace lw
