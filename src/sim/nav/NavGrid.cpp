#include "sim/nav/NavGrid.h"

#include "sim/Components.h"
#include "sim/Parallel.h"

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
    // Клетки независимы — строками параллельно (этап 2а.2).
    parallelFor(static_cast<std::size_t>(height), 64, 128, [&](std::size_t begin, std::size_t end, int) {
        for (std::size_t y = begin; y < end; ++y) {
            for (int x = 0; x < width; ++x) {
                cost_.at(x, static_cast<int>(y)) = terrainCost(map, x, static_cast<int>(y));
            }
        }
    });
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

    // Области — по отрезкам строк (этап 2а.2; на 16 км обход в ширину по
    // клеткам занимал четверть секунды): проходимые отрезки строки
    // объединяются с отрезками строки выше, которых касаются сбоку
    // (связность по сторонам), затем номера раздаются по порядку первого
    // отрезка области при обходе строк — так же, как раньше раздавал обход в
    // ширину от первой клетки области. Разметка зависит только от сетки.
    struct Run {
        int y = 0;
        int x0 = 0; // клетки [x0, x1)
        int x1 = 0;
    };
    std::vector<Run> runs;
    std::vector<std::uint32_t> parent;
    const auto find = [&parent](std::uint32_t i) {
        std::uint32_t root = i;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[i] != root) {
            const std::uint32_t up = parent[i];
            parent[i] = root;
            i = up;
        }
        return root;
    };
    const auto unite = [&](std::uint32_t a, std::uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            // Корень — меньший номер: порядок объединений на результат не влияет.
            parent[std::max(a, b)] = std::min(a, b);
        }
    };
    std::size_t above = 0; // первый отрезок строки выше
    for (int y = 0; y < height; ++y) {
        const std::size_t rowStart = runs.size();
        for (int x = 0; x < width;) {
            if (cost_.at(x, y) == kBlocked) {
                ++x;
                continue;
            }
            Run run{y, x, x};
            while (run.x1 < width && cost_.at(run.x1, y) != kBlocked) {
                ++run.x1;
            }
            const auto index = static_cast<std::uint32_t>(runs.size());
            runs.push_back(run);
            parent.push_back(index);
            // Отрезки строки выше, которые этот касается сбоку: пересекаются по x.
            while (above < rowStart && runs[above].x1 <= run.x0) {
                ++above;
            }
            for (std::size_t k = above; k < rowStart && runs[k].x0 < run.x1; ++k) {
                unite(index, static_cast<std::uint32_t>(k));
            }
            x = run.x1;
        }
        above = rowStart;
    }

    std::vector<Region> idOf(runs.size(), kNoRegion); // по корню
    Region next = 1;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const std::uint32_t root = find(static_cast<std::uint32_t>(i));
        if (idOf[root] == kNoRegion) {
            idOf[root] = next;
            if (next != kUnknownRegion) {
                ++next;
            }
            ++regionCount_;
        }
        const Run& run = runs[i];
        for (int x = run.x0; x < run.x1; ++x) {
            region_.at(x, run.y) = idOf[root];
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
