#include "sim/nav/NavGrid.h"

#include "sim/Components.h"

#include <algorithm>
#include <vector>

namespace lw {

namespace {

// Стоимость клетки — во сколько раз идти дольше, чем по дороге (× kRoadCost).
// Только у дороги она равна kRoadCost, у суши — меньше kWaterCost: по ней
// граф навигации отличает дорогу, сушу и воду.
constexpr std::uint8_t kCostGrass = 11;
constexpr std::uint8_t kCostField = 13; // пашня: борозды, грязь
constexpr std::uint8_t kCostSand = 13;
constexpr std::uint8_t kCostForest = 15;
constexpr std::uint8_t kCostRock = 22; // камень лежит на крутых склонах
// Реки: вброд, по грудь, вплавь. Штраф больше потерянного времени: мост в
// стороне обычно лучше, но ради большого крюка реку переплывают.
constexpr std::uint8_t kCostFord = NavGrid::kWaterCost;
constexpr std::uint8_t kCostWade = 60;
constexpr std::uint8_t kCostSwim = 250;
static_assert(kCostGrass > NavGrid::kRoadCost && kCostRock < NavGrid::kWaterCost);
constexpr int kFordDepth = 5;  // до 0,5 м (единицы Map::kDepthStep): ручей
constexpr int kWadeDepth = 15; // до 1,5 м

std::uint8_t terrainCost(const Map& map, int x, int y) {
    // Дорога на воде — мост.
    if (map.road.at(x, y) != Road::None) {
        return NavGrid::kRoadCost;
    }
    switch (map.water.at(x, y)) {
    case Water::Sea:
    case Water::Lake:
        return NavGrid::kBlocked;
    case Water::River: {
        const int depth = map.depth.at(x, y);
        return depth <= kFordDepth ? kCostFord : depth <= kWadeDepth ? kCostWade : kCostSwim;
    }
    case Water::None:
        break;
    }
    std::uint8_t cost = kCostGrass;
    switch (map.ground.at(x, y)) {
    case Ground::Grass:
        cost = kCostGrass;
        break;
    case Ground::Sand:
        cost = kCostSand;
        break;
    case Ground::Rock:
        cost = kCostRock;
        break;
    case Ground::Field:
        cost = kCostField;
        break;
    }
    if (map.cover.at(x, y) == Cover::Forest) {
        cost = std::max(cost, kCostForest);
    }
    return cost;
}

} // namespace

void NavGrid::build(const Map& map, const entt::registry& registry) {
    const int width = map.width();
    const int height = map.height();
    cost_ = Grid<std::uint8_t>(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            cost_.at(x, y) = terrainCost(map, x, y);
        }
    }
    // Внутренности зданий не моделируются: в здание входят до клетки двери.
    for (const auto [entity, building] : registry.view<const Building>().each()) {
        for (int y = building.origin.y; y < building.origin.y + building.height; ++y) {
            for (int x = building.origin.x; x < building.origin.x + building.width; ++x) {
                if (contains(x, y)) {
                    cost_.at(x, y) = kBlocked;
                }
            }
        }
    }
    labelRegions();
}

void NavGrid::labelRegions() {
    const int width = cost_.width();
    const int height = cost_.height();
    region_ = Grid<Region>(width, height, kNoRegion);
    regionCount_ = 0;

    // Номера по порядку обхода строк: разметка зависит только от сетки.
    std::vector<Cell> queue;
    Region next = 1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (cost_.at(x, y) == kBlocked || region_.at(x, y) != kNoRegion) {
                continue;
            }
            const Region id = next;
            if (next != kUnknownRegion) {
                ++next;
            }
            ++regionCount_;
            region_.at(x, y) = id;
            queue.clear();
            queue.push_back({x, y});
            for (std::size_t k = 0; k < queue.size(); ++k) {
                const Cell cell = queue[k];
                for (int d = 0; d < 4; ++d) {
                    const int nx = cell.x + kNavDx[d];
                    const int ny = cell.y + kNavDy[d];
                    if (contains(nx, ny) && cost_.at(nx, ny) != kBlocked &&
                        region_.at(nx, ny) == kNoRegion) {
                        region_.at(nx, ny) = id;
                        queue.push_back({nx, ny});
                    }
                }
            }
        }
    }
}

bool NavGrid::connected(Cell a, Cell b) const {
    const Region ra = region(a);
    const Region rb = region(b);
    if (ra == kNoRegion || rb == kNoRegion) {
        return false;
    }
    return ra == rb || ra == kUnknownRegion || rb == kUnknownRegion;
}

std::uint32_t NavGrid::stepCost(Cell from, Cell to) const {
    for (int d = 0; d < 8; ++d) {
        if (from.x + kNavDx[d] == to.x && from.y + kNavDy[d] == to.y) {
            return passable(from) ? stepCost(from.x, from.y, d) : 0;
        }
    }
    return 0;
}

bool NavGrid::segmentPassable(Cell a, Cell b) const {
    return forEachCellOnSegment(a, b, [this](int x, int y) { return cost(x, y) != kBlocked; });
}

std::optional<Cell> NavGrid::nearestPassable(Cell cell, int maxRadius) const {
    std::optional<Cell> best;
    long long bestSq = 0;
    for (int ring = 0; ring <= maxRadius; ++ring) {
        // Клетки кольца не ближе ring: дальше искать незачем.
        if (best && bestSq < static_cast<long long>(ring) * ring) {
            break;
        }
        for (int y = cell.y - ring; y <= cell.y + ring; ++y) {
            // В средних строках кольца — только две крайние клетки.
            const bool edgeRow = y == cell.y - ring || y == cell.y + ring;
            const int step = edgeRow ? 1 : 2 * ring;
            for (int x = cell.x - ring; x <= cell.x + ring; x += step) {
                if (!passable({x, y})) {
                    continue;
                }
                const long long dx = x - cell.x;
                const long long dy = y - cell.y;
                const long long distanceSq = dx * dx + dy * dy;
                if (!best || distanceSq < bestSq ||
                    (distanceSq == bestSq && (y < best->y || (y == best->y && x < best->x)))) {
                    best = Cell{x, y};
                    bestSq = distanceSq;
                }
            }
        }
    }
    return best;
}

} // namespace lw
