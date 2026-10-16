// Прогон симуляции без окна: проверка детерминизма и замер скорости.
//   lw_headless --seed 42 --ticks 10000
//   lw_headless --seed 42 --climate   сводка погоды и температуры за игровой год по сезонам
//   lw_headless --seed 42 --paths 500 замер поиска пути: пары дверей и пары случайных клеток суши
//                                     (--optimal — сравнить каждый путь с оптимальным)
//   lw_headless --seed 42 --people 3000 --ticks 144000   сутки мира с 3000 людей
//   lw_headless --seed 42 --zombies 0                    мир без зомби
//   lw_headless --seed 42 --animals 0 --flocks 0         мир без животных и птиц
//   lw_headless --seed 42 --ticks 8640000 --daily         год мира, строка на каждые сутки

#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Perception.h"
#include "sim/Rng.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"
#include "sim/nav/NavGraph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace {

// Сводка по карте: по ней видно, что генерация дала осмысленный мир.
void printMapSummary(const lw::World& world, double seconds) {
    const lw::Map& map = world.map();
    std::size_t land = 0;
    std::size_t forest = 0;
    std::size_t field = 0;
    std::size_t lake = 0;
    std::size_t river = 0;
    std::size_t road = 0;
    int highest = 0;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const lw::Water water = map.water.at(x, y);
            land += water == lw::Water::None ? 1 : 0;
            lake += water == lw::Water::Lake ? 1 : 0;
            river += water == lw::Water::River ? 1 : 0;
            forest += map.cover.at(x, y) == lw::Cover::Forest ? 1 : 0;
            field += map.ground.at(x, y) == lw::Ground::Field ? 1 : 0;
            road += map.road.at(x, y) != lw::Road::None ? 1 : 0;
            highest = map.elevation.at(x, y) > highest ? map.elevation.at(x, y) : highest;
        }
    }
    const double cells = static_cast<double>(map.width()) * map.height();
    const double landCells = land > 0 ? static_cast<double>(land) : 1.0;
    const double cellKm = lw::Map::kCellSize / 1000.0;

    std::size_t towns = 0;
    std::size_t villages = 0;
    const lw::Settlement* largest = nullptr;
    for (const auto [entity, settlement] : world.registry().view<const lw::Settlement>().each()) {
        (settlement.kind == lw::SettlementKind::Town ? towns : villages) += 1;
        if (largest == nullptr || settlement.radius > largest->radius) {
            largest = &settlement;
        }
    }

    std::printf("map generated in %.2f s\n", seconds);
    std::printf("  land %.0f%% (forest %.0f%%, fields %.0f%% of land), highest point %.0f m\n",
                100.0 * static_cast<double>(land) / cells,
                100.0 * static_cast<double>(forest) / landCells,
                100.0 * static_cast<double>(field) / landCells,
                highest * lw::Map::kElevationStep);
    std::printf("  lake cells %zu, river cells %zu, roads %.1f km\n", lake, river,
                static_cast<double>(road) * cellKm);
    std::printf("  towns %zu, villages %zu, buildings %zu\n", towns, villages,
                world.registry().view<const lw::Building>().size());
    if (largest != nullptr) {
        std::printf("  largest: %s at %.0f, %.0f m\n", largest->name.c_str(),
                    lw::Map::cellCenter(largest->center.x), lw::Map::cellCenter(largest->center.y));
    }
}

// Люди: сколько в пути, сколько у своего дома.
void printPeople(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::size_t people = 0;
    std::size_t walking = 0;
    std::size_t atHome = 0;
    std::size_t asleep = 0;
    lw::Needs mean{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float worstHealth = 1.0f;
    for (const auto [entity, person, walker, position] :
         registry.view<const lw::Person, const lw::Walker, const lw::Position>().each()) {
        ++people;
        asleep += person.asleep ? 1 : 0;
        if (const auto* needs = registry.try_get<lw::Needs>(entity)) {
            mean.hunger += needs->hunger;
            mean.thirst += needs->thirst;
            mean.fatigue += needs->fatigue;
            mean.fear += needs->fear;
            mean.loneliness += needs->loneliness;
            mean.health += needs->health;
            worstHealth = std::min(worstHealth, needs->health);
        }
        walking += walker.moving() ? 1 : 0;
        if (const entt::entity home = world.find(person.home); home != entt::null) {
            atHome += lw::Map::cellAt(position.x, position.y) ==
                              registry.get<lw::Building>(home).door
                          ? 1
                          : 0;
        }
    }
    std::printf("people %zu: walking %zu, at home %zu, asleep %zu\n", people, walking, atHome,
                asleep);
    constexpr const char* kActions[] = {"idle",   "sleep",     "eat",    "drink",   "scavenge",
                                        "forage", "socialize", "wander", "go home", "flee",
                                        "fight",  "hunt",      "treat"};
    static_assert(std::size(kActions) == static_cast<std::size_t>(lw::Action::Count));
    std::size_t actions[std::size(kActions)] = {};
    for (const auto [entity, mind] : registry.view<const lw::Mind>().each()) {
        ++actions[static_cast<std::size_t>(mind.action)];
    }
    std::printf("  doing:");
    for (std::size_t i = 0; i < std::size(kActions); ++i) {
        std::printf(" %s %zu%s", kActions[i], actions[i], i + 1 < std::size(kActions) ? "," : "\n");
    }
    // Лут в зданиях и что несут живые, по видам предметов.
    long long inBuildings[lw::kItemKinds] = {};
    long long carried[lw::kItemKinds] = {};
    for (const auto [entity, building, inventory] :
         registry.view<const lw::Building, const lw::Inventory>().each()) {
        for (std::size_t i = 0; i < lw::kItemKinds; ++i) {
            inBuildings[i] += inventory.items[i];
        }
    }
    for (const auto [entity, person, inventory] :
         registry.view<const lw::Person, const lw::Inventory>().each()) {
        for (std::size_t i = 0; i < lw::kItemKinds; ++i) {
            carried[i] += inventory.items[i];
        }
    }
    const auto at = [](const long long* counts, lw::ItemKind kind) {
        return counts[static_cast<std::size_t>(kind)];
    };
    using K = lw::ItemKind;
    std::printf("  in buildings: food %lld, water %lld, bandages %lld, medicine %lld, "
                "ammo %lld/%lld/%lld, guns %lld; carried: food %lld, meat %lld, water %lld, "
                "bandages %lld, medicine %lld\n",
                at(inBuildings, K::Food), at(inBuildings, K::Water), at(inBuildings, K::Bandage),
                at(inBuildings, K::Medicine), at(inBuildings, K::PistolAmmo),
                at(inBuildings, K::ShotgunShells), at(inBuildings, K::RifleAmmo),
                at(inBuildings, K::Pistol) + at(inBuildings, K::Shotgun) + at(inBuildings, K::Rifle),
                at(carried, K::Food), at(carried, K::Meat), at(carried, K::Water),
                at(carried, K::Bandage), at(carried, K::Medicine));
    const lw::Resources& resources = world.resources();
    std::printf("  forest: %lld portions in %d blocks (%d empty); rain filled barrels by %u\n",
                resources.forestTotal(), resources.forestBlocks(), resources.forestEmpty(),
                static_cast<unsigned>(resources.rainPortions()));
    if (people > 0) {
        const float n = static_cast<float>(people);
        std::printf("  mean needs: hunger %.2f, thirst %.2f, fatigue %.2f, fear %.2f, "
                    "loneliness %.2f, health %.2f (worst %.2f)\n",
                    mean.hunger / n, mean.thirst / n, mean.fatigue / n, mean.fear / n,
                    mean.loneliness / n, mean.health / n, worstHealth);
    }
}

// Отношения (этап 6.2): сколько знакомых, друзей и недругов у живых.
void printRelations(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::size_t people = 0;
    std::size_t bonds = 0;
    std::size_t friends = 0;
    std::size_t mutual = 0;
    std::size_t foes = 0;
    std::size_t lonely = 0;
    std::size_t gone = 0;
    float familiarity = 0.0f;
    for (const auto [entity, id, person, relations] :
         registry.view<const lw::Id, const lw::Person, const lw::Relations>().each()) {
        ++people;
        bonds += relations.count;
        bool hasFriend = false;
        for (std::size_t i = 0; i < relations.count; ++i) {
            const lw::Bond& bond = relations.bonds[i];
            familiarity += bond.familiarity;
            const entt::entity other = world.find(bond.other);
            const bool alive = other != entt::null && registry.all_of<lw::Person>(other);
            gone += alive ? 0 : 1;
            foes += bond.hostile() ? 1 : 0;
            if (bond.friendly()) {
                ++friends;
                hasFriend = hasFriend || alive;
                const auto* theirs = alive ? registry.try_get<lw::Relations>(other) : nullptr;
                const lw::Bond* back = theirs != nullptr ? theirs->find(id.value) : nullptr;
                // Пара считается один раз — у меньшего id.
                mutual += back != nullptr && back->friendly() && id.value < bond.other ? 1 : 0;
            }
        }
        lonely += hasFriend ? 0 : 1;
    }
    if (people == 0) {
        return;
    }
    std::printf("relations: %.1f bonds per person (mean familiarity %.2f), friends %zu "
                "(mutual pairs %zu), foes %zu, bonds with the dead %zu; no living friend %zu "
                "of %zu\n",
                static_cast<double>(bonds) / static_cast<double>(people),
                bonds > 0 ? familiarity / static_cast<float>(bonds) : 0.0f, friends, mutual, foes,
                gone, lonely, people);
}

// Память (этап 6.1), долг и обида (этап 6.2) у живых.
void printMemories(const lw::World& world) {
    const entt::registry& registry = world.registry();
    constexpr auto kKinds = static_cast<std::size_t>(lw::MemoryKind::Count);
    std::size_t byKind[kKinds] = {};
    std::size_t people = 0;
    std::size_t remembering = 0;
    std::size_t memories = 0;
    std::size_t debts = 0;
    std::size_t grudges = 0;
    float debt = 0.0f;
    float grudge = 0.0f;
    // Кто помнит больше всех (при равных — меньший id): его удобно открыть в
    // инспекторе (living_world --select ID).
    lw::EntityId richest = lw::kInvalidId;
    std::size_t most = 0;
    for (const auto [entity, id, person, memory] :
         registry.view<const lw::Id, const lw::Person, const lw::Memories>().each()) {
        ++people;
        memories += memory.count;
        remembering += memory.count > 0 ? 1 : 0;
        if (memory.count > most || (memory.count == most && most > 0 && id.value < richest)) {
            most = memory.count;
            richest = id.value;
        }
        for (std::size_t i = 0; i < memory.count; ++i) {
            ++byKind[static_cast<std::size_t>(memory.items[i].kind)];
        }
        if (const auto* relations = registry.try_get<lw::Relations>(entity)) {
            for (std::size_t i = 0; i < relations->count; ++i) {
                const lw::Bond& bond = relations->bonds[i];
                debts += bond.debt > 0.0f ? 1 : 0;
                grudges += bond.grudge > 0.0f ? 1 : 0;
                debt += bond.debt;
                grudge += bond.grudge;
            }
        }
    }
    if (people == 0) {
        return;
    }
    constexpr const char* kNames[] = {"saved", "defended", "abandoned", "robbed", "bereaved",
                                      "turned"};
    static_assert(std::size(kNames) == kKinds);
    std::printf("memories: %zu (%zu of %zu people remember something):", memories, remembering,
                people);
    for (std::size_t i = 0; i < kKinds; ++i) {
        std::printf("%s %s %zu", i == 0 ? "" : ",", kNames[i], byKind[i]);
    }
    std::printf("\n  debts %zu (mean %.2f), grudges %zu (mean %.2f)", debts,
                debts > 0 ? debt / static_cast<float>(debts) : 0.0f, grudges,
                grudges > 0 ? grudge / static_cast<float>(grudges) : 0.0f);
    if (richest != lw::kInvalidId) {
        std::printf("; remembers most: #%llu (%zu)", static_cast<unsigned long long>(richest),
                    most);
    }
    std::printf("\n");
}

// Восприятие: сколько контактов у существ и как они замечены.
void printPerception(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::size_t creatures = 0;
    std::size_t contacts = 0;
    std::size_t seen = 0;
    std::size_t heard = 0;
    std::size_t remembered = 0;
    std::size_t aware = 0; // замечают хоть кого-то сейчас
    for (const auto [entity, awareness] : registry.view<const lw::Awareness>().each()) {
        ++creatures;
        contacts += awareness.count;
        bool any = false;
        for (std::size_t i = 0; i < awareness.count; ++i) {
            const lw::Contact& contact = awareness.contacts[i];
            if (!lw::isFresh(contact, world.tick())) {
                ++remembered;
                continue;
            }
            any = true;
            seen += contact.seen ? 1 : 0;
            heard += contact.heard != lw::SoundKind::None ? 1 : 0;
        }
        aware += any ? 1 : 0;
    }
    if (creatures == 0) {
        return;
    }
    const lw::SenseConditions conditions = lw::senseConditions(world);
    std::printf("perception: sight x%.2f, hearing x%.2f; %zu of %zu notice someone; "
                "contacts %.2f per creature (seen %zu, heard %zu, remembered %zu); sounds %zu\n",
                conditions.sight, conditions.hearing, aware, creatures,
                static_cast<double>(contacts) / static_cast<double>(creatures), seen, heard,
                remembered, world.sounds().size());
}

// Зомби: чем заняты, сколько в ордах, какие орды.
void printZombies(const lw::World& world) {
    const entt::registry& registry = world.registry();
    constexpr const char* kStates[] = {"idle", "roam", "investigate", "chase", "follow"};
    static_assert(std::size(kStates) == static_cast<std::size_t>(lw::ZombieState::Count));
    std::size_t states[std::size(kStates)] = {};
    std::size_t zombies = 0;
    std::size_t loners = 0;
    for (const auto [entity, zombie] : registry.view<const lw::Zombie>().each()) {
        ++zombies;
        ++states[static_cast<std::size_t>(zombie.state)];
        loners += zombie.horde == lw::kInvalidId ? 1 : 0;
    }
    if (zombies == 0) {
        return;
    }
    std::printf("zombies %zu (alone %zu):", zombies, loners);
    for (std::size_t i = 0; i < std::size(kStates); ++i) {
        std::printf(" %s %zu%s", kStates[i], states[i], i + 1 < std::size(kStates) ? "," : "\n");
    }
    std::vector<std::size_t> sizes;
    std::size_t inSettlements = 0;
    for (const auto [entity, horde] : registry.view<const lw::Horde>().each()) {
        sizes.push_back(horde.members.size());
        inSettlements += horde.settlement != lw::kInvalidId ? 1 : 0;
    }
    std::sort(sizes.begin(), sizes.end());
    std::printf("  hordes %zu (in settlements %zu), sizes:", sizes.size(), inSettlements);
    for (const std::size_t size : sizes) {
        std::printf(" %zu", size);
    }
    std::printf("\n");
}

// Животные и птицы: сколько каких, чем заняты.
void printAnimals(const lw::World& world) {
    const entt::registry& registry = world.registry();
    constexpr const char* kKinds[] = {"deer", "boar", "hare"};
    constexpr const char* kStates[] = {"graze", "roam", "flee", "charge"};
    static_assert(std::size(kKinds) == static_cast<std::size_t>(lw::AnimalKind::Count));
    static_assert(std::size(kStates) == static_cast<std::size_t>(lw::AnimalState::Count));
    std::size_t kinds[std::size(kKinds)] = {};
    std::size_t states[std::size(kStates)] = {};
    std::size_t animals = 0;
    for (const auto [entity, animal] : registry.view<const lw::Animal>().each()) {
        ++animals;
        ++kinds[static_cast<std::size_t>(animal.kind)];
        ++states[static_cast<std::size_t>(animal.state)];
    }
    std::size_t flocks = 0;
    std::size_t flying = 0;
    std::size_t birds = 0;
    for (const auto [entity, flock] : registry.view<const lw::Flock>().each()) {
        ++flocks;
        flying += flock.flying ? 1 : 0;
        birds += flock.birds;
    }
    if (animals == 0 && flocks == 0) {
        return;
    }
    std::printf("animals %zu:", animals);
    for (std::size_t i = 0; i < std::size(kKinds); ++i) {
        std::printf(" %s %zu,", kKinds[i], kinds[i]);
    }
    for (std::size_t i = 0; i < std::size(kStates); ++i) {
        std::printf(" %s %zu%s", kStates[i], states[i], i + 1 < std::size(kStates) ? "," : "");
    }
    std::printf("; flocks %zu (%zu birds, flying %zu)\n", flocks, birds, flying);
}

// Бой: оружие и патроны у живых, раненые, тела.
void printCombat(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::size_t firearms = 0;
    long long ammo = 0;
    std::size_t melee = 0;
    for (const auto [entity, person, weapon] : registry.view<const lw::Person, const lw::Weapon>().each()) {
        if (weapon.kind == lw::WeaponKind::Pistol || weapon.kind == lw::WeaponKind::Shotgun ||
            weapon.kind == lw::WeaponKind::Rifle) {
            ++firearms;
            ammo += weapon.ammo;
        } else if (weapon.kind != lw::WeaponKind::Fists) {
            ++melee;
        }
    }
    std::size_t bleeding = 0;
    std::size_t bitten = 0;
    for (const auto [entity, wounds] : registry.view<const lw::Wounds>().each()) {
        bleeding += wounds.bleeding > 0.0f ? 1 : 0;
        bitten += wounds.infection > 0.0f ? 1 : 0;
    }
    // Погибшие люди по причинам — по летописи: тела со временем исчезают.
    constexpr const char* kCauses[] = {"wounds", "bleeding", "starvation", "dehydration",
                                       "exhaustion", "infection"};
    static_assert(std::size(kCauses) == static_cast<std::size_t>(lw::DeathCause::Count));
    std::size_t causes[std::size(kCauses)] = {};
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind == lw::ChronicleKind::Died) {
            ++causes[static_cast<std::size_t>(event.a)];
        }
    }
    std::size_t bodies[3] = {};
    for (const auto [entity, corpse] : registry.view<const lw::Corpse>().each()) {
        ++bodies[static_cast<std::size_t>(corpse.kind)];
    }
    std::printf("combat: firearms %zu (ammo %lld), melee weapons %zu; bleeding %zu, infected %zu; "
                "bodies: people %zu, zombies %zu, animals %zu\n",
                firearms, ammo, melee, bleeding, bitten, bodies[0], bodies[1], bodies[2]);
    std::printf("  people died:");
    for (std::size_t i = 0; i < std::size(kCauses); ++i) {
        std::printf(" %s %zu%s", kCauses[i], causes[i], i + 1 < std::size(kCauses) ? "," : "\n");
    }
}

// Летопись: сколько событий каждого вида.
void printChronicle(const lw::World& world) {
    const lw::Chronicle& chronicle = world.chronicle();
    std::printf("chronicle %zu events:", chronicle.events().size());
    const char* separator = " ";
    for (std::size_t i = 0; i < static_cast<std::size_t>(lw::ChronicleKind::Count); ++i) {
        const auto kind = static_cast<lw::ChronicleKind>(i);
        std::printf("%s%s %zu", separator, lw::chronicleKindName(kind), chronicle.countOf(kind));
        separator = ", ";
    }
    std::printf("\n");
}

void printWeather(const lw::World& world) {
    const lw::Environment environment = lw::environmentAt(world);
    const lw::CalendarTime& time = environment.time;
    const lw::Weather& weather = world.weather();
    std::printf("year %d, season %d, day %d of season, %02d:%02d; light %.2f, %.1f C, "
                "weather %d (clouds %.2f, precipitation %.2f, fog %.2f, wind %.1f m/s), snow %.2f\n",
                time.year, static_cast<int>(time.season), time.dayOfSeason + 1, time.hour,
                time.minute, environment.light, environment.temperature,
                static_cast<int>(weather.kind), weather.cloudCover, weather.precipitation,
                weather.fog, weather.windSpeed, weather.snowCover);
}

// Год погоды на маленькой карте: доли типов погоды, температура, снег — по сезонам.
// По сводке подбираются константы климата.
void printClimate(std::uint64_t seed) {
    lw::WorldConfig config;
    config.mapWidth = 64;
    config.mapHeight = 64;
    config.people = 0;
    config.zombies = 0;
    config.animals = 0;
    config.flocks = 0;
    lw::World world(seed, config);

    constexpr int kKinds = static_cast<int>(lw::WeatherKind::Count);
    struct SeasonStats {
        int hours = 0;
        int kinds[kKinds] = {};
        int snowfallHours = 0;
        int snowCoveredHours = 0;
        double temperatureSum = 0.0;
        float minTemperature = 1e9f;
        float maxTemperature = -1e9f;
    };
    SeasonStats seasons[4];

    for (int hour = 0; hour < lw::kGameDaysPerYear * 24; ++hour) {
        for (std::uint64_t i = 0; i < lw::kTicksPerGameHour; ++i) {
            world.step();
        }
        const lw::Environment environment = lw::environmentAt(world);
        const lw::Weather& weather = world.weather();
        SeasonStats& stats = seasons[static_cast<int>(environment.time.season)];
        ++stats.hours;
        ++stats.kinds[static_cast<int>(weather.kind)];
        stats.snowfallHours += weather.precipitation > 0.05f && lw::isSnowing(environment.temperature) ? 1 : 0;
        stats.snowCoveredHours += weather.snowCover > 0.2f ? 1 : 0;
        stats.temperatureSum += environment.temperature;
        stats.minTemperature = std::min(stats.minTemperature, environment.temperature);
        stats.maxTemperature = std::max(stats.maxTemperature, environment.temperature);
    }

    constexpr const char* kSeasonNames[] = {"spring", "summer", "autumn", "winter"};
    std::printf("climate, seed %llu: %% of hours by weather "
                "(clear cloudy overcast fog rain storm), temperature at sea level\n",
                static_cast<unsigned long long>(seed));
    for (int s = 0; s < 4; ++s) {
        const SeasonStats& stats = seasons[s];
        const double hours = stats.hours > 0 ? stats.hours : 1;
        std::printf("  %-6s", kSeasonNames[s]);
        for (const int count : stats.kinds) {
            std::printf(" %3.0f", 100.0 * count / hours);
        }
        std::printf(" | %+5.1f .. %+5.1f C, mean %+5.1f | snowfall %3d h, snow on ground %3d h\n",
                    stats.minTemperature, stats.maxTemperature, stats.temperatureSum / hours,
                    stats.snowfallHours, stats.snowCoveredHours);
    }
}


// Строка на игровые сутки: сколько кого, смерти по причинам, еда и патроны.
// По таким строкам подбирается баланс дефицита (этап 5.7).
void printDailyHeader() {
    std::printf("day season | people (hungry thirsty) | zombies hordes | animals | died: wounds "
                "bleed starve thirst exhaust infect | turned killedZ hunted | food bld/carried "
                "forest | ammo\n");
}

void printDaily(const lw::World& world) {
    const entt::registry& registry = world.registry();
    const lw::CalendarTime time = lw::calendarAt(world.tick());
    std::size_t people = 0;
    std::size_t hungry = 0;
    std::size_t thirsty = 0;
    for (const auto [entity, person, needs] : registry.view<const lw::Person, const lw::Needs>().each()) {
        ++people;
        hungry += needs.hunger >= 0.8f ? 1 : 0;
        thirsty += needs.thirst >= 0.8f ? 1 : 0;
    }
    std::size_t died[static_cast<std::size_t>(lw::DeathCause::Count)] = {};
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind == lw::ChronicleKind::Died) {
            ++died[static_cast<std::size_t>(event.a)];
        }
    }
    long long food = 0;
    for (const auto [entity, building, inventory] :
         registry.view<const lw::Building, const lw::Inventory>().each()) {
        food += inventory.food();
    }
    long long carried = 0;
    for (const auto [entity, person, inventory] :
         registry.view<const lw::Person, const lw::Inventory>().each()) {
        carried += inventory.food();
    }
    long long ammo = 0;
    for (const auto [entity, person, weapon] : registry.view<const lw::Person, const lw::Weapon>().each()) {
        ammo += weapon.ammo;
    }
    const lw::Chronicle& chronicle = world.chronicle();
    std::printf("%3d %d | %3zu (%3zu %3zu) | %3zu %2zu | %3zu | %3zu %3zu %3zu %3zu %3zu %3zu | "
                "%3zu %4zu %3zu | %5lld/%3lld %5lld | %4lld\n",
                time.day, static_cast<int>(time.season), people, hungry, thirsty,
                registry.view<const lw::Zombie>().size(), registry.view<const lw::Horde>().size(),
                registry.view<const lw::Animal>().size(), died[0], died[1], died[2], died[3],
                died[4], died[5], chronicle.countOf(lw::ChronicleKind::Turned),
                chronicle.countOf(lw::ChronicleKind::KilledZombie),
                chronicle.countOf(lw::ChronicleKind::Hunted), food, carried,
                world.resources().forestTotal(), ammo);
    std::fflush(stdout);
}

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Серия запросов к поиску пути и сводка: методы, время, раскрытые клетки, крюк.
// optimal — если задан, каждый путь сравнивается с оптимальным (A* по всей карте).
void runPathGroup(const char* name, lw::Pathfinder& finder, lw::Pathfinder* optimal, int count,
                  const std::function<std::pair<lw::Cell, lw::Cell>()>& nextPair) {
    constexpr int kMethods = static_cast<int>(lw::PathMethod::Full) + 1;
    int methods[kMethods] = {};
    int failed = 0;
    std::vector<double> times;
    double expandedSum = 0.0;
    std::uint32_t expandedMax = 0;
    double detourSum = 0.0;
    double costRatioSum = 0.0;
    int measured = 0;
    double overOptimalSum = 0.0;
    double overOptimalMax = 0.0;
    double optimalMs = 0.0;
    double overByMethod[kMethods] = {};
    for (int i = 0; i < count; ++i) {
        const auto [start, goal] = nextPair();
        const auto begin = Clock::now();
        const lw::Path path = finder.find(start, goal);
        times.push_back(millisSince(begin));
        if (!path.found()) {
            ++failed;
            continue;
        }
        ++methods[static_cast<int>(path.method)];
        if (optimal != nullptr) {
            const auto optimalBegin = Clock::now();
            const lw::Path best = optimal->find(start, goal);
            optimalMs += millisSince(optimalBegin);
            const double over = best.cost > 0 ? static_cast<double>(path.cost) / best.cost - 1.0 : 0.0;
            overOptimalSum += over;
            overOptimalMax = std::max(overOptimalMax, over);
            overByMethod[static_cast<int>(path.method)] += over;
        }
        expandedSum += path.expanded;
        expandedMax = std::max(expandedMax, path.expanded);
        const double dx = goal.x - start.x;
        const double dy = goal.y - start.y;
        const double straight = std::sqrt(dx * dx + dy * dy) * lw::Map::kCellSize;
        if (straight > 0.0) {
            detourSum += path.length / straight;
            costRatioSum += path.cost / static_cast<double>(lw::NavGrid::kCostPerRoadMeter) / straight;
            ++measured;
        }
    }
    std::sort(times.begin(), times.end());
    double total = 0.0;
    for (const double t : times) {
        total += t;
    }
    const auto percentile = [&](double p) {
        return times[std::min(times.size() - 1, static_cast<std::size_t>(p * times.size()))];
    };
    const int found = count - failed;
    std::printf("%s: %d paths, found %d (direct %d, graph %d, full %d), failed %d\n", name, count,
                found, methods[static_cast<int>(lw::PathMethod::Direct)],
                methods[static_cast<int>(lw::PathMethod::Graph)],
                methods[static_cast<int>(lw::PathMethod::Full)], failed);
    std::printf("  time ms: mean %.2f, p50 %.2f, p95 %.2f, max %.2f; expanded cells: mean %.0f, max %u\n",
                total / count, percentile(0.5), percentile(0.95), times.back(),
                found > 0 ? expandedSum / found : 0.0, expandedMax);
    if (measured > 0) {
        std::printf("  length / straight line %.2f, cost in road meters / straight line %.2f\n",
                    detourSum / measured, costRatioSum / measured);
    }
    if (optimal != nullptr && found > 0) {
        const int graph = methods[static_cast<int>(lw::PathMethod::Graph)];
        std::printf("  over optimal cost: mean %.1f%%, max %.1f%% (by graph %.1f%%); "
                    "optimal search mean %.1f ms\n",
                    100.0 * overOptimalSum / found, 100.0 * overOptimalMax,
                    graph > 0 ? 100.0 * overByMethod[static_cast<int>(lw::PathMethod::Graph)] / graph : 0.0,
                    optimalMs / found);
    }
}

// Замер поиска пути на карте мира: между дверями зданий (походы из дома в дом и
// из поселения в поселение) и между случайными клетками главной области.
void benchmarkPaths(const lw::World& world, int count, bool compare) {
    // Мир сетку и граф уже построил; здесь — заново, только ради времени.
    auto begin = Clock::now();
    lw::NavGrid nav;
    nav.build(world.map(), world.registry());
    const double navMs = millisSince(begin);
    begin = Clock::now();
    lw::NavGraph graph;
    graph.build(nav);
    const double graphMs = millisSince(begin);
    int byKind[4] = {};
    for (const lw::NavGraph::Node& node : graph.nodes()) {
        ++byKind[static_cast<int>(node.kind)];
    }
    std::printf("nav grid %.0f ms, %zu regions; nav graph %.0f ms, %zu nodes "
                "(road %d, land %d, water %d), %zu edges\n",
                navMs, nav.regionCount(), graphMs, graph.nodeCount(),
                byKind[static_cast<int>(lw::NavClass::Road)], byKind[static_cast<int>(lw::NavClass::Land)],
                byKind[static_cast<int>(lw::NavClass::Water)], graph.edgeCount() / 2);

    std::vector<std::pair<lw::EntityId, lw::Cell>> doors;
    for (const auto [entity, id, building] : world.registry().view<const lw::Id, const lw::Building>().each()) {
        doors.emplace_back(id.value, building.door);
    }
    std::sort(doors.begin(), doors.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (doors.empty()) {
        std::printf("no buildings\n");
        return;
    }
    // Свой генератор: замер не трогает потоки мира.
    lw::Rng rng(world.seed() ^ 0x9A7Bu);
    const auto randomDoor = [&]() {
        return doors[rng.below(static_cast<std::uint32_t>(doors.size()))].second;
    };
    const lw::NavGrid::Region mainland = nav.region(doors.front().second);
    const auto randomLand = [&]() {
        for (;;) {
            const lw::Cell cell{static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                                static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
            if (nav.region(cell) == mainland) {
                return cell;
            }
        }
    };

    lw::Pathfinder finder(nav, graph);
    lw::PathfinderSettings exhaustive;
    exhaustive.directRange = 1 << 20;
    exhaustive.directBudget = 0xFFFFFFFFu;
    lw::Pathfinder optimal(nav, graph, exhaustive);
    lw::Pathfinder* reference = compare ? &optimal : nullptr;
    runPathGroup("doors", finder, reference, count, [&]() { return std::make_pair(randomDoor(), randomDoor()); });
    runPathGroup("land", finder, reference, count, [&]() { return std::make_pair(randomLand(), randomLand()); });
    // Ближние: в пределах прямого поиска, как походы по поселению и в лес за дровами.
    runPathGroup("near", finder, reference, count, [&]() {
        const lw::Cell start = randomLand();
        for (;;) {
            const lw::Cell goal{start.x + rng.rangeInt(-120, 120), start.y + rng.rangeInt(-120, 120)};
            if (nav.region(goal) == mainland) {
                return std::make_pair(start, goal);
            }
        }
    });
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    std::uint64_t ticks = 1000;
    bool climate = false;
    int paths = 0;
    bool compare = false;
    bool daily = false;
    lw::WorldConfig config;

    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--seed") == 0 && hasValue) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--ticks") == 0 && hasValue) {
            ticks = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--climate") == 0) {
            climate = true;
        } else if (std::strcmp(argv[i], "--paths") == 0 && hasValue) {
            paths = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--optimal") == 0) {
            compare = true;
        } else if (std::strcmp(argv[i], "--daily") == 0) {
            daily = true;
        } else if (std::strcmp(argv[i], "--people") == 0 && hasValue) {
            config.people = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--zombies") == 0 && hasValue) {
            config.zombies = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--animals") == 0 && hasValue) {
            config.animals = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--flocks") == 0 && hasValue) {
            config.flocks = std::atoi(argv[++i]);
        } else {
            std::printf("usage: lw_headless [--seed N] [--ticks N] [--climate] [--people N] "
                        "[--zombies N] [--animals N] [--flocks N] [--paths N [--optimal]] [--daily]\n");
            return 2;
        }
    }

    if (climate) {
        printClimate(seed);
        return 0;
    }

    const auto genStart = std::chrono::steady_clock::now();
    lw::World world(seed, config);
    const std::chrono::duration<double> genElapsed = std::chrono::steady_clock::now() - genStart;
    printMapSummary(world, genElapsed.count());
    if (paths > 0) {
        benchmarkPaths(world, paths, compare);
    }

    const auto start = std::chrono::steady_clock::now();
    if (daily) {
        printDailyHeader();
    }
    for (std::uint64_t i = 0; i < ticks; ++i) {
        world.step();
        if (daily && world.tick() % lw::kTicksPerGameDay == 0) {
            printDaily(world);
        }
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    std::printf("seed=%llu ticks=%llu map=%dx%d entities=%zu hash=%016llx\n",
                static_cast<unsigned long long>(world.seed()),
                static_cast<unsigned long long>(world.tick()),
                world.map().width(),
                world.map().height(),
                world.entityCount(),
                static_cast<unsigned long long>(world.stateHash()));
    printWeather(world);
    printPeople(world);
    printRelations(world);
    printMemories(world);
    printPerception(world);
    printZombies(world);
    printAnimals(world);
    printCombat(world);
    printChronicle(world);
    if (elapsed.count() > 0.0) {
        std::printf("%.3f s, %.0f ticks/s\n", elapsed.count(),
                    static_cast<double>(ticks) / elapsed.count());
    }
    return 0;
}
