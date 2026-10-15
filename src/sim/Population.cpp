#include "sim/Population.h"

#include "sim/Components.h"
#include "sim/Traits.h"
#include "sim/World.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <iterator>
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

// Зомби: доли орд и одиночек у зданий (остальные — на природе), проценты.
constexpr int kHordeSharePercent = 25;
constexpr int kBuildingSharePercent = 60;
constexpr int kSpawnHordeMin = 12;
constexpr int kSpawnHordeMax = 30;
// Ближе этого к людям зомби не появляются, метры: мир начинается не с погони,
// а одиночка бродит в kRoamRadius (ZombieSystem) от места, где появился.
constexpr float kLonerPeopleDistance = 150.0f;
constexpr float kWildPeopleDistance = 200.0f;
constexpr float kHordePeopleDistance = 250.0f;
// Орды — вдали от поселений, метры от центра.
constexpr float kHordeSettlementDistance = 300.0f;
constexpr float kHordeSpawnRadius = 8.0f; // зомби орды — вокруг её центра
constexpr int kZombieSpawnAttempts = 200;

// Животные: доли оленей и кабанов (остальные — зайцы), проценты; размеры групп.
constexpr int kDeerSharePercent = 50;
constexpr int kBoarSharePercent = 25;
constexpr int kDeerHerdMin = 2;
constexpr int kDeerHerdMax = 5;
constexpr int kBoarGroupMax = 3;
// Вдали от поселений (от центра) и от людей, метры.
constexpr float kAnimalSettlementDistance = 400.0f;
constexpr float kAnimalPeopleDistance = 300.0f;
constexpr float kFlockPeopleDistance = 150.0f;
constexpr int kAnimalSpawnAttempts = 400;
constexpr int kFlockMinBirds = 6;
constexpr int kFlockMaxBirds = 30;

// Оружие при рождении: доли в процентах по WeaponKind (кулаки, нож, топор,
// пистолет, дробовик, винтовка) и патроны к огнестрельному.
constexpr int kWeaponPercent[] = {35, 25, 20, 10, 5, 5};
struct AmmoRange {
    int min, max;
};
constexpr AmmoRange kStartAmmo[] = {{0, 0}, {0, 0}, {0, 0}, {4, 12}, {2, 8}, {3, 10}};

// Что у человека в руках при рождении.
Weapon startWeapon(Rng& rng) {
    static_assert(std::size(kWeaponPercent) == static_cast<std::size_t>(WeaponKind::Count));
    int roll = rng.rangeInt(0, 99);
    std::size_t kind = 0;
    while (kind + 1 < std::size(kWeaponPercent) && roll >= kWeaponPercent[kind]) {
        roll -= kWeaponPercent[kind];
        ++kind;
    }
    Weapon weapon;
    weapon.kind = static_cast<WeaponKind>(kind);
    if (kStartAmmo[kind].max > 0) {
        weapon.ammo = static_cast<std::uint16_t>(rng.rangeInt(kStartAmmo[kind].min, kStartAmmo[kind].max));
    }
    return weapon;
}

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

// Снаряжение в зданиях (этап 5.6): с какой вероятностью найдётся и сколько.
struct LootRoll {
    ItemKind item;
    float chance;
    int min, max;
};

std::vector<LootRoll> lootTable(BuildingKind kind) {
    using K = ItemKind;
    switch (kind) {
    case BuildingKind::House:
        return {{K::Bandage, 0.5f, 1, 2},      {K::Medicine, 0.12f, 1, 1},
                {K::Knife, 0.12f, 1, 1},       {K::Axe, 0.1f, 1, 1},
                {K::Pistol, 0.03f, 1, 1},      {K::Shotgun, 0.02f, 1, 1},
                {K::Rifle, 0.02f, 1, 1},       {K::PistolAmmo, 0.08f, 4, 10},
                {K::ShotgunShells, 0.06f, 3, 8}, {K::RifleAmmo, 0.05f, 3, 8}};
    case BuildingKind::Apartment:
        return {{K::Bandage, 0.7f, 1, 4},       {K::Medicine, 0.25f, 1, 2},
                {K::Knife, 0.2f, 1, 1},         {K::Axe, 0.05f, 1, 1},
                {K::Pistol, 0.05f, 1, 1},       {K::Shotgun, 0.02f, 1, 1},
                {K::Rifle, 0.02f, 1, 1},        {K::PistolAmmo, 0.12f, 4, 12},
                {K::ShotgunShells, 0.06f, 3, 8}, {K::RifleAmmo, 0.05f, 3, 8}};
    case BuildingKind::Shop:
        return {{K::Bandage, 0.8f, 2, 8},          {K::Medicine, 0.4f, 1, 4},
                {K::Knife, 0.3f, 1, 2},            {K::Axe, 0.2f, 1, 2},
                {K::PistolAmmo, 0.2f, 6, 20},      {K::ShotgunShells, 0.25f, 6, 20},
                {K::RifleAmmo, 0.15f, 5, 15}};
    case BuildingKind::Barn:
        return {{K::Axe, 0.35f, 1, 1},            {K::Knife, 0.15f, 1, 1},
                {K::Shotgun, 0.06f, 1, 1},        {K::Rifle, 0.05f, 1, 1},
                {K::ShotgunShells, 0.1f, 3, 10},  {K::RifleAmmo, 0.08f, 3, 10}};
    }
    return {};
}

// Бинт при себе у человека с начала мира — с такой вероятностью.
constexpr float kStartBandageChance = 0.4f;

// Есть ли кто-то из людей ближе radius к центру клетки (по индексу).
bool nearPeople(const World& world, Cell cell, float radius) {
    const entt::registry& registry = world.registry();
    return world.spatial()
        .nearest(Map::cellCenter(cell.x), Map::cellCenter(cell.y), radius,
                 [&registry](const SpatialHit& hit) { return registry.all_of<Person>(hit.entity); })
        .has_value();
}

bool nearSettlement(const World& world, Cell cell, float radius) {
    const float x = Map::cellCenter(cell.x);
    const float y = Map::cellCenter(cell.y);
    for (const Places::SettlementPlace& settlement : world.places().settlements()) {
        const float dx = settlement.x - x;
        const float dy = settlement.y - y;
        if (dx * dx + dy * dy < radius * radius) {
            return true;
        }
    }
    return false;
}

// Главная суша — область, где первое здание; без зданий — kNoRegion (любая).
NavGrid::Region mainlandOf(const World& world) {
    const entt::registry& registry = world.registry();
    EntityId first = kInvalidId;
    Cell door;
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        if (first == kInvalidId || id.value < first) {
            first = id.value;
            door = building.door;
        }
    }
    return first == kInvalidId ? NavGrid::kNoRegion : world.nav().region(door);
}

} // namespace

void spawnLoot(World& world) {
    Rng& rng = world.rng(RngStream::Supplies);
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> buildings;
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        buildings.emplace_back(id.value, entity);
    }
    std::sort(buildings.begin(), buildings.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // Еда и вода — первым проходом, тем же порядком рандома, что до этапа 5.6:
    // снаряжение не сдвигает припасы.
    for (const auto& [id, entity] : buildings) {
        const SupplyRange range = supplyRange(registry.get<Building>(entity).kind);
        Inventory inventory;
        inventory.set(ItemKind::Food, rng.rangeInt(range.foodMin, range.foodMax));
        inventory.set(ItemKind::Water, rng.rangeInt(range.waterMin, range.waterMax));
        registry.emplace<Inventory>(entity, inventory);
    }
    for (const auto& [id, entity] : buildings) {
        Inventory& inventory = registry.get<Inventory>(entity);
        for (const LootRoll& roll : lootTable(registry.get<Building>(entity).kind)) {
            if (rng.chance(roll.chance)) {
                inventory.add(roll.item, rng.rangeInt(roll.min, roll.max));
            }
        }
    }
}

void spawnPeople(World& world, int count) {
    Rng& rng = world.rng(RngStream::Spawn);
    Rng& character = world.rng(RngStream::Traits);
    Rng& gear = world.rng(RngStream::Combat);
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
            person.indoors = true; // утро застаёт дома
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
        Inventory inventory;
        if (gear.chance(kStartBandageChance)) {
            inventory.set(ItemKind::Bandage, 1);
        }
        registry.emplace<Inventory>(entity, inventory);
        registry.emplace<Senses>(entity);
        registry.emplace<Awareness>(entity);
        registry.emplace<Weapon>(entity, startWeapon(gear));
        registry.emplace<Combat>(entity);
        registry.emplace<Wounds>(entity);
        world.record(ChronicleKind::Settled, registry.get<Id>(entity).value, person.home);
    }
}

void spawnZombies(World& world, int count) {
    if (count <= 0) {
        return;
    }
    Rng& rng = world.rng(RngStream::Zombies);
    entt::registry& registry = world.registry();
    const NavGrid& nav = world.nav();

    std::vector<std::pair<EntityId, Cell>> doors;
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        doors.emplace_back(id.value, building.door);
    }
    std::sort(doors.begin(), doors.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // Главная суша — область, где первое здание; без зданий — любая.
    const NavGrid::Region mainland =
        doors.empty() ? NavGrid::kNoRegion : nav.region(doors.front().second);
    const auto randomLand = [&]() -> std::optional<Cell> {
        const Cell cell{static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                        static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
        if (!nav.passable(cell) ||
            (mainland != NavGrid::kNoRegion && nav.region(cell) != mainland)) {
            return std::nullopt;
        }
        return cell;
    };

    const int hordeZombies = count * kHordeSharePercent / 100;
    const int buildingZombies = count * kBuildingSharePercent / 100;
    int spawned = 0;

    // Орды.
    int left = hordeZombies;
    while (left >= kSpawnHordeMin) {
        const int size = std::min(left, rng.rangeInt(kSpawnHordeMin, kSpawnHordeMax));
        left -= size;
        std::optional<Cell> center;
        for (int attempt = 0; attempt < kZombieSpawnAttempts && !center; ++attempt) {
            const auto cell = randomLand();
            if (cell && !nearSettlement(world, *cell, kHordeSettlementDistance) &&
                !nearPeople(world, *cell, kHordePeopleDistance)) {
                center = cell;
            }
        }
        if (!center) {
            continue;
        }
        std::vector<EntityId> members;
        const int reach = static_cast<int>(kHordeSpawnRadius / Map::kCellSize);
        for (int i = 0; i < size; ++i) {
            Cell cell = *center;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const Cell candidate{center->x + rng.rangeInt(-reach, reach),
                                     center->y + rng.rangeInt(-reach, reach)};
                if (nav.passable(candidate) && nav.connected(*center, candidate)) {
                    cell = candidate;
                    break;
                }
            }
            members.push_back(registry.get<Id>(createZombie(world, cell, rng)).value);
        }
        formHorde(world, std::move(members), false);
        spawned += size;
    }

    // Одиночки у зданий: там, где обратились жильцы, — подальше от живых.
    for (int i = 0; i < buildingZombies && !doors.empty(); ++i) {
        for (int attempt = 0; attempt < kZombieSpawnAttempts / 10; ++attempt) {
            const Cell door = doors[rng.below(static_cast<std::uint32_t>(doors.size()))].second;
            if (!nearPeople(world, door, kLonerPeopleDistance)) {
                createZombie(world, door, rng);
                ++spawned;
                break;
            }
        }
    }

    // Остальные — на природе.
    for (int attempts = 0; spawned < count && attempts < count * kZombieSpawnAttempts; ++attempts) {
        const auto cell = randomLand();
        if (cell && !nearPeople(world, *cell, kWildPeopleDistance)) {
            createZombie(world, *cell, rng);
            ++spawned;
        }
    }
}

void spawnAnimals(World& world, int count, int flocks) {
    Rng& rng = world.rng(RngStream::Animals);
    const NavGrid& nav = world.nav();
    const Map& map = world.map();
    const NavGrid::Region mainland = mainlandOf(world);
    const auto randomLand = [&]() -> std::optional<Cell> {
        const Cell cell{static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                        static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
        if (!nav.passable(cell) || map.water.at(cell.x, cell.y) != Water::None ||
            (mainland != NavGrid::kNoRegion && nav.region(cell) != mainland)) {
            return std::nullopt;
        }
        return cell;
    };

    const int shares[] = {count * kDeerSharePercent / 100, count * kBoarSharePercent / 100,
                          count - count * kDeerSharePercent / 100 - count * kBoarSharePercent / 100};
    for (std::size_t k = 0; k < std::size(shares); ++k) {
        const auto kind = static_cast<AnimalKind>(k);
        const AnimalSpec& spec = animalSpec(kind);
        int left = shares[k];
        while (left > 0) {
            int size = 1;
            if (kind == AnimalKind::Deer) {
                size = rng.rangeInt(kDeerHerdMin, kDeerHerdMax);
            } else if (kind == AnimalKind::Boar) {
                size = rng.rangeInt(1, kBoarGroupMax);
            }
            size = std::min(size, left);
            left -= size;
            std::optional<Cell> anchor;
            for (int attempt = 0; attempt < kAnimalSpawnAttempts && !anchor; ++attempt) {
                const auto cell = randomLand();
                if (cell && (map.cover.at(cell->x, cell->y) == Cover::Forest) == spec.forest &&
                    !nearSettlement(world, *cell, kAnimalSettlementDistance) &&
                    !nearPeople(world, *cell, kAnimalPeopleDistance)) {
                    anchor = cell;
                }
            }
            if (!anchor) {
                continue;
            }
            for (int i = 0; i < size; ++i) {
                Cell cell = *anchor;
                for (int attempt = 0; attempt < 8; ++attempt) {
                    const Cell candidate{anchor->x + rng.rangeInt(-3, 3),
                                         anchor->y + rng.rangeInt(-3, 3)};
                    if (nav.passable(candidate) &&
                        map.water.at(candidate.x, candidate.y) == Water::None &&
                        nav.connected(*anchor, candidate)) {
                        cell = candidate;
                        break;
                    }
                }
                createAnimal(world, kind, cell, rng);
            }
        }
    }

    for (int i = 0; i < flocks; ++i) {
        for (int attempt = 0; attempt < kAnimalSpawnAttempts; ++attempt) {
            const auto cell = randomLand();
            if (cell && !nearPeople(world, *cell, kFlockPeopleDistance)) {
                createFlock(world, *cell, rng.rangeInt(kFlockMinBirds, kFlockMaxBirds), rng);
                break;
            }
        }
    }
}

} // namespace lw
