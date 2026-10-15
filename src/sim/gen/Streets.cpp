// Улицы и здания внутри поселений.

#include "sim/gen/GenContext.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lw::gen {

namespace {

// --- Улицы ------------------------------------------------------------------
constexpr int kMinBlock = 12; // сторона городского квартала, клетки
constexpr int kMaxBlock = 18;
constexpr int kStreetMaxSlope = 22; // проценты
constexpr int kStreetMaxBridge = 3; // клеток реки под одним отрезком улицы
constexpr float kBlockDropout = 0.08f;     // доля пропущенных улиц в центре города
constexpr float kOutskirtsDropout = 0.35f; // прибавка к ней на окраине
constexpr float kMainStreetReach = 0.85f;  // длина главной улицы деревни, доли радиуса
constexpr float kLaneZone = 0.8f;          // переулки начинаются не дальше этой доли радиуса
constexpr float kWobble = 0.12f;           // как часто деревенская улица виляет

// --- Здания -----------------------------------------------------------------
constexpr float kTownDensity = 0.55f;      // вероятность застроить участок в центре города
constexpr float kOutskirtsThinning = 0.3f; // на сколько она падает к окраине
constexpr float kVillageDensity = 0.25f;
constexpr float kDowntown = 0.5f; // доля радиуса: ближе — многоэтажки и магазины
constexpr int kHouseRelief = 12;  // допустимый перепад высот под зданием, единицы kElevationStep
constexpr int kLargeRelief = 20;

int distanceSq(Cell a, Cell b) {
    const int dx = a.x - b.x;
    const int dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Доля радиуса: 0 в центре поселения, 1 на границе.
float centrality(const SettlementSite& site, Cell cell) {
    return std::sqrt(static_cast<float>(distanceSq(site.center, cell))) /
           static_cast<float>(site.radius);
}

// Можно ли провести улицу через клетку.
bool streetAllowed(const Context& ctx, const SettlementSite& site, Cell cell) {
    return ctx.map.contains(cell) && distanceSq(site.center, cell) <= site.radius * site.radius &&
           !isOpenWater(ctx.map.water.at(cell.x, cell.y)) &&
           ctx.slope.at(cell.x, cell.y) <= kStreetMaxSlope;
}

// Город: прямоугольная сетка кварталов с пропусками, реже к окраине.
void buildTownStreets(Context& ctx, const SettlementSite& site) {
    Rng& rng = ctx.rng;
    const int stepX = rng.rangeInt(kMinBlock, kMaxBlock);
    const int stepY = rng.rangeInt(kMinBlock, kMaxBlock);
    const int halfX = site.radius / stepX;
    const int halfY = site.radius / stepY;
    const int cols = 2 * halfX + 1;
    const int rows = 2 * halfY + 1;

    // Перекрёсток (i, j); центральный — (halfX, halfY) — совпадает с центром города.
    const auto crossing = [&](int i, int j) {
        return Cell{site.center.x + (i - halfX) * stepX, site.center.y + (j - halfY) * stepY};
    };
    // Отрезок улицы между соседними перекрёстками годится, если весь проходим.
    const auto usable = [&](Cell from, Cell to) {
        const int dx = (to.x > from.x) - (to.x < from.x);
        const int dy = (to.y > from.y) - (to.y < from.y);
        int bridge = 0;
        for (Cell cell = from;; cell = {cell.x + dx, cell.y + dy}) {
            if (!streetAllowed(ctx, site, cell)) {
                return false;
            }
            bridge += ctx.map.water.at(cell.x, cell.y) == Water::River ? 1 : 0;
            if (cell == to) {
                break;
            }
        }
        return bridge <= kStreetMaxBridge;
    };
    const auto keep = [&](int i, int j, int ni, int nj) {
        if (ni >= cols || nj >= rows || !usable(crossing(i, j), crossing(ni, nj))) {
            return false;
        }
        // Улицы от центрального перекрёстка есть всегда.
        if ((i == halfX && j == halfY) || (ni == halfX && nj == halfY)) {
            return true;
        }
        const float outward = centrality(site, crossing(ni, nj));
        return !rng.chance(kBlockDropout + kOutskirtsDropout * outward * outward);
    };

    Grid<std::uint8_t> east(cols, rows);  // есть улица к перекрёстку (i + 1, j)
    Grid<std::uint8_t> south(cols, rows); // к перекрёстку (i, j + 1)
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            east.at(i, j) = keep(i, j, i + 1, j) ? 1 : 0;
            south.at(i, j) = keep(i, j, i, j + 1) ? 1 : 0;
        }
    }

    // Оставляем только улицы, по которым можно доехать до центра.
    Grid<std::uint8_t> reached(cols, rows);
    std::vector<Cell> queue = {{halfX, halfY}};
    reached.at(halfX, halfY) = 1;
    const auto pave = [&](Cell from, Cell to) {
        const int dx = (to.x > from.x) - (to.x < from.x);
        const int dy = (to.y > from.y) - (to.y < from.y);
        for (Cell cell = from;; cell = {cell.x + dx, cell.y + dy}) {
            setRoad(ctx.map, cell.x, cell.y, Road::Asphalt);
            if (cell == to) {
                break;
            }
        }
    };
    for (std::size_t k = 0; k < queue.size(); ++k) {
        const Cell node = queue[k];
        const Cell neighbors[4] = {{node.x + 1, node.y}, {node.x, node.y + 1},
                                   {node.x - 1, node.y}, {node.x, node.y - 1}};
        for (const Cell next : neighbors) {
            if (!reached.contains(next.x, next.y)) {
                continue;
            }
            const bool linked = next.x > node.x   ? east.at(node.x, node.y)
                                : next.x < node.x ? east.at(next.x, next.y)
                                : next.y > node.y ? south.at(node.x, node.y)
                                                  : south.at(next.x, next.y);
            if (!linked) {
                continue;
            }
            pave(crossing(node.x, node.y), crossing(next.x, next.y));
            if (!reached.at(next.x, next.y)) {
                reached.at(next.x, next.y) = 1;
                queue.push_back(next);
            }
        }
    }
}

// Грунтовая улица из start в сторону direction (номер из kDx/kDy), слегка
// виляющая. Останавливается у воды, обрыва и границы поселения, а с
// stopAtRoad — ещё и влившись в другую дорогу.
void drawLane(Context& ctx, const SettlementSite& site, Cell start, int direction, int length,
              bool stopAtRoad) {
    Rng& rng = ctx.rng;
    const auto allowed = [&](Cell cell) {
        return streetAllowed(ctx, site, cell) && ctx.map.water.at(cell.x, cell.y) == Water::None;
    };

    Cell cell = start;
    for (int step = 0; step < length; ++step) {
        if (rng.chance(kWobble)) {
            const int side = (direction + (rng.chance(0.5f) ? 1 : 3)) % 4;
            const Cell shifted{cell.x + kDx[side], cell.y + kDy[side]};
            if (allowed(shifted)) {
                setRoad(ctx.map, shifted.x, shifted.y, Road::Dirt);
                cell = shifted;
            }
        }
        const Cell next{cell.x + kDx[direction], cell.y + kDy[direction]};
        if (!allowed(next)) {
            break;
        }
        const bool joined = ctx.map.road.at(next.x, next.y) != Road::None;
        setRoad(ctx.map, next.x, next.y, Road::Dirt);
        cell = next;
        if (joined && stopAtRoad) {
            break;
        }
    }
}

// Деревня: главная улица через центр и несколько переулков от дорог.
void buildVillageStreets(Context& ctx, const SettlementSite& site) {
    Rng& rng = ctx.rng;
    const int axis = rng.chance(0.5f) ? 0 : 1;
    const int reach = static_cast<int>(static_cast<float>(site.radius) * kMainStreetReach);
    drawLane(ctx, site, site.center, axis, reach, false);
    drawLane(ctx, site, site.center, axis + 2, reach, false);

    const int lanes = rng.rangeInt(2, 4);
    const int zone = static_cast<int>(static_cast<float>(site.radius) * kLaneZone);
    std::vector<Cell> roadCells;
    for (int n = 0; n < lanes; ++n) {
        roadCells.clear();
        for (int y = site.center.y - zone; y <= site.center.y + zone; ++y) {
            for (int x = site.center.x - zone; x <= site.center.x + zone; ++x) {
                const Cell cell{x, y};
                if (ctx.map.contains(cell) && distanceSq(site.center, cell) <= zone * zone &&
                    ctx.map.road.at(x, y) != Road::None) {
                    roadCells.push_back(cell);
                }
            }
        }
        if (roadCells.empty()) {
            break;
        }
        const Cell start = roadCells[rng.below(static_cast<std::uint32_t>(roadCells.size()))];
        const int first = static_cast<int>(rng.below(4));
        const int length = rng.rangeInt(6, 16);
        // Переулок уходит в ту сторону, где дороги ещё нет.
        for (int turn = 0; turn < 4; ++turn) {
            const int direction = (first + turn) % 4;
            const Cell next{start.x + kDx[direction], start.y + kDy[direction]};
            if (ctx.map.contains(next) && ctx.map.road.at(next.x, next.y) == Road::None) {
                drawLane(ctx, site, start, direction, length, true);
                break;
            }
        }
    }
}

struct Plan {
    BuildingKind kind = BuildingKind::House;
    int frontage = 0; // клетки вдоль дороги
    int depth = 0;    // клетки вглубь участка
    int maxRelief = kHouseRelief;
};

Plan chooseBuilding(Rng& rng, const SettlementSite& site, float outward) {
    const float roll = rng.unit();
    if (site.kind == SettlementKind::Town) {
        if (outward < kDowntown && roll < 0.3f) {
            return {BuildingKind::Apartment, rng.rangeInt(6, 8), 3, kLargeRelief};
        }
        if (roll < (outward < kDowntown ? 0.45f : 0.05f)) {
            return {BuildingKind::Shop, rng.rangeInt(3, 4), 3, kLargeRelief};
        }
    } else if (roll < 0.15f) {
        return {BuildingKind::Barn, rng.rangeInt(3, 5), 3, kLargeRelief};
    }
    const int frontage = rng.rangeInt(2, 3);
    return {BuildingKind::House, frontage, frontage == 2 ? 3 : rng.rangeInt(2, 3), kHouseRelief};
}

// Свободен ли прямоугольник [x0, x1] × [y0, y1] под здание.
bool fits(const Context& ctx, const SettlementSite& site, int x0, int y0, int x1, int y1,
          int maxRelief) {
    const Map& map = ctx.map;
    if (x0 < 1 || y0 < 1 || x1 >= ctx.width - 1 || y1 >= ctx.height - 1) {
        return false;
    }
    if (distanceSq(site.center, {(x0 + x1) / 2, (y0 + y1) / 2}) > site.radius * site.radius) {
        return false;
    }
    int low = map.elevation.at(x0, y0);
    int high = low;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (map.water.at(x, y) != Water::None || map.road.at(x, y) != Road::None ||
                map.ground.at(x, y) == Ground::Rock) {
                return false;
            }
            low = std::min<int>(low, map.elevation.at(x, y));
            high = std::max<int>(high, map.elevation.at(x, y));
        }
    }
    if (high - low > maxRelief) {
        return false;
    }
    // Между зданиями остаётся проход хотя бы в клетку.
    for (int y = y0 - 1; y <= y1 + 1; ++y) {
        for (int x = x0 - 1; x <= x1 + 1; ++x) {
            if (ctx.occupied.at(x, y)) {
                return false;
            }
        }
    }
    return true;
}

void placeBuildingsIn(Context& ctx, int siteIndex) {
    Map& map = ctx.map;
    Rng& rng = ctx.rng;
    const SettlementSite& site = ctx.out.settlements[static_cast<std::size_t>(siteIndex)];
    const int radius = site.radius;

    // Идём вдоль всех дорог поселения и пробуем застроить обе стороны.
    for (int y = std::max(site.center.y - radius, 0); y <= std::min(site.center.y + radius, ctx.height - 1); ++y) {
        for (int x = std::max(site.center.x - radius, 0); x <= std::min(site.center.x + radius, ctx.width - 1); ++x) {
            if (map.road.at(x, y) == Road::None || distanceSq(site.center, {x, y}) > radius * radius) {
                continue;
            }
            for (int side = 0; side < 4; ++side) {
                const int dx = kDx[side];
                const int dy = kDy[side];
                // Клетка между дорогой и зданием — двор; на неё выходит дверь.
                const Cell door{x + dx, y + dy};
                if (!map.contains(door) || map.road.at(door.x, door.y) != Road::None ||
                    map.water.at(door.x, door.y) != Water::None || ctx.occupied.at(door.x, door.y)) {
                    continue;
                }
                const float outward = centrality(site, door);
                const float density = site.kind == SettlementKind::Town
                                          ? kTownDensity - kOutskirtsThinning * outward
                                          : kVillageDensity;
                if (!rng.chance(density)) {
                    continue;
                }

                const Plan plan = chooseBuilding(rng, site, outward);
                // Фасад идёт вдоль дороги, то есть поперёк направления side.
                const int alongX = -dy;
                const int alongY = dx;
                const int first = -(plan.frontage / 2);
                const int last = first + plan.frontage - 1;
                const Cell a{door.x + dx + alongX * first, door.y + dy + alongY * first};
                const Cell b{door.x + dx * plan.depth + alongX * last,
                             door.y + dy * plan.depth + alongY * last};
                const int x0 = std::min(a.x, b.x);
                const int x1 = std::max(a.x, b.x);
                const int y0 = std::min(a.y, b.y);
                const int y1 = std::max(a.y, b.y);
                if (!fits(ctx, site, x0, y0, x1, y1, plan.maxRelief)) {
                    continue;
                }

                for (int by = y0; by <= y1; ++by) {
                    for (int bx = x0; bx <= x1; ++bx) {
                        ctx.occupied.at(bx, by) = 1;
                    }
                }
                for (int by = y0 - 1; by <= y1 + 1; ++by) {
                    for (int bx = x0 - 1; bx <= x1 + 1; ++bx) {
                        map.cover.at(bx, by) = Cover::None;
                    }
                }
                BuildingSite building;
                building.kind = plan.kind;
                building.origin = {x0, y0};
                building.width = x1 - x0 + 1;
                building.height = y1 - y0 + 1;
                building.door = door;
                building.settlement = siteIndex;
                ctx.out.buildings.push_back(building);
            }
        }
    }
}

} // namespace

void buildStreets(Context& ctx) {
    for (const SettlementSite& site : ctx.out.settlements) {
        if (site.kind == SettlementKind::Town) {
            buildTownStreets(ctx, site);
        } else {
            buildVillageStreets(ctx, site);
        }
    }
}

void placeBuildings(Context& ctx) {
    for (int i = 0; i < static_cast<int>(ctx.out.settlements.size()); ++i) {
        placeBuildingsIn(ctx, i);
    }
}

// Лес не подступает к дороге вплотную.
void clearRoadsides(Context& ctx) {
    Map& map = ctx.map;
    for (int y = 0; y < ctx.height; ++y) {
        for (int x = 0; x < ctx.width; ++x) {
            if (map.road.at(x, y) == Road::None) {
                continue;
            }
            for (int d = 0; d < 8; ++d) {
                const Cell next{x + kDx[d], y + kDy[d]};
                if (map.contains(next)) {
                    map.cover.at(next.x, next.y) = Cover::None;
                }
            }
        }
    }
}

} // namespace lw::gen
