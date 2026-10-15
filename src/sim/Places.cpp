#include "sim/Places.h"

#include "sim/nav/NavGrid.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lw {

namespace {

bool freshWater(Water water) {
    return water == Water::Lake || water == Water::River;
}

// Можно напиться: проходимая клетка реки или суша рядом с пресной водой.
bool drinkable(const Map& map, const NavGrid& nav, int x, int y) {
    if (!nav.passable({x, y})) {
        return false;
    }
    if (freshWater(map.water.at(x, y))) {
        return true;
    }
    for (int d = 0; d < 4; ++d) {
        const Cell next{x + kNavDx[d], y + kNavDy[d]};
        if (map.contains(next) && freshWater(map.water.at(next.x, next.y))) {
            return true;
        }
    }
    return false;
}

bool forageable(const Map& map, const NavGrid& nav, int x, int y) {
    return nav.passable({x, y}) && map.cover.at(x, y) == Cover::Forest &&
           map.water.at(x, y) == Water::None;
}

float distanceSq(float ax, float ay, float bx, float by) {
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

} // namespace

void Places::build(const Map& map, const NavGrid& nav, const entt::registry& registry) {
    settlements_.clear();
    for (const auto [entity, id, settlement] : registry.view<const Id, const Settlement>().each()) {
        SettlementPlace place;
        place.id = id.value;
        place.center = settlement.center;
        place.x = Map::cellCenter(settlement.center.x);
        place.y = Map::cellCenter(settlement.center.y);
        settlements_.push_back(std::move(place));
    }
    std::sort(settlements_.begin(), settlements_.end(),
              [](const SettlementPlace& a, const SettlementPlace& b) { return a.id < b.id; });
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        const auto it = std::lower_bound(
            settlements_.begin(), settlements_.end(), building.settlement,
            [](const SettlementPlace& place, EntityId value) { return place.id < value; });
        if (it != settlements_.end() && it->id == building.settlement) {
            it->buildings.push_back({id.value, building.kind, building.door});
        }
    }
    for (SettlementPlace& place : settlements_) {
        std::sort(place.buildings.begin(), place.buildings.end(),
                  [](const BuildingPlace& a, const BuildingPlace& b) { return a.id < b.id; });
        for (const SettlementPlace& other : settlements_) {
            if (other.id != place.id && distanceSq(place.x, place.y, other.x, other.y) <=
                                            kNeighbourRange * kNeighbourRange) {
                place.neighbours.push_back(other.id);
            }
        }
    }

    // В каждом блоке — первая подходящая клетка по строкам.
    const int columns = (map.width() + kSpotBlock - 1) / kSpotBlock;
    const int rows = (map.height() + kSpotBlock - 1) / kSpotBlock;
    water_ = SpotGrid(columns, rows);
    forage_ = SpotGrid(columns, rows);
    waterCount_ = 0;
    forageCount_ = 0;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            std::optional<Cell>& water = water_.at(x / kSpotBlock, y / kSpotBlock);
            if (!water && drinkable(map, nav, x, y)) {
                water = Cell{x, y};
                ++waterCount_;
            }
            std::optional<Cell>& forage = forage_.at(x / kSpotBlock, y / kSpotBlock);
            if (!forage && forageable(map, nav, x, y)) {
                forage = Cell{x, y};
                ++forageCount_;
            }
        }
    }
}

const Places::SettlementPlace* Places::settlement(EntityId id) const {
    const auto it = std::lower_bound(
        settlements_.begin(), settlements_.end(), id,
        [](const SettlementPlace& place, EntityId value) { return place.id < value; });
    return it != settlements_.end() && it->id == id ? &*it : nullptr;
}

EntityId Places::nearestSettlement(float x, float y) const {
    EntityId best = kInvalidId;
    float bestDistance = 0.0f;
    for (const SettlementPlace& place : settlements_) {
        const float d = distanceSq(place.x, place.y, x, y);
        if (best == kInvalidId || d < bestDistance) {
            best = place.id;
            bestDistance = d;
        }
    }
    return best;
}

std::optional<Cell> Places::nearestWater(const NavGrid& nav, Cell from, float maxDistance) const {
    return nearestSpot(water_, nav, from, maxDistance, {});
}

std::optional<Cell> Places::nearestForage(const NavGrid& nav, Cell from, float maxDistance,
                                          const std::function<bool(Cell)>& accept) const {
    return nearestSpot(forage_, nav, from, maxDistance, accept);
}

std::optional<Cell> Places::nearestSpot(const SpotGrid& spots, const NavGrid& nav, Cell from,
                                        float maxDistance,
                                        const std::function<bool(Cell)>& accept) {
    if (spots.width() == 0) {
        return std::nullopt;
    }
    const int bx = std::clamp(from.x / kSpotBlock, 0, spots.width() - 1);
    const int by = std::clamp(from.y / kSpotBlock, 0, spots.height() - 1);
    const float blockMeters = static_cast<float>(kSpotBlock) * Map::kCellSize;
    const int maxRing = static_cast<int>(std::ceil(maxDistance / blockMeters)) + 1;
    const float fx = Map::cellCenter(from.x);
    const float fy = Map::cellCenter(from.y);
    const float maxSq = maxDistance * maxDistance;

    std::optional<Cell> best;
    float bestSq = 0.0f;
    const auto consider = [&](int x, int y) {
        if (!spots.contains(x, y)) {
            return;
        }
        const std::optional<Cell>& spot = spots.at(x, y);
        if (!spot) {
            return;
        }
        const float d = distanceSq(Map::cellCenter(spot->x), Map::cellCenter(spot->y), fx, fy);
        // При равных расстояниях остаётся первое по обходу: порядок колец фиксирован.
        if (d > maxSq || (best && d >= bestSq) || (accept && !accept(*spot)) ||
            !nav.connected(from, *spot)) {
            return;
        }
        best = spot;
        bestSq = d;
    };
    // Кольца блоков от центра. Место в кольце ring не ближе (ring − 1) блоков,
    // поэтому, когда кольца ушли дальше найденного, лучше уже не найти.
    for (int ring = 0; ring <= maxRing; ++ring) {
        if (best && ring > 1) {
            const float ringNear = static_cast<float>(ring - 1) * blockMeters;
            if (ringNear * ringNear > bestSq) {
                break;
            }
        }
        if (ring == 0) {
            consider(bx, by);
            continue;
        }
        for (int x = bx - ring; x <= bx + ring; ++x) {
            consider(x, by - ring);
            consider(x, by + ring);
        }
        for (int y = by - ring + 1; y <= by + ring - 1; ++y) {
            consider(bx - ring, y);
            consider(bx + ring, y);
        }
    }
    return best;
}

} // namespace lw
