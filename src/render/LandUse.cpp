#include "render/LandUse.h"

#include "render/FieldLayout.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <queue>
#include <tuple>

namespace lw {

namespace {

constexpr int kDx8[8] = {1, 0, -1, 0, 1, -1, -1, 1};
constexpr int kDy8[8] = {0, 1, 0, -1, 1, 1, -1, -1};
constexpr int kDx4[4] = {0, 0, -1, 1}; // север, юг, запад, восток
constexpr int kDy4[4] = {-1, 1, 0, 0};
constexpr std::uint8_t kNoFlow = 255;

// Опускание воды у лагуны и озера (как kWaterInset в Terrain3D) и у русла по классу реки.
constexpr float kOpenInset = 0.3f;
// Сколько клеток от озера или моря русло углубляется до своего.
constexpr int kInsetRamp = 6;
// Столбы — не ближе стольких клеток друг к другу (около 30 м), провод — не длиннее.
constexpr float kPoleSpacing = 7.5f;
constexpr float kWireReach = 11.0f;
// Мостков на поселение.
constexpr int kPiersPerSettlement = 2;

bool freshWater(Water water) {
    return water == Water::River || water == Water::Lake;
}

// Класс реки по глубине: ручей, река, большая река.
int riverClass(float depth) {
    return depth <= 0.5f ? 0 : depth <= 1.5f ? 1 : 2;
}

std::uint32_t cellHash(Cell cell, std::uint32_t salt) {
    return fieldHash(cell.x, cell.y, salt + 1000u);
}

} // namespace

LandUse::LandUse(const Map& map)
    : map_(map),
      flags_(map.width(), map.height(), 0),
      lot_(map.width(), map.height(), 0),
      flowDir_(map.width(), map.height(), kNoFlow),
      drop_(map.width(), map.height(), 0),
      inset_(map.width(), map.height(), static_cast<std::uint8_t>(std::lround(kOpenInset * 100.0f))) {
    computeFlow();
    computeChannels();
    markRoadsAndShores();
    bucketsX_ = (map.width() + kBucketCells - 1) / kBucketCells;
    const int bucketsY = (map.height() + kBucketCells - 1) / kBucketCells;
    buckets_.resize(static_cast<std::size_t>(bucketsX_) * static_cast<std::size_t>(bucketsY));
}

float LandUse::waterLevel(Cell cell) const {
    if (map_.water.at(cell.x, cell.y) == Water::Sea) {
        return 0.0f;
    }
    return map_.elevationMeters(cell) + map_.depthMeters(cell);
}

// Течение: «затопление по приоритету» от моря и края карты вверх по воде.
// Клетка достаётся от той, из которой до неё дошли первой, — это и есть, куда
// течёт вода. На ровном участке (уровень одинаков) первыми достаются клетки у
// нижнего выхода, поэтому плёс течёт к своему стоку, а не назад в озеро.
void LandUse::computeFlow() {
    const int width = map_.width();
    const int height = map_.height();
    struct Node {
        float key = 0.0f;
        std::uint32_t order = 0;
        int x = 0;
        int y = 0;
        bool operator>(const Node& other) const {
            return std::tie(key, order) > std::tie(other.key, other.order);
        }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<>> queue;
    Grid<std::uint8_t> visited(width, height, 0);
    // Направления — во временном плотном слое: в затоплении участвуют и озёра,
    // а хранятся (разреженно) только у рек.
    Grid<std::uint8_t> direction(width, height, kNoFlow);
    std::uint32_t order = 0;
    const auto inland = [&](int x, int y) {
        return map_.contains({x, y}) && freshWater(map_.water.at(x, y));
    };
    const auto flood = [&]() {
        while (!queue.empty()) {
            const Node node = queue.top();
            queue.pop();
            for (int d = 0; d < 8; ++d) {
                const int nx = node.x + kDx8[d];
                const int ny = node.y + kDy8[d];
                if (!inland(nx, ny) || visited.at(nx, ny) != 0) {
                    continue;
                }
                visited.at(nx, ny) = 1;
                // Из соседа вода течёт сюда: направление обратное d.
                direction.at(nx, ny) = static_cast<std::uint8_t>(d < 4 ? (d + 2) % 4 : 4 + (d - 4 + 2) % 4);
                queue.push({std::max(waterLevel({nx, ny}), node.key), order++, nx, ny});
            }
        }
    };

    // Истоки затопления: пресная вода у моря и у края карты.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!inland(x, y)) {
                continue;
            }
            int out = -1;
            for (int d = 0; d < 8 && out < 0; ++d) {
                const int nx = x + kDx8[d];
                const int ny = y + kDy8[d];
                if (!map_.contains({nx, ny}) || map_.water.at(nx, ny) == Water::Sea) {
                    out = d;
                }
            }
            if (out >= 0) {
                visited.at(x, y) = 1;
                direction.at(x, y) = static_cast<std::uint8_t>(out);
                queue.push({waterLevel({x, y}), order++, x, y});
            }
        }
    }
    flood();

    // Бессточные: от самой низкой клетки, сток — в ней самой.
    std::vector<std::tuple<float, int, int>> rest;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (inland(x, y) && visited.at(x, y) == 0) {
                rest.emplace_back(waterLevel({x, y}), y, x);
            }
        }
    }
    std::sort(rest.begin(), rest.end());
    for (const auto& [level, y, x] : rest) {
        if (visited.at(x, y) != 0) {
            continue;
        }
        visited.at(x, y) = 1;
        direction.at(x, y) = kNoFlow;
        queue.push({level, order++, x, y});
        flood();
    }

    // Падение уровня до следующей клетки — для перекатов; течёт только река.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t d = direction.at(x, y);
            if (map_.water.at(x, y) != Water::River || d == kNoFlow) {
                continue;
            }
            flowDir_.set(x, y, d);
            const Cell next = {x + kDx8[d], y + kDy8[d]};
            if (!map_.contains(next) || map_.water.at(next.x, next.y) == Water::None) {
                continue;
            }
            const float drop = waterLevel({x, y}) - waterLevel(next);
            drop_.set(x, y, static_cast<std::uint8_t>(std::clamp(drop * 100.0f, 0.0f, 255.0f)));
        }
    }
}

// Русло: вода реки опущена ниже расчётной по классу реки, а у озера и моря —
// так же, как у них, чтобы на стыке не было ступеньки.
void LandUse::computeChannels() {
    const int width = map_.width();
    const int height = map_.height();
    Grid<std::uint8_t> distance(width, height, 255);
    std::deque<Cell> queue;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (map_.water.at(x, y) != Water::River) {
                continue;
            }
            for (int d = 0; d < 8; ++d) {
                const Cell next = {x + kDx8[d], y + kDy8[d]};
                if (map_.contains(next) && (map_.water.at(next.x, next.y) == Water::Lake ||
                                            map_.water.at(next.x, next.y) == Water::Sea)) {
                    distance.at(x, y) = 1;
                    queue.push_back({x, y});
                    break;
                }
            }
        }
    }
    while (!queue.empty()) {
        const Cell cell = queue.front();
        queue.pop_front();
        const int next = distance.at(cell.x, cell.y) + 1;
        if (next > kInsetRamp) {
            continue;
        }
        for (int d = 0; d < 8; ++d) {
            const Cell around = {cell.x + kDx8[d], cell.y + kDy8[d]};
            if (map_.contains(around) && map_.water.at(around.x, around.y) == Water::River &&
                distance.at(around.x, around.y) > next) {
                distance.at(around.x, around.y) = static_cast<std::uint8_t>(next);
                queue.push_back(around);
            }
        }
    }
    constexpr float kClassInset[3] = {0.55f, 0.8f, 1.0f};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Water water = map_.water.at(x, y);
            float inset = kOpenInset;
            if (water == Water::River) {
                const float full = kClassInset[riverClass(map_.depthMeters({x, y}))];
                const float share =
                    std::min(static_cast<float>(distance.at(x, y)) - 1.0f, static_cast<float>(kInsetRamp)) /
                    static_cast<float>(kInsetRamp);
                inset = kOpenInset + (full - kOpenInset) * std::clamp(share, 0.0f, 1.0f);
            }
            inset_.set(x, y, static_cast<std::uint8_t>(std::lround(inset * 100.0f)));
        }
    }
}

void LandUse::markRoadsAndShores() {
    for (int y = 0; y < map_.height(); ++y) {
        for (int x = 0; x < map_.width(); ++x) {
            if (map_.water.at(x, y) != Water::None || map_.road.at(x, y) != Road::None) {
                continue;
            }
            std::uint8_t flags = 0;
            for (int d = 0; d < 4; ++d) {
                const Cell next = {x + kDx4[d], y + kDy4[d]};
                if (!map_.contains(next)) {
                    continue;
                }
                if (map_.road.at(next.x, next.y) != Road::None && map_.water.at(next.x, next.y) == Water::None) {
                    flags |= kRoadside;
                }
                if (freshWater(map_.water.at(next.x, next.y))) {
                    flags |= kShore;
                }
            }
            flags_.set(x, y, flags);
        }
    }
}

LandUse::Flow LandUse::flowAt(Cell cell) const {
    Flow flow;
    if (!map_.contains(cell) || map_.water.at(cell.x, cell.y) != Water::River) {
        return flow;
    }
    // Среднее по соседним клеткам реки: течение плавно поворачивает.
    float sx = 0.0f;
    float sz = 0.0f;
    float rapids = 0.0f;
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Cell around = {cell.x + dx, cell.y + dy};
            if (!map_.contains(around)) {
                continue;
            }
            const std::uint8_t d = flowDir_.get(around.x, around.y);
            if (d == kNoFlow) {
                continue;
            }
            const float length = d < 4 ? 1.0f : 1.41421356f;
            sx += static_cast<float>(kDx8[d]) / length;
            sz += static_cast<float>(kDy8[d]) / length;
            // Падение на клетку русла: от 12 см — рябит быстрее, от полуметра — пена.
            const float drop = static_cast<float>(drop_.get(around.x, around.y)) / 100.0f / length;
            rapids += std::clamp((drop - 0.12f) / 0.38f, 0.0f, 1.0f);
            ++count;
        }
    }
    const float length = std::sqrt(sx * sx + sz * sz);
    if (count == 0 || length < 1e-3f) {
        return flow;
    }
    flow.x = sx / length;
    flow.z = sz / length;
    flow.rapids = rapids / static_cast<float>(count);
    constexpr float kBaseSpeed[3] = {0.6f, 0.8f, 0.7f};
    flow.speed = kBaseSpeed[riverClass(map_.depthMeters(cell))] * std::min(1.0f, length / static_cast<float>(count) * 1.5f) +
                 1.6f * flow.rapids;
    return flow;
}

float LandUse::channelInset(Cell cell) const {
    const Cell c = map_.clampedCellAt(Map::cellCenter(cell.x), Map::cellCenter(cell.y));
    return static_cast<float>(inset_.get(c.x, c.y)) / 100.0f;
}

float LandUse::bankHeight(Cell cell) const {
    if (!map_.contains(cell)) {
        return 0.0f;
    }
    switch (map_.water.at(cell.x, cell.y)) {
    case Water::River: {
        constexpr float kBank[3] = {0.45f, 0.7f, 0.9f};
        return kBank[riverClass(map_.depthMeters(cell))];
    }
    case Water::Lake:
        return 0.3f;
    default:
        return 0.0f;
    }
}

const std::vector<int>& LandUse::thingsIn(int bx, int by) const {
    static const std::vector<int> kNone;
    if (bx < 0 || by < 0 || bx >= bucketsX_ || by >= static_cast<int>(buckets_.size()) / bucketsX_) {
        return kNone;
    }
    return buckets_[static_cast<std::size_t>(by * bucketsX_ + bx)];
}

void LandUse::addSettlements(const World& world) {
    if (settled_) {
        return;
    }
    settled_ = true;
    addLots(world);
    addYardThings();
    addPoles(world);
    addPiers(world);
    bucketThings();
}

void LandUse::addLots(const World& world) {
    const entt::registry& registry = world.registry();
    // Поселения: где деревня, а где город; клетки поселений.
    std::vector<std::pair<EntityId, const Settlement*>> settlements;
    for (const auto [entity, settlement, id] : registry.view<const Settlement, const Id>().each()) {
        settlements.emplace_back(id.value, &settlement);
    }
    std::sort(settlements.begin(), settlements.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto villageOf = [&](EntityId id) {
        for (const auto& [sid, settlement] : settlements) {
            if (sid == id) {
                return settlement->kind == SettlementKind::Village;
            }
        }
        return true;
    };
    for (const auto& [sid, settlement] : settlements) {
        const int r = settlement->radius;
        for (int y = settlement->center.y - r; y <= settlement->center.y + r; ++y) {
            for (int x = settlement->center.x - r; x <= settlement->center.x + r; ++x) {
                const int dx = x - settlement->center.x;
                const int dy = y - settlement->center.y;
                if (map_.contains({x, y}) && dx * dx + dy * dy <= r * r) {
                    flags_.at(x, y) |= kSettled;
                }
            }
        }
    }

    std::vector<std::pair<EntityId, const Building*>> buildings;
    for (const auto [entity, building, id] : registry.view<const Building, const Id>().each()) {
        buildings.emplace_back(id.value, &building);
    }
    std::sort(buildings.begin(), buildings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    lots_.reserve(buildings.size());
    for (const auto& [id, building] : buildings) {
        Lot lot;
        lot.kind = building->kind;
        lot.origin = building->origin;
        lot.width = building->width;
        lot.height = building->height;
        lot.door = building->door;
        lot.village = villageOf(building->settlement);
        lot.bits = cellHash(building->origin, 11u);
        const float u = hashUnit(lot.bits);
        if (lot.kind == BuildingKind::Barn) {
            lot.fence = FenceStyle::Rails;
        } else if (lot.village) {
            lot.fence = u < 0.35f ? FenceStyle::Wattle : u < 0.8f ? FenceStyle::Picket : FenceStyle::Boards;
        } else {
            lot.fence = u < 0.6f ? FenceStyle::Picket : FenceStyle::Boards;
        }
        lots_.push_back(lot);
        const auto index = static_cast<std::uint16_t>(lots_.size());
        for (int y = lot.origin.y; y < lot.origin.y + lot.height; ++y) {
            for (int x = lot.origin.x; x < lot.origin.x + lot.width; ++x) {
                if (map_.contains({x, y})) {
                    flags_.at(x, y) |= kBuilding;
                    lot_.at(x, y) = index;
                }
            }
        }
    }

    // Дворы — у домов и сараев: клетки вокруг здания достаются ближайшему
    // (обход в ширину по восьми соседям — расстояние Чебышёва), спорные — тому,
    // кто раньше в списке. Клетка перед входом — всегда своему.
    const auto yardRadius = [](const Lot& lot) {
        if (lot.kind == BuildingKind::Barn) {
            return lot.village ? 3 : 2;
        }
        if (lot.kind == BuildingKind::House) {
            return lot.village ? 2 : 1;
        }
        return 0;
    };
    const auto open = [&](int x, int y) {
        return map_.contains({x, y}) && lot_.get(x, y) == 0 && map_.water.at(x, y) == Water::None &&
               map_.road.at(x, y) == Road::None && map_.cover.at(x, y) == Cover::None;
    };
    struct Step {
        int x;
        int y;
        int lot;
        int distance;
    };
    std::deque<Step> queue;
    for (std::size_t i = 0; i < lots_.size(); ++i) {
        const Lot& lot = lots_[i];
        if (yardRadius(lot) == 0) {
            continue;
        }
        for (int y = lot.origin.y; y < lot.origin.y + lot.height; ++y) {
            for (int x = lot.origin.x; x < lot.origin.x + lot.width; ++x) {
                queue.push_back({x, y, static_cast<int>(i + 1), 0});
            }
        }
    }
    for (std::size_t i = 0; i < lots_.size(); ++i) {
        const Lot& lot = lots_[i];
        if (yardRadius(lot) > 0 && open(lot.door.x, lot.door.y)) {
            lot_.at(lot.door.x, lot.door.y) = static_cast<std::uint16_t>(i + 1);
            flags_.at(lot.door.x, lot.door.y) |= kYard | kDoor;
            queue.push_back({lot.door.x, lot.door.y, static_cast<int>(i + 1), 1});
        }
    }
    while (!queue.empty()) {
        const Step step = queue.front();
        queue.pop_front();
        if (step.distance + 1 > yardRadius(lots_[static_cast<std::size_t>(step.lot - 1)])) {
            continue;
        }
        for (int d = 0; d < 8; ++d) {
            const int nx = step.x + kDx8[d];
            const int ny = step.y + kDy8[d];
            if (!open(nx, ny)) {
                continue;
            }
            lot_.at(nx, ny) = static_cast<std::uint16_t>(step.lot);
            flags_.at(nx, ny) |= kYard;
            queue.push_back({nx, ny, step.lot, step.distance + 1});
        }
    }

    // Огород — за домом, в дальней от входа части двора (у большинства деревенских домов).
    for (std::size_t i = 0; i < lots_.size(); ++i) {
        const Lot& lot = lots_[i];
        if (lot.kind != BuildingKind::House || !lot.village || hashUnit(lot.bits >> 3) > 0.7f) {
            continue;
        }
        const int x1 = lot.origin.x + lot.width;
        const int y1 = lot.origin.y + lot.height;
        for (int y = lot.origin.y - 2; y < y1 + 2; ++y) {
            for (int x = lot.origin.x - 2; x < x1 + 2; ++x) {
                if (!map_.contains({x, y}) || lot_.get(x, y) != i + 1 || (flags_.get(x, y) & kYard) == 0) {
                    continue;
                }
                bool behind = false;
                if (lot.door.y < lot.origin.y) {
                    behind = y >= y1;
                } else if (lot.door.y >= y1) {
                    behind = y < lot.origin.y;
                } else if (lot.door.x < lot.origin.x) {
                    behind = x >= x1;
                } else {
                    behind = x < lot.origin.x;
                }
                if (behind) {
                    flags_.at(x, y) |= kGarden;
                }
            }
        }
    }
}

void LandUse::addYardThings() {
    constexpr float kCell = Map::kCellSize;
    // Заборы — по краям дворов: к улице (перед входом — калитка), к соседу
    // (один общий), к ничьей земле. К стенам и воде забора нет.
    for (std::size_t i = 0; i < lots_.size(); ++i) {
        const Lot& lot = lots_[i];
        const int own = static_cast<int>(i + 1);
        for (int y = lot.origin.y - 3; y < lot.origin.y + lot.height + 3; ++y) {
            for (int x = lot.origin.x - 3; x < lot.origin.x + lot.width + 3; ++x) {
                const Cell cell = {x, y};
                if (!map_.contains(cell) || lot_.get(x, y) != own || (flags_.get(x, y) & kYard) == 0) {
                    continue;
                }
                for (int d = 0; d < 4; ++d) {
                    const Cell next = {x + kDx4[d], y + kDy4[d]};
                    if (!map_.contains(next)) {
                        continue;
                    }
                    const int other = lot_.get(next.x, next.y);
                    if (other == own || (flags_.get(next.x, next.y) & kBuilding) != 0 ||
                        map_.water.at(next.x, next.y) != Water::None) {
                        continue;
                    }
                    if (other != 0 && other < own) {
                        continue; // общий забор ставит сосед с меньшим номером
                    }
                    Thing fence;
                    const bool street = map_.road.at(next.x, next.y) != Road::None;
                    fence.kind = street && (flags_.get(x, y) & kDoor) != 0 ? ThingKind::Gate : ThingKind::Fence;
                    fence.style = lot.fence;
                    fence.length = kCell;
                    const float x0 = static_cast<float>(x) * kCell;
                    const float z0 = static_cast<float>(y) * kCell;
                    switch (d) {
                    case 0: // север: край z0, вдоль x
                        fence.x = x0;
                        fence.z = z0;
                        break;
                    case 1:
                        fence.x = x0;
                        fence.z = z0 + kCell;
                        break;
                    case 2: // запад: край x0, вдоль z
                        fence.x = x0;
                        fence.z = z0;
                        fence.dx = 0.0f;
                        fence.dz = 1.0f;
                        break;
                    default:
                        fence.x = x0 + kCell;
                        fence.z = z0;
                        fence.dx = 0.0f;
                        fence.dz = 1.0f;
                        break;
                    }
                    fence.bits = cellHash(cell, 20u + static_cast<std::uint32_t>(d));
                    things_.push_back(fence);
                }
            }
        }
    }

    // Колодец, скамейка у входа, поленница у стены, стог у сарая.
    for (std::size_t i = 0; i < lots_.size(); ++i) {
        const Lot& lot = lots_[i];
        if (lot.kind != BuildingKind::House && lot.kind != BuildingKind::Barn) {
            continue;
        }
        const int own = static_cast<int>(i + 1);
        // Куда смотрит фасад: от дома к клетке входа.
        int fx = 0;
        int fz = 0;
        if (lot.door.y < lot.origin.y) {
            fz = -1;
        } else if (lot.door.y >= lot.origin.y + lot.height) {
            fz = 1;
        } else if (lot.door.x < lot.origin.x) {
            fx = -1;
        } else {
            fx = 1;
        }
        const float doorX = Map::cellCenter(lot.door.x);
        const float doorZ = Map::cellCenter(lot.door.y);
        const float centerX = (static_cast<float>(lot.origin.x) + 0.5f * static_cast<float>(lot.width)) * kCell;
        const float centerZ = (static_cast<float>(lot.origin.y) + 0.5f * static_cast<float>(lot.height)) * kCell;
        const std::uint32_t bits = lot.bits;
        const auto yardCell = [&](float x, float z) {
            const Cell cell = Map::cellAt(x, z);
            return map_.contains(cell) && lot_.get(cell.x, cell.y) == own && (flags_.get(cell.x, cell.y) & kYard) != 0;
        };

        if (lot.kind == BuildingKind::House && hashUnit(bits >> 5) < 0.6f && (flags_.get(lot.door.x, lot.door.y) & kYard)) {
            // Скамейка у стены сбоку от входа, сидящий смотрит на улицу. Крыльцо
            // (render/Models3D) — дверь 1 м и по 0,3 м с боков: скамейка длиной
            // 1,4 м начинается за ним.
            const float side = (bits & 1u) != 0u ? 1.8f : -1.8f;
            Thing bench;
            bench.kind = ThingKind::Bench;
            bench.x = doorX - static_cast<float>(fx) * (0.5f * kCell - 0.45f) + static_cast<float>(-fz) * side;
            bench.z = doorZ - static_cast<float>(fz) * (0.5f * kCell - 0.45f) + static_cast<float>(fx) * side;
            bench.dx = static_cast<float>(fx);
            bench.dz = static_cast<float>(fz);
            bench.length = 1.4f;
            bench.bits = bits;
            // Дальний конец скамейки — тоже во дворе (не за забором, не на улице).
            const float far = side + (side > 0.0f ? 0.7f : -0.7f);
            const float endX = doorX - static_cast<float>(fx) * (0.5f * kCell - 0.45f) + static_cast<float>(-fz) * far;
            const float endZ = doorZ - static_cast<float>(fz) * (0.5f * kCell - 0.45f) + static_cast<float>(fx) * far;
            if (Map::cellAt(endX, endZ) == lot.door || yardCell(endX, endZ)) {
                things_.push_back(bench);
            }
        }
        if (lot.kind == BuildingKind::House && lot.village && hashUnit(bits >> 7) < 0.55f) {
            // Поленница вдоль боковой стены.
            const float s = (bits & 2u) != 0u ? 1.0f : -1.0f;
            const float halfAcross = 0.5f * static_cast<float>(fz != 0 ? lot.width : lot.height) * kCell;
            const float x = centerX + static_cast<float>(-fz) * s * (halfAcross + 0.5f);
            const float z = centerZ + static_cast<float>(fx) * s * (halfAcross + 0.5f);
            if (yardCell(x, z)) {
                Thing pile;
                pile.kind = ThingKind::Woodpile;
                pile.x = x;
                pile.z = z;
                pile.dx = static_cast<float>(fx);
                pile.dz = static_cast<float>(fz);
                pile.length = 1.6f + 1.2f * hashUnit(bits >> 9);
                pile.bits = bits;
                things_.push_back(pile);
            }
        }
        const bool well = lot.kind == BuildingKind::House && lot.village && hashUnit(bits >> 11) < 0.35f;
        const bool stack = lot.kind == BuildingKind::Barn && hashUnit(bits >> 11) < 0.75f;
        if (well || stack) {
            // Клетка двора не у самой стены, не перед входом и не в огороде.
            std::uint32_t best = 0xFFFFFFFFu;
            Cell chosen = {-1, -1};
            for (int y = lot.origin.y - 3; y < lot.origin.y + lot.height + 3; ++y) {
                for (int x = lot.origin.x - 3; x < lot.origin.x + lot.width + 3; ++x) {
                    const Cell cell = {x, y};
                    if (!map_.contains(cell) || lot_.get(x, y) != own ||
                        (flags_.get(x, y) & (kYard | kDoor | kGarden)) != kYard) {
                        continue;
                    }
                    const int gapX = std::max({lot.origin.x - x, 0, x - (lot.origin.x + lot.width - 1)});
                    const int gapY = std::max({lot.origin.y - y, 0, y - (lot.origin.y + lot.height - 1)});
                    if (std::max(gapX, gapY) < 2) {
                        continue;
                    }
                    const std::uint32_t h = cellHash(cell, 31u);
                    if (h < best) {
                        best = h;
                        chosen = cell;
                    }
                }
            }
            if (chosen.x >= 0) {
                Thing thing;
                thing.kind = well ? ThingKind::Well : ThingKind::Haystack;
                thing.x = Map::cellCenter(chosen.x) + (hashUnit(best) - 0.5f) * 1.2f;
                thing.z = Map::cellCenter(chosen.y) + (hashUnit(best >> 4) - 0.5f) * 1.2f;
                thing.dx = static_cast<float>(fx);
                thing.dz = static_cast<float>(fz);
                thing.bits = best;
                things_.push_back(thing);
            }
        }
    }
}

// Столбы с проводами: вдоль асфальта и вдоль улиц в поселениях и у них, по
// одну сторону дороги (сторона по направлению дороги: у широтной — южная, у
// меридиональной — восточная), через 30 м; провод — к ближнему столбу вперёд и назад.
void LandUse::addPoles(const World& world) {
    std::vector<std::pair<Cell, int>> settlements; // центр и радиус для проводов
    for (const auto [entity, settlement] : world.registry().view<const Settlement>().each()) {
        settlements.emplace_back(settlement.center, settlement.radius * 14 / 10);
    }
    const auto nearSettlement = [&](Cell cell) {
        for (const auto& [center, radius] : settlements) {
            const int dx = cell.x - center.x;
            const int dy = cell.y - center.y;
            if (dx * dx + dy * dy <= radius * radius) {
                return true;
            }
        }
        return false;
    };
    // Направление дороги у клетки r: главная ось облака дорожных клеток вокруг.
    const auto roadAxis = [&](Cell r, float& ux, float& uz) {
        float sx = 0.0f;
        float sz = 0.0f;
        int n = 0;
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                if (map_.contains({r.x + dx, r.y + dy}) && map_.road.at(r.x + dx, r.y + dy) != Road::None) {
                    sx += static_cast<float>(dx);
                    sz += static_cast<float>(dy);
                    ++n;
                }
            }
        }
        const float mx = sx / static_cast<float>(std::max(n, 1));
        const float mz = sz / static_cast<float>(std::max(n, 1));
        float cxx = 0.0f;
        float czz = 0.0f;
        float cxz = 0.0f;
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                if (map_.contains({r.x + dx, r.y + dy}) && map_.road.at(r.x + dx, r.y + dy) != Road::None) {
                    const float ex = static_cast<float>(dx) - mx;
                    const float ez = static_cast<float>(dy) - mz;
                    cxx += ex * ex;
                    czz += ez * ez;
                    cxz += ex * ez;
                }
            }
        }
        const float angle = 0.5f * std::atan2(2.0f * cxz, cxx - czz);
        ux = std::cos(angle);
        uz = std::sin(angle);
    };

    struct Pole {
        Cell cell;
        float ux;
        float uz;
    };
    std::vector<Pole> poles;
    constexpr int kGrid = 8;
    const int gridX = map_.width() / kGrid + 1;
    std::vector<std::vector<int>> grid(static_cast<std::size_t>(gridX) * static_cast<std::size_t>(map_.height() / kGrid + 1));
    const auto bucket = [&](Cell cell) -> std::vector<int>& {
        return grid[static_cast<std::size_t>((cell.y / kGrid) * gridX + cell.x / kGrid)];
    };
    const auto crowded = [&](Cell cell) {
        for (int by = cell.y / kGrid - 1; by <= cell.y / kGrid + 1; ++by) {
            for (int bx = cell.x / kGrid - 1; bx <= cell.x / kGrid + 1; ++bx) {
                if (bx < 0 || by < 0 || bx >= gridX || by * kGrid >= map_.height() + kGrid) {
                    continue;
                }
                for (const int index : grid[static_cast<std::size_t>(by * gridX + bx)]) {
                    const float dx = static_cast<float>(poles[static_cast<std::size_t>(index)].cell.x - cell.x);
                    const float dy = static_cast<float>(poles[static_cast<std::size_t>(index)].cell.y - cell.y);
                    if (dx * dx + dy * dy < kPoleSpacing * kPoleSpacing) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    for (int y = 0; y < map_.height(); ++y) {
        for (int x = 0; x < map_.width(); ++x) {
            const std::uint8_t flags = flags_.get(x, y);
            if ((flags & kRoadside) == 0 || (flags & (kBuilding | kYard)) != 0) {
                continue;
            }
            const Cell cell = {x, y};
            for (int d = 0; d < 4; ++d) {
                const Cell road = {x + kDx4[d], y + kDy4[d]};
                if (!map_.contains(road) || map_.water.at(road.x, road.y) != Water::None) {
                    continue;
                }
                const Road type = map_.road.at(road.x, road.y);
                if (type == Road::None || (type == Road::Dirt && !nearSettlement(road))) {
                    continue;
                }
                float ux = 0.0f;
                float uz = 0.0f;
                roadAxis(road, ux, uz);
                float nx = -uz;
                float nz = ux;
                if (std::abs(nz) >= std::abs(nx) ? nz < 0.0f : nx < 0.0f) {
                    nx = -nx;
                    nz = -nz;
                }
                if (static_cast<float>(-kDx4[d]) * nx + static_cast<float>(-kDy4[d]) * nz <= 0.3f) {
                    continue; // не та сторона дороги
                }
                if (crowded(cell)) {
                    break;
                }
                bucket(cell).push_back(static_cast<int>(poles.size()));
                poles.push_back({cell, ux, uz});
                break;
            }
        }
    }

    const int first = static_cast<int>(things_.size());
    for (const Pole& pole : poles) {
        Thing thing;
        thing.kind = ThingKind::Pole;
        thing.x = Map::cellCenter(pole.cell.x);
        thing.z = Map::cellCenter(pole.cell.y);
        thing.dx = pole.ux;
        thing.dz = pole.uz;
        thing.bits = cellHash(pole.cell, 41u);
        things_.push_back(thing);
    }
    const auto link = [&](int a, int b) {
        Thing& from = things_[static_cast<std::size_t>(first + a)];
        Thing& to = things_[static_cast<std::size_t>(first + b)];
        for (const int existing : from.links) {
            if (existing == first + b) {
                return;
            }
        }
        if (from.links[1] >= 0 || to.links[1] >= 0) {
            return;
        }
        (from.links[0] < 0 ? from.links[0] : from.links[1]) = first + b;
        (to.links[0] < 0 ? to.links[0] : to.links[1]) = first + a;
    };
    for (std::size_t i = 0; i < poles.size(); ++i) {
        const Pole& pole = poles[i];
        int best[2] = {-1, -1};
        float bestDistance[2] = {kWireReach * kWireReach, kWireReach * kWireReach};
        const int reach = static_cast<int>(kWireReach) / kGrid + 1;
        for (int by = pole.cell.y / kGrid - reach; by <= pole.cell.y / kGrid + reach; ++by) {
            for (int bx = pole.cell.x / kGrid - reach; bx <= pole.cell.x / kGrid + reach; ++bx) {
                if (bx < 0 || by < 0 || bx >= gridX || static_cast<std::size_t>(by * gridX + bx) >= grid.size()) {
                    continue;
                }
                for (const int j : grid[static_cast<std::size_t>(by * gridX + bx)]) {
                    if (j == static_cast<int>(i)) {
                        continue;
                    }
                    const float dx = static_cast<float>(poles[static_cast<std::size_t>(j)].cell.x - pole.cell.x);
                    const float dy = static_cast<float>(poles[static_cast<std::size_t>(j)].cell.y - pole.cell.y);
                    const float distance = dx * dx + dy * dy;
                    const float along = (dx * pole.ux + dy * pole.uz) / std::sqrt(std::max(distance, 1e-3f));
                    const int side = along > 0.8f ? 0 : along < -0.8f ? 1 : -1;
                    if (side >= 0 && distance < bestDistance[side]) {
                        bestDistance[side] = distance;
                        best[side] = j;
                    }
                }
            }
        }
        for (const int j : best) {
            if (j >= 0) {
                link(static_cast<int>(i), j);
            }
        }
    }
}

// Мостки: у поселений, с берега в пресную воду шириной не меньше трёх клеток.
void LandUse::addPiers(const World& world) {
    std::vector<std::pair<EntityId, const Settlement*>> settlements;
    for (const auto [entity, settlement, id] : world.registry().view<const Settlement, const Id>().each()) {
        settlements.emplace_back(id.value, &settlement);
    }
    std::sort(settlements.begin(), settlements.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, settlement] : settlements) {
        const int r = settlement->radius * 13 / 10;
        struct Candidate {
            std::uint32_t hash;
            Cell cell;
            int d;
        };
        std::vector<Candidate> candidates;
        for (int y = settlement->center.y - r; y <= settlement->center.y + r; ++y) {
            for (int x = settlement->center.x - r; x <= settlement->center.x + r; ++x) {
                const Cell cell = {x, y};
                const int dx = x - settlement->center.x;
                const int dy = y - settlement->center.y;
                if (!map_.contains(cell) || dx * dx + dy * dy > r * r || (flags_.get(x, y) & kShore) == 0 ||
                    (flags_.get(x, y) & (kBuilding | kRoadside)) != 0) {
                    continue;
                }
                for (int d = 0; d < 4; ++d) {
                    bool wide = true;
                    for (int k = 1; k <= 3 && wide; ++k) {
                        const Cell c = {x + kDx4[d] * k, y + kDy4[d] * k};
                        wide = map_.contains(c) && freshWater(map_.water.at(c.x, c.y)) &&
                               map_.road.at(c.x, c.y) == Road::None;
                    }
                    if (wide) {
                        candidates.push_back({cellHash(cell, 51u + static_cast<std::uint32_t>(d)), cell, d});
                    }
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.hash < b.hash; });
        std::vector<Cell> placed;
        for (const Candidate& candidate : candidates) {
            if (static_cast<int>(placed.size()) >= kPiersPerSettlement) {
                break;
            }
            bool apart = true;
            for (const Cell other : placed) {
                const int dx = other.x - candidate.cell.x;
                const int dy = other.y - candidate.cell.y;
                apart = apart && dx * dx + dy * dy > 100;
            }
            if (!apart) {
                continue;
            }
            placed.push_back(candidate.cell);
            Thing pier;
            pier.kind = ThingKind::Pier;
            pier.dx = static_cast<float>(kDx4[candidate.d]);
            pier.dz = static_cast<float>(kDy4[candidate.d]);
            pier.x = Map::cellCenter(candidate.cell.x) + pier.dx * 0.6f;
            pier.z = Map::cellCenter(candidate.cell.y) + pier.dz * 0.6f;
            pier.length = 4.5f + 2.5f * hashUnit(candidate.hash);
            pier.bits = candidate.hash;
            things_.push_back(pier);
        }
    }
}

void LandUse::bucketThings() {
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    for (std::size_t i = 0; i < things_.size(); ++i) {
        const Cell cell = map_.clampedCellAt(things_[i].x, things_[i].z);
        buckets_[static_cast<std::size_t>((cell.y / kBucketCells) * bucketsX_ + cell.x / kBucketCells)].push_back(
            static_cast<int>(i));
    }
}

std::size_t LandUse::memoryBytes() const {
    std::size_t bytes = flags_.memoryBytes() + lot_.memoryBytes() + flowDir_.memoryBytes() + drop_.memoryBytes() +
                        inset_.memoryBytes();
    bytes += lots_.capacity() * sizeof(Lot) + things_.capacity() * sizeof(Thing);
    for (const std::vector<int>& bucket : buckets_) {
        bytes += bucket.capacity() * sizeof(int);
    }
    return bytes + buckets_.capacity() * sizeof(std::vector<int>);
}

} // namespace lw
