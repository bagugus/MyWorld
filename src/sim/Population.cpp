#include "sim/Population.h"

#include "sim/Components.h"
#include "sim/Traits.h"
#include "sim/World.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Сколько жильцов приходится на дом и на многоэтажку (вес при расселении).
constexpr std::uint32_t kHouseWeight = 1;
constexpr std::uint32_t kApartmentWeight = 4;
// Попыток найти проходимую клетку для бездомного.
constexpr int kSpawnAttempts = 200;
// Первое решение — в течение стольких игровых минут: люди думают не в один тик.
constexpr int kFirstThinkMinutes = 10;

struct SupplyRange {
    int foodMin, foodMax, waterMin, waterMax;
};

SupplyRange supplyRange(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return {3, 12, 2, 8};
    case BuildingKind::Apartment:
        return {8, 30, 6, 20};
    case BuildingKind::Shop:
        return {20, 80, 15, 50};
    case BuildingKind::Barn:
        return {0, 15, 0, 0};
    }
    return {0, 0, 0, 0};
}

} // namespace

void spawnSupplies(World& world) {
    Rng& rng = world.rng(RngStream::Supplies);
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> buildings;
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        buildings.emplace_back(id.value, entity);
    }
    std::sort(buildings.begin(), buildings.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, entity] : buildings) {
        const SupplyRange range = supplyRange(registry.get<Building>(entity).kind);
        Supplies supplies;
        supplies.food = static_cast<std::uint16_t>(rng.rangeInt(range.foodMin, range.foodMax));
        supplies.water = static_cast<std::uint16_t>(rng.rangeInt(range.waterMin, range.waterMax));
        registry.emplace<Supplies>(entity, supplies);
    }
}

void spawnPeople(World& world, int count) {
    Rng& rng = world.rng(RngStream::Spawn);
    Rng& character = world.rng(RngStream::Traits);
    entt::registry& registry = world.registry();
    const NavGrid& nav = world.nav();

    struct Home {
        EntityId id;
        Cell door;
        std::uint32_t weightEnd; // накопленный вес: дом выбирается по нему
    };
    std::vector<std::pair<EntityId, const Building*>> buildings;
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        if (building.kind == BuildingKind::House || building.kind == BuildingKind::Apartment) {
            buildings.emplace_back(id.value, &building);
        }
    }
    std::sort(buildings.begin(), buildings.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Home> homes;
    std::uint32_t totalWeight = 0;
    for (const auto& [id, building] : buildings) {
        totalWeight += building->kind == BuildingKind::Apartment ? kApartmentWeight : kHouseWeight;
        homes.push_back({id, building->door, totalWeight});
    }

    for (int i = 0; i < count; ++i) {
        Person person;
        std::optional<Cell> cell;
        if (!homes.empty()) {
            const std::uint32_t roll = rng.below(totalWeight);
            const auto home = std::upper_bound(
                homes.begin(), homes.end(), roll,
                [](std::uint32_t value, const Home& h) { return value < h.weightEnd; });
            person.home = home->id;
            cell = home->door;
        } else {
            for (int attempt = 0; attempt < kSpawnAttempts && !cell; ++attempt) {
                const Cell candidate{
                    static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                    static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
                if (nav.passable(candidate)) {
                    cell = candidate;
                }
            }
            if (!cell) {
                return; // суши нет — селить некуда
            }
        }

        Walker walker;
        walker.speed = rng.range(1.2f, 1.6f);
        // Утро: кто-то давно не ел, кто-то не выспался.
        Needs needs;
        needs.hunger = rng.range(0.0f, 0.4f);
        needs.thirst = rng.range(0.0f, 0.4f);
        needs.fatigue = rng.range(0.0f, 0.3f);
        needs.loneliness = rng.range(0.0f, 0.3f);
        Mind mind;
        mind.spot = *cell;
        mind.nextThink = world.tick() + rng.below(static_cast<std::uint32_t>(
                                            kFirstThinkMinutes * kTicksPerGameMinute));
        mind.until = mind.nextThink;

        const entt::entity entity = world.create();
        registry.emplace<Position>(entity, Map::cellCenter(cell->x), Map::cellCenter(cell->y));
        registry.emplace<Velocity>(entity);
        registry.emplace<Person>(entity, person);
        registry.emplace<Walker>(entity, std::move(walker));
        registry.emplace<Needs>(entity, needs);
        registry.emplace<Traits>(entity, generateTraits(character));
        registry.emplace<Mind>(entity, mind);
        registry.emplace<Carry>(entity);
        world.record(ChronicleKind::Settled, registry.get<Id>(entity).value, person.home);
    }
}

} // namespace lw
