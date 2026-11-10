#include "sim/Population.h"

#include "sim/Components.h"
#include "sim/Family.h"
#include "sim/Names.h"
#include "sim/Relations.h"
#include "sim/Traits.h"
#include "sim/World.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Сколько жильцов приходится на дом и на многоэтажку (вес при расселении).
constexpr std::uint32_t kHouseWeight = 1;
constexpr std::uint32_t kApartmentWeight = 4;
// Попыток найти проходимую клетку для бездомного.
constexpr int kSpawnAttempts = 200;
// Сколько раз перевыбирать имя (и фамилию рода), пока не найдётся незанятое.
constexpr int kNameAttempts = 50;
// Семьи (этап 7.3): квартир в многоэтажке; сколько раз перевыбирать занятый дом.
constexpr int kFlatsPerApartment = 8;
constexpr int kHomeAttempts = 20;
// Возраст: младше kSlowChildAge и с kSlowElderAge ходят медленнее; младше
// kYoungChildAge — маленькие дети (рано спят); младше kArmedAge оружия нет.
constexpr int kSlowChildAge = 10;
constexpr int kSlowElderAge = 72;
constexpr int kYoungChildAge = 13;
constexpr int kArmedAge = 16;
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
// Приток зомби (этап 5.7): орда приходит вдали от людей и поселений, метры.
constexpr float kArrivalPeopleDistance = 800.0f;
constexpr float kArrivalSettlementDistance = 400.0f;

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
    case BuildingKind::Hospital:
        return {5, 15, 10, 30};
    case BuildingKind::PoliceStation:
        return {3, 8, 4, 10};
    case BuildingKind::School:
        return {2, 8, 4, 12};
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
    // Этап 7.2: больница — бинты и лекарства, полиция — оружейная, школа — аптечка.
    case BuildingKind::Hospital:
        return {{K::Bandage, 0.95f, 10, 30}, {K::Medicine, 0.95f, 6, 20}};
    case BuildingKind::PoliceStation:
        return {{K::Pistol, 0.8f, 1, 3},           {K::PistolAmmo, 0.9f, 20, 60},
                {K::Shotgun, 0.4f, 1, 2},          {K::ShotgunShells, 0.6f, 10, 30},
                {K::Rifle, 0.15f, 1, 1},           {K::RifleAmmo, 0.2f, 5, 15},
                {K::Bandage, 0.6f, 2, 6},          {K::Knife, 0.3f, 1, 1}};
    case BuildingKind::School:
        return {{K::Bandage, 0.6f, 1, 4}, {K::Medicine, 0.2f, 1, 2}, {K::Axe, 0.1f, 1, 1}};
    }
    return {};
}

// Распределение (этап 5.7): в городах снаряжение встречается чаще, в каждом
// городе есть аптека (первый по id магазин), в части деревень — дом охотника
// (первый по id дом) с винтовкой.
constexpr float kTownGearFactor = 1.25f;
constexpr int kPharmacyMedicineMin = 4;
constexpr int kPharmacyMedicineMax = 10;
constexpr int kPharmacyBandagesMin = 6;
constexpr int kPharmacyBandagesMax = 15;
constexpr float kHunterChance = 0.5f;
constexpr int kHunterAmmoMin = 8;
constexpr int kHunterAmmoMax = 16;

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
    const auto inTown = [&](const Building& building) {
        const entt::entity settlement = world.find(building.settlement);
        return settlement != entt::null &&
               registry.get<Settlement>(settlement).kind == SettlementKind::Town;
    };
    for (const auto& [id, entity] : buildings) {
        Inventory& inventory = registry.get<Inventory>(entity);
        const Building& building = registry.get<Building>(entity);
        const float factor = inTown(building) ? kTownGearFactor : 1.0f;
        for (const LootRoll& roll : lootTable(building.kind)) {
            if (rng.chance(std::min(1.0f, roll.chance * factor))) {
                inventory.add(roll.item, rng.rangeInt(roll.min, roll.max));
            }
        }
    }
    // Особые места: аптека в каждом городе, дом охотника в части деревень.
    // Поселения — по id, здания в них — по id.
    for (const Places::SettlementPlace& place : world.places().settlements()) {
        const entt::entity settlement = world.find(place.id);
        const bool town = registry.get<Settlement>(settlement).kind == SettlementKind::Town;
        const BuildingKind wanted = town ? BuildingKind::Shop : BuildingKind::House;
        const auto special = std::find_if(place.buildings.begin(), place.buildings.end(),
                                          [&](const Places::BuildingPlace& b) { return b.kind == wanted; });
        if (special == place.buildings.end()) {
            continue;
        }
        Inventory& inventory = registry.get<Inventory>(world.find(special->id));
        if (town) {
            inventory.add(ItemKind::Medicine, rng.rangeInt(kPharmacyMedicineMin, kPharmacyMedicineMax));
            inventory.add(ItemKind::Bandage, rng.rangeInt(kPharmacyBandagesMin, kPharmacyBandagesMax));
        } else if (rng.chance(kHunterChance)) {
            inventory.add(ItemKind::Rifle, 1);
            inventory.add(ItemKind::RifleAmmo, rng.rangeInt(kHunterAmmoMin, kHunterAmmoMax));
        }
    }
}

void spawnPeople(World& world, int count) {
    Rng& rng = world.rng(RngStream::Spawn);
    Rng& character = world.rng(RngStream::Traits);
    Rng& gear = world.rng(RngStream::Combat);
    Rng& naming = world.rng(RngStream::Names);
    Rng& families = world.rng(RngStream::Families);
    entt::registry& registry = world.registry();
    const NavGrid& nav = world.nav();

    struct Home {
        EntityId id;
        Cell door;
        std::uint32_t weightEnd; // накопленный вес: дом выбирается по нему
        int capacity;            // сколько домохозяйств: дом — одно, многоэтажка — квартиры
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
        const bool apartment = building->kind == BuildingKind::Apartment;
        totalWeight += apartment ? kApartmentWeight : kHouseWeight;
        homes.push_back({id, building->door, totalWeight, apartment ? kFlatsPerApartment : 1});
    }

    // Состав домов (этап 7.3): кто с кем живёт, возраст, пол, родство.
    const HouseholdPlan plan = planHouseholds(families, count);

    // Дом каждому домохозяйству — по весу; занятый (в многоэтажке — все
    // квартиры) перевыбирается.
    std::vector<int> homeOf(plan.households.size(), -1);
    std::vector<int> occupied(homes.size(), 0);
    for (std::size_t h = 0; h < plan.households.size() && !homes.empty(); ++h) {
        for (int attempt = 0; attempt < kHomeAttempts; ++attempt) {
            const std::uint32_t roll = rng.below(totalWeight);
            const auto home = std::upper_bound(
                homes.begin(), homes.end(), roll,
                [](std::uint32_t value, const Home& candidate) { return value < candidate.weightEnd; });
            const auto index = static_cast<std::size_t>(home - homes.begin());
            if (occupied[index] < home->capacity || attempt + 1 == kHomeAttempts) {
                homeOf[h] = static_cast<int>(index);
                ++occupied[index];
                break;
            }
        }
    }

    // Фамилия рода: у разных родов по возможности разные.
    std::vector<std::string> surnames(static_cast<std::size_t>(plan.lineages));
    std::unordered_set<std::string> takenSurnames;
    for (const PlannedPerson& planned : plan.people) {
        std::string& surname = surnames[static_cast<std::size_t>(planned.lineage)];
        if (!surname.empty()) {
            continue;
        }
        for (int attempt = 0; attempt < kNameAttempts && (surname.empty() || takenSurnames.contains(surname));
             ++attempt) {
            surname = randomLastName(naming);
        }
        takenSurnames.insert(surname);
    }
    // Полное имя у каждого своё, чтобы людей было легко различать.
    std::unordered_set<std::string> takenNames;
    for (const auto [entity, name] : registry.view<const Name>().each()) {
        takenNames.insert(name.first + " " + name.last);
    }

    std::vector<entt::entity> created(plan.people.size(), entt::entity{entt::null});
    for (std::size_t i = 0; i < plan.people.size(); ++i) {
        const PlannedPerson& planned = plan.people[i];
        Person person;
        std::optional<Cell> cell;
        const int home = homeOf[static_cast<std::size_t>(planned.household)];
        if (home >= 0) {
            person.home = homes[static_cast<std::size_t>(home)].id;
            person.indoors = true; // утро застаёт дома
            cell = homes[static_cast<std::size_t>(home)].door;
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
                break; // суши нет — селить некуда
            }
        }

        const int age = planned.age;
        Walker walker;
        // Дети и старики ходят медленнее.
        walker.speed = age < kSlowChildAge || age >= kSlowElderAge ? rng.range(0.9f, 1.25f)
                                                                     : rng.range(1.2f, 1.6f);
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

        Name name;
        name.last = surnames[static_cast<std::size_t>(planned.lineage)];
        for (int attempt = 0; attempt < kNameAttempts; ++attempt) {
            name.first = randomFirstName(naming, planned.female);
            if (!takenNames.contains(name.first + " " + name.last)) {
                break;
            }
        }
        takenNames.insert(name.first + " " + name.last);

        // Характер: у детей — свой склад (рано ложатся, реже верховодят и
        // лезут в драку), старики встают рано.
        Traits traits = generateTraits(character);
        if (age < kAdultAge) {
            traits.leadership *= 0.6f;
        }
        if (age < kYoungChildAge) {
            traits.chronotype *= 0.5f;
            traits.aggression *= 0.7f;
        } else if (age >= kSlowElderAge) {
            traits.chronotype *= 0.6f;
        }
        Weapon weapon = startWeapon(gear);
        if (age < kArmedAge) {
            weapon = {}; // оружие у детей не держат
        }

        const entt::entity entity = world.create();
        created[i] = entity;
        registry.emplace<Position>(entity, Map::cellCenter(cell->x), Map::cellCenter(cell->y));
        registry.emplace<Velocity>(entity);
        registry.emplace<Person>(entity, person);
        registry.emplace<Name>(entity, std::move(name));
        registry.emplace<Age>(entity, Age{static_cast<std::uint8_t>(age)});
        registry.emplace<Kin>(entity);
        registry.emplace<Walker>(entity, std::move(walker));
        registry.emplace<Needs>(entity, needs);
        registry.emplace<Traits>(entity, traits);
        registry.emplace<Mind>(entity, mind);
        Inventory inventory;
        if (gear.chance(kStartBandageChance)) {
            inventory.set(ItemKind::Bandage, 1);
        }
        registry.emplace<Inventory>(entity, inventory);
        registry.emplace<Senses>(entity);
        registry.emplace<Awareness>(entity);
        registry.emplace<Weapon>(entity, weapon);
        registry.emplace<Combat>(entity);
        registry.emplace<Wounds>(entity);
        registry.emplace<Relations>(entity);
        registry.emplace<Memories>(entity);
        registry.emplace<Leads>(entity);
        registry.emplace<Alarm>(entity); // о беде пока никто не знает (этап 7.6)
        world.record(ChronicleKind::Settled, registry.get<Id>(entity).value, person.home);
    }

    // Родство и опекуны — по id появившихся.
    const auto idOf = [&](int index) {
        const entt::entity entity = created[static_cast<std::size_t>(index)];
        return entity != entt::null ? registry.get<Id>(entity).value : kInvalidId;
    };
    for (const PlannedKin& link : plan.kin) {
        const entt::entity self = created[static_cast<std::size_t>(link.a)];
        if (self != entt::null && idOf(link.b) != kInvalidId) {
            registry.get<Kin>(self).add(idOf(link.b), link.kind);
        }
    }
    for (std::size_t i = 0; i < plan.people.size(); ++i) {
        if (created[i] != entt::null && plan.guardian[i] >= 0) {
            registry.get<Kin>(created[i]).guardian = idOf(plan.guardian[i]);
        }
    }

    // Знакомства с начала мира: родня, соседи по дому и поселению, прошлое.
    introduceFamilies(world);
    introduceNeighbours(world);
    weavePast(world);
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
        formHorde(world, std::move(members), HordeOrigin::Start);
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

int spawnZombieGroup(World& world, int size, Rng& rng) {
    if (size <= 0) {
        return 0;
    }
    const NavGrid& nav = world.nav();
    const NavGrid::Region mainland = mainlandOf(world);
    std::optional<Cell> center;
    for (int attempt = 0; attempt < kZombieSpawnAttempts && !center; ++attempt) {
        const Cell cell{static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                        static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
        if (nav.passable(cell) && (mainland == NavGrid::kNoRegion || nav.region(cell) == mainland) &&
            !nearSettlement(world, cell, kArrivalSettlementDistance) &&
            !nearPeople(world, cell, kArrivalPeopleDistance)) {
            center = cell;
        }
    }
    if (!center) {
        return 0;
    }
    entt::registry& registry = world.registry();
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
    if (members.size() >= 2) {
        formHorde(world, std::move(members), HordeOrigin::Arrived);
    }
    return size;
}

int animalTarget(int count, AnimalKind kind) {
    const int deer = count * kDeerSharePercent / 100;
    const int boar = count * kBoarSharePercent / 100;
    switch (kind) {
    case AnimalKind::Deer:
        return deer;
    case AnimalKind::Boar:
        return boar;
    default:
        return count - deer - boar;
    }
}

int animalGroupSize(AnimalKind kind, Rng& rng) {
    if (kind == AnimalKind::Deer) {
        return rng.rangeInt(kDeerHerdMin, kDeerHerdMax);
    }
    if (kind == AnimalKind::Boar) {
        return rng.rangeInt(1, kBoarGroupMax);
    }
    return 1;
}

int spawnAnimalGroup(World& world, AnimalKind kind, int size, float peopleDistance, Rng& rng) {
    const NavGrid& nav = world.nav();
    const Map& map = world.map();
    const NavGrid::Region mainland = mainlandOf(world);
    const AnimalSpec& spec = animalSpec(kind);
    std::optional<Cell> anchor;
    for (int attempt = 0; attempt < kAnimalSpawnAttempts && !anchor; ++attempt) {
        const Cell cell{static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                        static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
        if (!nav.passable(cell) || map.water.at(cell.x, cell.y) != Water::None ||
            (mainland != NavGrid::kNoRegion && nav.region(cell) != mainland)) {
            continue;
        }
        if ((map.cover.at(cell.x, cell.y) == Cover::Forest) == spec.forest &&
            !nearSettlement(world, cell, kAnimalSettlementDistance) &&
            !nearPeople(world, cell, peopleDistance)) {
            anchor = cell;
        }
    }
    if (!anchor) {
        return 0;
    }
    for (int i = 0; i < size; ++i) {
        Cell cell = *anchor;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const Cell candidate{anchor->x + rng.rangeInt(-3, 3), anchor->y + rng.rangeInt(-3, 3)};
            if (nav.passable(candidate) && map.water.at(candidate.x, candidate.y) == Water::None &&
                nav.connected(*anchor, candidate)) {
                cell = candidate;
                break;
            }
        }
        createAnimal(world, kind, cell, rng);
    }
    return size;
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

    for (std::size_t k = 0; k < static_cast<std::size_t>(AnimalKind::Count); ++k) {
        const auto kind = static_cast<AnimalKind>(k);
        int left = animalTarget(count, kind);
        while (left > 0) {
            const int size = std::min(animalGroupSize(kind, rng), left);
            left -= size;
            spawnAnimalGroup(world, kind, size, kAnimalPeopleDistance, rng);
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
