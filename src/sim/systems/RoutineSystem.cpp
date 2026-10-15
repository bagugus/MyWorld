#include "sim/systems/RoutineSystem.h"

#include "sim/Calendar.h"
#include "sim/Components.h"
#include "sim/World.h"
#include "sim/systems/MovementSystem.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Сколько жильцов приходится на дом и на многоэтажку (вес при расселении).
constexpr std::uint32_t kHouseWeight = 1;
constexpr std::uint32_t kApartmentWeight = 4;
// Отдых между прогулками и после неудачной попытки проложить путь.
constexpr int kRestMinMinutes = 20;
constexpr int kRestMaxMinutes = 120;
constexpr int kRetryMinutes = 30;
// Доля прогулок в соседнее поселение и домой днём. До соседей — несколько
// игровых часов пути, поэтому к ним идут только утром, чтобы успеть вернуться.
constexpr float kNeighbourChance = 0.25f;
constexpr int kNeighbourLatestMinute = 11 * 60;
constexpr float kHomeChance = 0.3f;
// Соседние поселения — с центром не дальше этого, метры.
constexpr float kNeighbourRange = 2500.0f;
// Попыток найти проходимую клетку для бездомного.
constexpr int kSpawnAttempts = 200;

std::uint64_t gameMinutes(int minutes) {
    return static_cast<std::uint64_t>(minutes) * kTicksPerGameMinute;
}

bool isNight(const Routine& routine, int minuteOfDay) {
    return routine.bedtime > routine.wakeTime
               ? minuteOfDay >= routine.bedtime || minuteOfDay < routine.wakeTime
               : minuteOfDay >= routine.bedtime && minuteOfDay < routine.wakeTime;
}

Cell positionCell(const Position& position) {
    return Map::cellAt(position.x, position.y);
}

Position cellCenter(Cell cell) {
    return {Map::cellCenter(cell.x), Map::cellCenter(cell.y)};
}

// Двери зданий по id: порядок не зависит от хранения в реестре.
std::vector<std::pair<EntityId, Cell>> doorsOf(const entt::registry& registry,
                                               EntityId settlement) {
    std::vector<std::pair<EntityId, Cell>> doors;
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        if (building.settlement == settlement) {
            doors.emplace_back(id.value, building.door);
        }
    }
    std::sort(doors.begin(), doors.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return doors;
}

struct SettlementPlace {
    EntityId id;
    float x;
    float y;
};

std::vector<SettlementPlace> settlementsOf(const entt::registry& registry) {
    std::vector<SettlementPlace> places;
    for (const auto [entity, id, settlement] : registry.view<const Id, const Settlement>().each()) {
        places.push_back({id.value, Map::cellCenter(settlement.center.x),
                          Map::cellCenter(settlement.center.y)});
    }
    std::sort(places.begin(), places.end(),
              [](const SettlementPlace& a, const SettlementPlace& b) { return a.id < b.id; });
    return places;
}

float distanceSq(float ax, float ay, float bx, float by) {
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

// Поселение человека: где его дом, у бездомного — ближайшее.
EntityId homeSettlement(const World& world, const Person& person, const Position& position,
                        const std::vector<SettlementPlace>& places) {
    if (const entt::entity home = world.find(person.home); home != entt::null) {
        return world.registry().get<Building>(home).settlement;
    }
    EntityId best = kInvalidId;
    float bestDistance = 0.0f;
    for (const SettlementPlace& place : places) {
        const float d = distanceSq(place.x, place.y, position.x, position.y);
        if (best == kInvalidId || d < bestDistance) {
            best = place.id;
            bestDistance = d;
        }
    }
    return best;
}

// Куда пойти днём: дверь случайного здания своего или соседнего поселения.
std::optional<Cell> pickDestination(World& world, Rng& rng, EntityId own, bool neighbours) {
    if (own == kInvalidId) {
        return std::nullopt;
    }
    const entt::registry& registry = world.registry();
    EntityId target = own;
    if (neighbours && rng.chance(kNeighbourChance)) {
        const std::vector<SettlementPlace> places = settlementsOf(registry);
        const auto self = std::find_if(places.begin(), places.end(),
                                       [own](const SettlementPlace& p) { return p.id == own; });
        if (self != places.end()) {
            std::vector<EntityId> near;
            for (const SettlementPlace& place : places) {
                if (place.id != own && distanceSq(place.x, place.y, self->x, self->y) <=
                                           kNeighbourRange * kNeighbourRange) {
                    near.push_back(place.id);
                }
            }
            if (!near.empty()) {
                target = near[rng.below(static_cast<std::uint32_t>(near.size()))];
            }
        }
    }
    const auto doors = doorsOf(registry, target);
    if (doors.empty()) {
        return std::nullopt;
    }
    return doors[rng.below(static_cast<std::uint32_t>(doors.size()))].second;
}

} // namespace

void spawnPeople(World& world, int count) {
    Rng& rng = world.rng(RngStream::Spawn);
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
        Routine routine;
        routine.restUntil = world.tick() + rng.below(static_cast<std::uint32_t>(gameMinutes(120)));
        routine.bedtime = rng.rangeInt(21 * 60, 23 * 60 + 30);
        routine.wakeTime = rng.rangeInt(5 * 60 + 30, 8 * 60);

        // Утро: кто-то давно не ел, кто-то не выспался.
        Needs needs;
        needs.hunger = rng.range(0.0f, 0.4f);
        needs.thirst = rng.range(0.0f, 0.4f);
        needs.fatigue = rng.range(0.0f, 0.3f);
        needs.loneliness = rng.range(0.0f, 0.3f);

        const entt::entity entity = world.create();
        registry.emplace<Position>(entity, cellCenter(*cell));
        registry.emplace<Velocity>(entity);
        registry.emplace<Person>(entity, person);
        registry.emplace<Walker>(entity, std::move(walker));
        registry.emplace<Needs>(entity, needs);
        registry.emplace<Routine>(entity, routine);
    }
}

void updateRoutine(World& world) {
    Rng& rng = world.rng(RngStream::Behavior);
    const std::uint64_t tick = world.tick();
    const CalendarTime now = calendarAt(tick);
    entt::registry& registry = world.registry();
    std::vector<SettlementPlace> places; // только для бездомных, собирается по надобности

    auto view = registry.view<Person, Routine, const Walker, const Position>();
    for (const auto [entity, person, routine, walker, position] : view.each()) {
        if (routine.activity == Activity::Walk) {
            if (walker.moving()) {
                continue;
            }
            routine.activity = Activity::Rest;
            routine.restUntil =
                tick + gameMinutes(rng.rangeInt(kRestMinMinutes, kRestMaxMinutes));
            continue;
        }
        if (tick < routine.restUntil) {
            continue;
        }
        person.asleep = false;

        const entt::entity home = world.find(person.home);
        const std::optional<Cell> door =
            home != entt::null ? std::optional<Cell>(registry.get<Building>(home).door)
                               : std::nullopt;
        const bool atHome = door && positionCell(position) == *door;

        std::optional<Cell> goal;
        if (isNight(routine, now.minuteOfDay)) {
            if (!door || atHome) {
                // Спать до утра.
                person.asleep = true;
                routine.restUntil =
                    nextTimeOfDay(tick, routine.wakeTime / 60, routine.wakeTime % 60);
                continue;
            }
            goal = door;
        } else if (door && !atHome && rng.chance(kHomeChance)) {
            goal = door;
        } else {
            if (!door && places.empty()) {
                places = settlementsOf(registry);
            }
            goal = pickDestination(world, rng, homeSettlement(world, person, position, places),
                                   now.minuteOfDay < kNeighbourLatestMinute);
        }

        if (goal && startWalk(world, entity, *goal)) {
            routine.activity = Activity::Walk;
        } else {
            routine.restUntil = tick + gameMinutes(kRetryMinutes);
        }
    }
}

} // namespace lw
