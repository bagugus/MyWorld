#pragma once

#include "sim/Components.h"
#include "sim/Grid.h"
#include "sim/Id.h"
#include "sim/Map.h"

#include <entt/entt.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace lw {

class NavGrid;

// Справочник мест для решений людей: поселения и их здания, где напиться, где
// собирать ягоды и грибы.
//
// Производные данные карты и зданий: в хеш мира не входят, строятся один раз
// (пока здания не меняются). Все списки упорядочены по id, поэтому ответы не
// зависят от порядка хранения в реестре.
class Places {
public:
    // Сторона блока, в котором запоминается одно место для питья и одно для сбора, клетки.
    static constexpr int kSpotBlock = 8;
    // Соседние поселения — с центрами не дальше этого, метры.
    static constexpr float kNeighbourRange = 2500.0f;

    struct BuildingPlace {
        EntityId id = kInvalidId;
        BuildingKind kind = BuildingKind::House;
        Cell door;
    };

    struct SettlementPlace {
        EntityId id = kInvalidId;
        Cell center;
        float x = 0.0f; // центр, метры
        float y = 0.0f;
        std::vector<BuildingPlace> buildings; // по id
        std::vector<EntityId> neighbours;     // по id
    };

    void build(const Map& map, const NavGrid& nav, const entt::registry& registry);

    const std::vector<SettlementPlace>& settlements() const { return settlements_; }
    // nullptr, если поселения с таким id нет.
    const SettlementPlace* settlement(EntityId id) const;
    // Ближайшее к точке поселение (по центру); kInvalidId, если поселений нет.
    EntityId nearestSettlement(float x, float y) const;

    // Ближайшее место, где можно напиться (брод через реку или берег реки и
    // озера), достижимое из from и не дальше maxDistance метров по прямой.
    std::optional<Cell> nearestWater(const NavGrid& nav, Cell from, float maxDistance) const;
    // То же для клетки леса, где можно собирать ягоды и грибы. accept отсеивает
    // места (например, где всё уже собрано); пустой — подходит любое.
    std::optional<Cell> nearestForage(const NavGrid& nav, Cell from, float maxDistance,
                                      const std::function<bool(Cell)>& accept = {}) const;

    // Не больше одного места на блок kSpotBlock × kSpotBlock клеток.
    using SpotGrid = Grid<std::optional<Cell>>;
    // Места для сбора по блокам: по ним Resources заводит запас леса.
    const SpotGrid& forageSpots() const { return forage_; }

    std::size_t waterSpotCount() const { return waterCount_; }
    std::size_t forageSpotCount() const { return forageCount_; }

private:
    static std::optional<Cell> nearestSpot(const SpotGrid& spots, const NavGrid& nav, Cell from,
                                           float maxDistance,
                                           const std::function<bool(Cell)>& accept);

    std::vector<SettlementPlace> settlements_;
    SpotGrid water_;
    SpotGrid forage_;
    std::size_t waterCount_ = 0;
    std::size_t forageCount_ = 0;
};

} // namespace lw
