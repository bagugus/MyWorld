// Прогон симуляции без окна: проверка детерминизма и замер скорости.
//   lw_headless --seed 42 --ticks 10000
//   lw_headless --seed 42 --climate   сводка погоды и температуры за игровой год по сезонам
//   lw_headless --seed 42 --paths 500 замер поиска пути: пары дверей и пары случайных клеток суши
//                                     (--optimal — сравнить каждый путь с оптимальным)
//   lw_headless --seed 42 --people 3000 --ticks 144000   сутки мира с 3000 людей
//   lw_headless --seed 42 --zombies 400 --no-peace       мир сразу после катастрофы, как до этапа 7.2
//   lw_headless --seed 42 --animals 0 --flocks 0         мир без животных и птиц
//   lw_headless --seed 42 --ticks 8640000 --daily         год мира, строка на каждые сутки
//   lw_headless --seed 42 --ticks 1440000 --social        и список социальных событий (этап 6)
//   lw_headless --seed 42 --ticks 720000 --events         и журнал ивентов (этап 7)
//   lw_headless --seed 42 --event drop 3                 один сброс груза через 3 ч
//   lw_headless --seed 42 --event-schedule               временное расписание ивентов (до рассказчика)
//   lw_headless --seed 42 --event crash 3                крушение вертолёта через 3 ч (convoy — конвой)
//   lw_headless --seed 42 --no-events                    без ивентов по фазам сценария (этап 7.7)
//   lw_headless --seed 42 --outbreak 3 --daily           вспышка на 4-е сутки (0 — не по расписанию)
//   lw_headless --seed 42 --phase collapse               сразу в фазу коллапса (этап 7.4)
//   lw_headless --seed 42 --phase panic --stories        и сюжетные линии из летописи (этап 12а)
//   lw_headless --seed 42 --phase panic --foresight      что предвиделось и сбылось ли (этап 12а.2)
//   lw_headless --seed 42 --phase panic --cast           главные герои: кто, когда и через кого (этап 12а.3)
//   lw_headless --seed 42 --ticks 0 --families           семьи: дома, родня, сироты, прошлое (этап 7.3)
//   lw_headless --seed 42 --outbreak 1 --ticks 1440000 --infections   вспышка: кто от кого заразился (этап 7.5)

#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Events.h"
#include "sim/Family.h"
#include "sim/Cast.h"
#include "sim/Infection.h"
#include "sim/Foresight.h"
#include "sim/Names.h"
#include "sim/Perception.h"
#include "sim/Relations.h"
#include "sim/Rng.h"
#include "sim/Scenario.h"
#include "sim/Society.h"
#include "sim/Story.h"
#include "sim/Work.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"
#include "sim/nav/NavGraph.h"
#include "sim/systems/EventSystem.h"
#include "sim/systems/ScenarioSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// «John Miller #1745» — для людей (живых, погибших, обращённых), иначе «#1745».
std::string label(const lw::World& world, lw::EntityId id) {
    return lw::labelOf(world.registry(), world.find(id), id);
}

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
                                        "fight",  "hunt",      "treat",  "share",   "trade",
                                        "steal",  "avenge",    "salvage", "work",
                                        "buy",    "doctor",    "evacuate", "reunite"};
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
// Мирная жизнь (этап 7.2): кто работает, деньги, места работы, покупки.
void printWork(const lw::World& world) {
    const entt::registry& registry = world.registry();
    constexpr std::size_t kProfessions = static_cast<std::size_t>(lw::Profession::Count);
    std::size_t byProfession[kProfessions] = {};
    std::size_t people = 0;
    std::size_t onDuty = 0;
    long long money = 0;
    int poorest = 0;
    std::size_t broke = 0;
    std::size_t adults = 0;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        ++people;
        const auto* job = registry.try_get<lw::Job>(entity);
        ++byProfession[static_cast<std::size_t>(job != nullptr ? job->profession : lw::Profession::None)];
        onDuty += lw::isOnDuty(world, entity) && job->profession != lw::Profession::Pupil ? 1 : 0;
        // Деньги — у взрослых: у детей карманные (этап 7.3).
        const auto* wallet = registry.try_get<lw::Wallet>(entity);
        if (wallet != nullptr && !lw::isMinor(registry, entity)) {
            ++adults;
            money += wallet->money;
            poorest = adults == 1 ? wallet->money : std::min(poorest, wallet->money);
            broke += wallet->money < lw::kFoodPrice ? 1 : 0;
        }
    }
    const std::size_t pupils = byProfession[static_cast<std::size_t>(lw::Profession::Pupil)];
    std::printf("work: employed %zu of %zu (shopkeepers %zu, farmers %zu, doctors %zu, police %zu, "
                "teachers %zu), pupils %zu, on duty %zu; adults' money mean %.0f (min %d, broke %zu)\n",
                people - byProfession[0] - pupils, people, byProfession[1], byProfession[2], byProfession[3],
                byProfession[4], byProfession[5], pupils, onDuty,
                adults > 0 ? static_cast<double>(money) / static_cast<double>(adults) : 0.0, poorest,
                broke);
    // Здания по видам: всего, с работниками, открыто, еды в тех, где работают.
    constexpr lw::BuildingKind kKinds[] = {lw::BuildingKind::Shop, lw::BuildingKind::Barn,
                                           lw::BuildingKind::Hospital,
                                           lw::BuildingKind::PoliceStation, lw::BuildingKind::School};
    constexpr const char* kKindNames[] = {"shops", "farms", "hospitals", "police", "schools"};
    std::printf("  workplaces:");
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        std::size_t total = 0;
        std::size_t staffed = 0;
        std::size_t open = 0;
        long long food = 0;
        for (const auto [entity, building] : registry.view<const lw::Building>().each()) {
            if (building.kind != kKinds[k]) {
                continue;
            }
            ++total;
            if (const auto* workplace = registry.try_get<lw::Workplace>(entity)) {
                ++staffed;
                open += workplace->onDuty > 0 ? 1 : 0;
                if (const auto* inventory = registry.try_get<lw::Inventory>(entity)) {
                    food += inventory->food();
                }
            }
        }
        std::printf(" %s %zu of %zu (open %zu, food %lld)%s", kKindNames[k], staffed, total, open,
                    food, k + 1 < std::size(kKinds) ? "," : "\n");
    }
    // Жители и работающие по поселениям: «Зеленогорск 80/20».
    std::printf("  residents/employed:");
    for (const lw::Places::SettlementPlace& place : world.places().settlements()) {
        std::size_t residents = 0;
        std::size_t employed = 0;
        for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
            const entt::entity home = world.find(person.home);
            if (home != entt::null && registry.get<lw::Building>(home).settlement == place.id) {
                ++residents;
                employed += lw::isEmployed(registry.try_get<lw::Job>(entity)) ? 1 : 0;
            }
        }
        std::printf(" %s %zu/%zu", registry.get<lw::Settlement>(world.find(place.id)).name.c_str(),
                    residents, employed);
    }
    std::printf("\n");
    long long food = 0;
    long long water = 0;
    std::size_t empty = 0;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind == lw::ChronicleKind::Bought) {
            food += event.a;
            water += event.b;
            empty += event.a == 0 && event.b == 0 ? 1 : 0;
        }
    }
    const lw::Chronicle& chronicle = world.chronicle();
    std::printf("  bought %zu times (empty shelves %zu): food %lld, water %lld; sold out %zu; "
                "doctor visits %zu\n",
                chronicle.countOf(lw::ChronicleKind::Bought), empty, food, water,
                chronicle.countOf(lw::ChronicleKind::SoldOut),
                chronicle.countOf(lw::ChronicleKind::SawDoctor));
}

void printRelations(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::size_t people = 0;
    std::size_t bonds = 0;
    std::size_t friends = 0;
    std::size_t mutual = 0;
    std::size_t foes = 0;
    std::size_t lonely = 0;
    std::size_t friendless = 0; // нет живого друга вне семьи
    std::size_t gone = 0;
    std::size_t kin = 0;
    float familiarity = 0.0f;
    for (const auto [entity, id, person, relations] :
         registry.view<const lw::Id, const lw::Person, const lw::Relations>().each()) {
        ++people;
        bonds += relations.count;
        bool hasFriend = false;
        bool hasOutsideFriend = false;
        for (std::size_t i = 0; i < relations.count; ++i) {
            const lw::Bond& bond = relations.bonds[i];
            familiarity += bond.familiarity;
            kin += bond.origin == lw::BondOrigin::Kin ? 1 : 0;
            const entt::entity other = world.find(bond.other);
            const bool alive = other != entt::null && registry.all_of<lw::Person>(other);
            gone += alive ? 0 : 1;
            foes += bond.hostile() ? 1 : 0;
            if (bond.friendly()) {
                ++friends;
                hasFriend = hasFriend || alive;
                hasOutsideFriend = hasOutsideFriend || (alive && bond.origin != lw::BondOrigin::Kin);
                const auto* theirs = alive ? registry.try_get<lw::Relations>(other) : nullptr;
                const lw::Bond* back = theirs != nullptr ? theirs->find(id.value) : nullptr;
                // Пара считается один раз — у меньшего id.
                mutual += back != nullptr && back->friendly() && id.value < bond.other ? 1 : 0;
            }
        }
        lonely += hasFriend ? 0 : 1;
        friendless += hasOutsideFriend ? 0 : 1;
    }
    if (people == 0) {
        return;
    }
    std::printf("relations: %.1f bonds per person (mean familiarity %.2f, kin %zu), friends %zu "
                "(mutual pairs %zu), foes %zu, bonds with the dead %zu; no living friend %zu "
                "of %zu (outside the family %zu)\n",
                static_cast<double>(bonds) / static_cast<double>(people),
                bonds > 0 ? familiarity / static_cast<float>(bonds) : 0.0f, kin, friends, mutual, foes,
                gone, lonely, people, friendless);
}

// Семьи и прошлое (этап 7.3): возраст, родня, опекуны, прошлые связи; с
// detailed — состав домов и события сирот.
void printFamilies(const lw::World& world, bool detailed) {
    const entt::registry& registry = world.registry();
    std::size_t people = 0;
    std::size_t children = 0;
    std::size_t elders = 0;
    std::size_t orphans = 0;   // ребёнок без живых родителей
    std::size_t fostered = 0;  // опекун — не родитель
    std::size_t alone = 0;     // ребёнок без опекуна
    std::size_t apart = 0;     // родня, живущая в другом доме (связей)
    constexpr auto kKinds = static_cast<std::size_t>(lw::KinKind::Count);
    std::size_t links[kKinds] = {};
    constexpr auto kOrigins = static_cast<std::size_t>(lw::BondOrigin::Count);
    std::size_t origins[kOrigins] = {};
    long long ageSum = 0;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        ++people;
        const int age = lw::ageOf(registry, entity);
        ageSum += age;
        elders += age >= 65 ? 1 : 0;
        if (const auto* kin = registry.try_get<lw::Kin>(entity)) {
            bool parent = false;
            for (std::size_t i = 0; i < kin->count; ++i) {
                const lw::Relative& relative = kin->relatives[i];
                ++links[static_cast<std::size_t>(relative.kind)];
                const entt::entity other = world.find(relative.id);
                const auto* theirs = other != entt::null ? registry.try_get<lw::Person>(other) : nullptr;
                apart += theirs != nullptr && theirs->home != person.home ? 1 : 0;
                parent = parent || (relative.kind == lw::KinKind::Parent && theirs != nullptr);
            }
            if (age < lw::kAdultAge) {
                ++children;
                orphans += parent ? 0 : 1;
                alone += kin->guardian == lw::kInvalidId ? 1 : 0;
                fostered += kin->guardian != lw::kInvalidId &&
                                    lw::kinOf(registry, entity, kin->guardian) != lw::KinKind::Parent
                                ? 1
                                : 0;
            }
        }
        if (const auto* relations = registry.try_get<lw::Relations>(entity)) {
            for (std::size_t i = 0; i < relations->count; ++i) {
                ++origins[static_cast<std::size_t>(relations->bonds[i].origin)];
            }
        }
    }
    if (people == 0) {
        return;
    }
    const lw::Chronicle& chronicle = world.chronicle();
    // Связи — с обеих сторон: пары — пополам.
    std::printf("families: children %zu, elders 65+ %zu, mean age %.0f; spouses %zu pairs, parent-child %zu, "
                "siblings %zu pairs, grandparent-grandchild %zu; kin living apart %zu links; orphans %zu, "
                "fostered %zu, alone %zu; orphaned %zu, took in %zu\n",
                children, elders, static_cast<double>(ageSum) / static_cast<double>(people),
                links[static_cast<std::size_t>(lw::KinKind::Spouse)] / 2,
                links[static_cast<std::size_t>(lw::KinKind::Child)],
                links[static_cast<std::size_t>(lw::KinKind::Sibling)] / 2,
                links[static_cast<std::size_t>(lw::KinKind::Grandchild)], apart, orphans, fostered, alone,
                chronicle.countOf(lw::ChronicleKind::Orphaned), chronicle.countOf(lw::ChronicleKind::TookIn));
    std::printf("  bonds by origin: met %zu, housemates %zu, neighbours %zu, kin %zu, colleagues %zu, "
                "classmates %zu, old friends %zu, old feuds %zu, ex-partners %zu\n",
                origins[0], origins[1], origins[2], origins[3], origins[4], origins[5], origins[6],
                origins[7], origins[8]);
    if (!detailed) {
        return;
    }
    static constexpr const char* kKinNames[] = {"spouse", "parent", "child", "sibling", "grandparent",
                                                "grandchild"};
    static_assert(std::size(kKinNames) == kKinds);
    // Дома с жильцами — по id дома: кто живёт и кем друг другу приходятся.
    std::vector<std::pair<lw::EntityId, lw::EntityId>> residents; // дом, человек
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        if (person.home != lw::kInvalidId) {
            residents.emplace_back(person.home, id.value);
        }
    }
    std::sort(residents.begin(), residents.end());
    std::printf("households (first 20 homes):\n");
    int shown = 0;
    for (std::size_t begin = 0; begin < residents.size() && shown < 20; ++shown) {
        std::size_t end = begin;
        while (end < residents.size() && residents[end].first == residents[begin].first) {
            ++end;
        }
        std::printf("  home #%llu:", static_cast<unsigned long long>(residents[begin].first));
        for (std::size_t i = begin; i < end; ++i) {
            const entt::entity entity = world.find(residents[i].second);
            std::printf(" %s (%d)", label(world, residents[i].second).c_str(), lw::ageOf(registry, entity));
        }
        std::printf("\n");
        for (std::size_t i = begin; i < end; ++i) {
            const auto* kin = registry.try_get<lw::Kin>(world.find(residents[i].second));
            if (kin == nullptr || kin->count == 0) {
                continue;
            }
            std::printf("    %s:", label(world, residents[i].second).c_str());
            for (std::size_t k = 0; k < kin->count; ++k) {
                const lw::Relative& relative = kin->relatives[k];
                const entt::entity other = world.find(relative.id);
                const auto* theirs = other != entt::null ? registry.try_get<lw::Person>(other) : nullptr;
                std::printf(" %s %s%s", kKinNames[static_cast<std::size_t>(relative.kind)],
                            label(world, relative.id).c_str(),
                            theirs != nullptr && theirs->home != residents[begin].first ? " (elsewhere)" : "");
            }
            if (kin->guardian != lw::kInvalidId) {
                std::printf("; guardian %s", label(world, kin->guardian).c_str());
            }
            std::printf("\n");
        }
        begin = end;
    }
    // Прошлые связи и судьбы детей — по летописи и связям.
    std::printf("past ties:\n");
    int ties = 0;
    for (const auto [entity, id, relations] : registry.view<const lw::Id, const lw::Relations>().each()) {
        for (std::size_t i = 0; i < relations.count && ties < 15; ++i) {
            const lw::Bond& bond = relations.bonds[i];
            if (bond.other < id.value || (bond.origin != lw::BondOrigin::OldFriend &&
                                          bond.origin != lw::BondOrigin::OldFeud &&
                                          bond.origin != lw::BondOrigin::ExPartner)) {
                continue;
            }
            static constexpr const char* kPast[] = {"old friends", "old feud", "ex-partners"};
            ++ties;
            std::printf("  %s: %s and %s (affinity %+.2f)\n",
                        kPast[static_cast<int>(bond.origin) - static_cast<int>(lw::BondOrigin::OldFriend)],
                        label(world, id.value).c_str(), label(world, bond.other).c_str(), bond.affinity);
        }
    }
    for (const lw::ChronicleEvent& event : chronicle.events()) {
        if (event.kind == lw::ChronicleKind::Orphaned) {
            std::printf("  tick %llu: %s orphaned (last parent %s)\n", static_cast<unsigned long long>(event.tick),
                        label(world, event.subject).c_str(), label(world, event.object).c_str());
        } else if (event.kind == lw::ChronicleKind::TookIn) {
            lw::KinKind kind = lw::KinKind::Count;
            bool female = false;
            const bool kin = lw::decodeKin(event.a, kind, female);
            std::printf("  tick %llu: %s took in %s (%s)%s\n", static_cast<unsigned long long>(event.tick),
                        label(world, event.subject).c_str(), label(world, event.object).c_str(),
                        kin ? kKinNames[static_cast<std::size_t>(kind)] : "not kin",
                        event.b == 1 ? ", into the group" : "");
        }
    }
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
    // Как узнал (этап 6.3): с ним самим, видел чужое, слышал.
    std::size_t own = 0;
    std::size_t witnessed = 0;
    std::size_t heard = 0;
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
            const lw::Memory& item = memory.items[i];
            ++byKind[static_cast<std::size_t>(item.kind)];
            if (item.source != lw::kInvalidId) {
                ++heard;
            } else if (item.victim != lw::kInvalidId) {
                ++witnessed;
            } else {
                ++own;
            }
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
    constexpr const char* kNames[] = {"saved",    "defended", "abandoned", "robbed",
                                      "bereaved", "turned",   "expelled",  "helped",
                                      "betrayed", "assaulted", "killed"};
    static_assert(std::size(kNames) == kKinds);
    std::printf("memories: %zu (%zu of %zu people remember something):", memories, remembering,
                people);
    for (std::size_t i = 0; i < kKinds; ++i) {
        std::printf("%s %s %zu", i == 0 ? "" : ",", kNames[i], byKind[i]);
    }
    std::printf("\n  own %zu, witnessed %zu, heard %zu", own, witnessed, heard);
    std::printf("; debts %zu (mean %.2f), grudges %zu (mean %.2f)", debts,
                debts > 0 ? debt / static_cast<float>(debts) : 0.0f, grudges,
                grudges > 0 ? grudge / static_cast<float>(grudges) : 0.0f);
    if (richest != lw::kInvalidId) {
        std::printf("; remembers most: %s (%zu)", label(world, richest).c_str(), most);
    }
    std::printf("\n");
    // Репутация (этап 6.3): кого знают многие и любят меньше и больше всех.
    lw::EntityId worst = lw::kInvalidId;
    lw::EntityId best = lw::kInvalidId;
    lw::Reputation worstReputation;
    lw::Reputation bestReputation;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        const lw::Reputation reputation = lw::reputationOf(world, id.value);
        if (reputation.known < 3) {
            continue;
        }
        if (worst == lw::kInvalidId || reputation.affinity < worstReputation.affinity) {
            worst = id.value;
            worstReputation = reputation;
        }
        if (best == lw::kInvalidId || reputation.affinity > bestReputation.affinity) {
            best = id.value;
            bestReputation = reputation;
        }
    }
    if (worst != lw::kInvalidId) {
        std::printf("  reputation (known by 3+): worst %s %+.2f (%d), best %s %+.2f (%d)\n",
                    label(world, worst).c_str(), worstReputation.affinity, worstReputation.known,
                    label(world, best).c_str(), bestReputation.affinity, bestReputation.known);
    }
}

// Группы (этап 6.4): сколько, каких размеров, сколько людей в группах,
// самая большая (её удобно открыть в окне: living_world --select ID).
void printGroups(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::vector<std::size_t> sizes;
    std::size_t members = 0;
    std::size_t moved = 0;
    lw::EntityId largest = lw::kInvalidId;
    std::size_t most = 0;
    for (const auto [entity, id, group] : registry.view<const lw::Id, const lw::Group>().each()) {
        sizes.push_back(group.members.size());
        members += group.members.size();
        if (group.members.size() > most || (group.members.size() == most && id.value < largest)) {
            most = group.members.size();
            largest = id.value;
        }
    }
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        moved += person.formerHome != lw::kInvalidId ? 1 : 0;
    }
    if (sizes.empty()) {
        std::printf("groups 0\n");
        return;
    }
    std::sort(sizes.begin(), sizes.end());
    std::printf("groups %zu (%zu people, %zu moved to a base), sizes:", sizes.size(), members, moved);
    for (const std::size_t size : sizes) {
        std::printf(" %zu", size);
    }
    std::printf("; largest #%llu\n", static_cast<unsigned long long>(largest));
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
                                       "exhaustion", "infection", "crash"};
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

// Социальные события этапа 6 по порядку — из них читаются истории: спасения,
// предательства, месть, группы, убийства людьми.
void printSocialLog(const lw::World& world) {
    using K = lw::ChronicleKind;
    const entt::registry& registry = world.registry();
    const auto isPerson = [&](lw::EntityId id) {
        // Убийца — человек: живой человек или тело человека (обратившийся
        // помнит черты, но он зомби).
        const entt::entity entity = world.find(id);
        if (entity == entt::null || registry.all_of<lw::Zombie>(entity)) {
            return false;
        }
        const auto* corpse = registry.try_get<lw::Corpse>(entity);
        return corpse != nullptr ? corpse->kind == lw::CorpseKind::Human
                                 : registry.all_of<lw::Person>(entity);
    };
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        switch (event.kind) {
        case K::SavedBy:
        case K::DefendedBy:
        case K::AbandonedBy:
        case K::HeardOf:
        case K::Reconciled:
        case K::GroupFormed:
        case K::LeftGroup:
        case K::Expelled:
        case K::LedGroup:
        case K::GroupDisbanded:
        case K::Shared:
        case K::Traded:
        case K::Stole:
        case K::Deserted:
        case K::BetrayedBy:
        case K::AssaultedBy:
        case K::Avenged:
            break;
        case K::Witnessed:
            if (event.a != static_cast<std::int32_t>(lw::MemoryKind::Killed)) {
                continue;
            }
            break;
        case K::Died:
            if (event.object == lw::kInvalidId || !isPerson(event.object)) {
                continue;
            }
            break;
        default:
            continue;
        }
        const lw::CalendarTime time = lw::calendarAt(event.tick);
        std::printf("  day %2d %02d:%02d %-15s %s -> %s a=%d b=%d", time.day, time.hour,
                    time.minute, lw::chronicleKindName(event.kind),
                    label(world, event.subject).c_str(), label(world, event.object).c_str(),
                    event.a, event.b);
        if (event.third != lw::kInvalidId) {
            std::printf(" third=%s", label(world, event.third).c_str());
        }
        std::printf("\n");
    }
}

// Сюжетные линии (этап 12а.1): сколько каких и чем кончились, затем самые
// длинные — со всеми событиями по порядку.
void printStories(const lw::World& world) {
    lw::StoryTracker tracker;
    tracker.update(world);
    const std::vector<lw::Story>& stories = tracker.stories();
    constexpr auto kKinds = static_cast<std::size_t>(lw::StoryKind::Count);
    constexpr auto kEndings = static_cast<std::size_t>(lw::StoryEnding::Count);
    std::size_t total[kKinds] = {};
    std::size_t active[kKinds] = {};
    std::size_t betrayals = 0;
    std::size_t endings[kEndings] = {};
    for (const lw::Story& story : stories) {
        const auto kind = static_cast<std::size_t>(story.kind);
        ++total[kind];
        active[kind] += story.active() ? 1 : 0;
        betrayals += story.betrayal ? 1 : 0;
        ++endings[static_cast<std::size_t>(story.ending)];
    }
    std::printf("stories: %zu (betrayals %zu)\n", stories.size(), betrayals);
    for (std::size_t kind = 0; kind < kKinds; ++kind) {
        std::printf("  %-10s %4zu, active %zu\n",
                    lw::storyKindName(static_cast<lw::StoryKind>(kind)), total[kind], active[kind]);
    }
    std::printf("  endings:");
    for (std::size_t ending = 1; ending < kEndings; ++ending) {
        std::printf(" %s %zu%s", lw::storyEndingName(static_cast<lw::StoryEnding>(ending)),
                    endings[ending], ending + 1 < kEndings ? "," : "\n");
    }

    std::vector<const lw::Story*> longest;
    for (const lw::Story& story : stories) {
        longest.push_back(&story);
    }
    std::stable_sort(longest.begin(), longest.end(), [](const lw::Story* a, const lw::Story* b) {
        return a->beats.size() > b->beats.size();
    });
    constexpr std::size_t kShown = 8;
    constexpr std::size_t kBeats = 16;
    const std::vector<lw::ChronicleEvent>& events = world.chronicle().events();
    for (std::size_t i = 0; i < std::min(kShown, longest.size()); ++i) {
        const lw::Story& story = *longest[i];
        std::printf("story %u %s%s: hero %s", story.id, lw::storyKindName(story.kind),
                    story.betrayal ? " (betrayal)" : "", label(world, story.hero).c_str());
        if (story.other != lw::kInvalidId) {
            std::printf(", other %s", label(world, story.other).c_str());
        }
        if (story.focus != lw::kInvalidId) {
            std::printf(", about #%llu", static_cast<unsigned long long>(story.focus));
        }
        std::printf(" — %s, %s, cast %zu, interest %.2f\n", lw::storyStageName(story.stage),
                    lw::storyEndingName(story.active() ? lw::expectedEnding(story) : story.ending),
                    story.cast.size(), lw::storyInterest(story, world.tick()));
        for (std::size_t b = 0; b < story.beats.size(); ++b) {
            if (b == kBeats && story.beats.size() > kBeats + 1) {
                std::printf("    ... %zu more\n", story.beats.size() - kBeats);
                b = story.beats.size() - 1; // и последнее событие
            }
            const lw::StoryBeat& beat = story.beats[b];
            const lw::ChronicleEvent& event = events[beat.event];
            const lw::CalendarTime time = lw::calendarAt(event.tick);
            std::printf("    day %2d %02d:%02d %-7s %-16s %s -> %s a=%d b=%d", time.day, time.hour,
                        time.minute, lw::storyStageName(beat.stage),
                        lw::chronicleKindName(event.kind), label(world, event.subject).c_str(),
                        label(world, event.object).c_str(), event.a, event.b);
            if (event.third != lw::kInvalidId) {
                std::printf(" third=%s", label(world, event.third).c_str());
            }
            std::printf("\n");
        }
    }
}

// Главные герои (этап 12а.3): состав по порядку вхождения — когда вошёл, через
// кого и каким событием, вес биографии, линии и чем кончилось.
void printCast(const lw::World& world) {
    lw::CastTracker cast;
    cast.update(world);
    lw::StoryTracker tracker;
    tracker.update(world);
    const std::vector<lw::ChronicleEvent>& events = world.chronicle().events();
    const auto when = [](std::uint64_t tick) {
        const lw::CalendarTime time = lw::calendarAt(tick);
        char text[32];
        std::snprintf(text, sizeof(text), "day %d %02d:%02d", time.day, time.hour, time.minute);
        return std::string(text);
    };
    std::printf("cast: %zu heroes, alive %zu\n", cast.heroes().size(), cast.living());
    for (const lw::Protagonist& hero : cast.heroes()) {
        std::printf("  %-28s since %s, weight %.1f, deeds %u, stories %zu, %s",
                    label(world, hero.id).c_str(), when(hero.since).c_str(), cast.weight(hero.id),
                    cast.deeds(hero.id), tracker.storiesOf(hero.id).size(),
                    lw::heroStatusName(hero.status));
        if (!hero.alive()) {
            std::printf(" at %s", when(hero.ended).c_str());
        }
        std::printf("\n");
        if (hero.introducedBy != lw::kInvalidId) {
            std::printf("    via %s (link %.1f)", label(world, hero.introducedBy).c_str(),
                        cast.link(hero.id, hero.introducedBy));
            if (hero.via != lw::Protagonist::kNoEvent) {
                const lw::ChronicleEvent& event = events[hero.via];
                std::printf(": %s %s %s -> %s", when(event.tick).c_str(),
                            lw::chronicleKindName(event.kind), label(world, event.subject).c_str(),
                            label(world, event.object).c_str());
                if (event.third != lw::kInvalidId) {
                    std::printf(" third=%s", label(world, event.third).c_str());
                }
            }
            std::printf("\n");
        }
    }
}

// Предвкушение (этап 12а.2): предвестия, замеченные по ходу прогона (раз в
// игровую минуту), — сбылись ли и насколько заранее их было видно.
class ForesightLog {
public:
    void sample(const lw::World& world) {
        const std::uint64_t tick = world.tick();
        for (const lw::Foresight& omen : lw::foresee(world)) {
            const auto open = std::find_if(watches_.begin(), watches_.end(), [&](const Watch& watch) {
                return watch.last == previous_ && watch.first.same(omen);
            });
            if (open != watches_.end()) {
                open->last = tick;
                open->lastEta = omen.eta;
            } else {
                watches_.push_back({omen, tick, tick, omen.eta});
            }
        }
        previous_ = tick;
    }

    void print(const lw::World& world) const {
        constexpr auto kKinds = static_cast<std::size_t>(lw::ForesightKind::Count);
        // Сбылось, если нужное событие случилось, пока предвестие было видно,
        // или вскоре после. Обращение — к последнему сроку и ещё за игровой час:
        // предвестие пропадает, когда рядом никого, а тело встаёт позже. Срок
        // не вышел к концу прогона — «ещё ждём», в долю сбывшихся не идёт.
        constexpr std::uint64_t kGrace = 10 * lw::kTicksPerGameMinute;
        const std::vector<lw::ChronicleEvent>& events = world.chronicle().events();
        std::size_t seen[kKinds] = {};
        std::size_t fulfilled[kKinds] = {};
        std::size_t pending[kKinds] = {};
        double lead[kKinds] = {};
        double error[kKinds] = {};
        std::vector<std::pair<const Watch*, std::uint64_t>> examples[kKinds];
        for (const Watch& watch : watches_) {
            const auto kind = static_cast<std::size_t>(watch.first.kind);
            ++seen[kind];
            const std::uint64_t until =
                watch.last +
                (watch.first.kind == lw::ForesightKind::Turning
                     ? static_cast<std::uint64_t>(watch.lastEta * lw::kTicksPerSecond) +
                           lw::kTicksPerGameHour
                     : kGrace);
            // События летописи идут по времени: только окно [seen, until].
            std::uint64_t when = 0;
            auto event = std::lower_bound(
                events.begin(), events.end(), watch.seen,
                [](const lw::ChronicleEvent& e, std::uint64_t tick) { return e.tick < tick; });
            for (; event != events.end() && event->tick <= until; ++event) {
                if (lw::fulfilledBy(world, watch.first, *event)) {
                    when = event->tick;
                    break;
                }
            }
            if (when == 0) {
                pending[kind] += until > world.tick() ? 1 : 0;
                continue;
            }
            ++fulfilled[kind];
            const double seconds = static_cast<double>(when - watch.seen) / lw::kTicksPerSecond;
            lead[kind] += seconds;
            error[kind] += std::abs(seconds - watch.first.eta);
            if (examples[kind].size() < 2) {
                examples[kind].emplace_back(&watch, when);
            }
        }
        std::printf("foresight: %zu seen (sampled each game minute)\n", watches_.size());
        for (std::size_t kind = 0; kind < kKinds; ++kind) {
            const double n = static_cast<double>(std::max<std::size_t>(fulfilled[kind], 1));
            const std::size_t settled = seen[kind] - pending[kind];
            std::printf("  %-10s seen %4zu, came true %4zu (%3.0f%%), pending %3zu, "
                        "lead mean %6.0f s, eta error mean %6.0f s\n",
                        lw::foresightKindName(static_cast<lw::ForesightKind>(kind)), seen[kind],
                        fulfilled[kind],
                        settled > 0 ? 100.0 * static_cast<double>(fulfilled[kind]) /
                                          static_cast<double>(settled)
                                    : 0.0,
                        pending[kind], lead[kind] / n, error[kind] / n);
            for (const auto& [watch, when] : examples[kind]) {
                const lw::CalendarTime time = lw::calendarAt(watch->seen);
                std::printf("    day %2d %02d:%02d %s -> %s (about #%llu): %.0f m, eta %.0f s, "
                            "came true in %.0f s\n",
                            time.day, time.hour, time.minute, label(world, watch->first.hero).c_str(),
                            label(world, watch->first.other).c_str(),
                            static_cast<unsigned long long>(watch->first.focus),
                            watch->first.distance, watch->first.eta,
                            static_cast<double>(when - watch->seen) / lw::kTicksPerSecond);
            }
        }
    }

private:
    struct Watch {
        lw::Foresight first;   // каким было, когда заметили
        std::uint64_t seen = 0; // тик, когда заметили
        std::uint64_t last = 0; // тик последнего замера, где оно было
        float lastEta = 0.0f;   // оценка срока на последнем замере, секунды
    };
    std::vector<Watch> watches_;
    std::uint64_t previous_ = ~std::uint64_t{0};
};

// Ивенты (этап 7.1): сколько было, сколько идёт, что осталось в грузах, кто
// узнал и сколько унесли.
void printEvents(const lw::World& world) {
    using K = lw::ChronicleKind;
    const entt::registry& registry = world.registry();
    const lw::Chronicle& chronicle = world.chronicle();
    std::size_t clues[3] = {};
    std::size_t empty = 0;
    long long food = 0;
    long long water = 0;
    for (const lw::ChronicleEvent& event : chronicle.events()) {
        if (event.kind == K::LearnedOfEvent) {
            ++clues[std::min<std::size_t>(static_cast<std::size_t>(event.a), 2)];
        } else if (event.kind == K::Salvaged) {
            empty += event.a == 0 && event.b == 0 ? 1 : 0;
            food += event.a;
            water += event.b;
        }
    }
    lw::Inventory left;
    for (const auto [entity, cache, inventory] :
         registry.view<const lw::Cache, const lw::Inventory>().each()) {
        for (std::size_t i = 0; i < lw::kItemKinds; ++i) {
            left.items[i] = static_cast<std::uint16_t>(left.items[i] + inventory.items[i]);
        }
    }
    std::size_t knowing = 0;
    for (const auto [entity, leads] : registry.view<const lw::Leads>().each()) {
        knowing += leads.count > 0 ? 1 : 0;
    }
    std::size_t heading = 0;
    for (const auto [entity, mind] : registry.view<const lw::Mind>().each()) {
        heading += mind.action == lw::Action::Salvage ? 1 : 0;
    }
    std::printf("events: began %zu, ended %zu, active %zu; caches %zu (food %d, water %d, "
                "bandages %d, medicine %d)\n",
                chronicle.countOf(K::EventBegan), chronicle.countOf(K::EventEnded),
                registry.view<const lw::WorldEvent>().size(),
                registry.view<const lw::Cache>().size(), left.food(),
                left.count(lw::ItemKind::Water), left.count(lw::ItemKind::Bandage),
                left.count(lw::ItemKind::Medicine));
    std::printf("  learned: saw %zu, heard %zu, told %zu; people with leads %zu, heading there %zu; "
                "salvaged %zu times (empty %zu): food %lld, water %lld\n",
                clues[0], clues[1], clues[2], knowing, heading, chronicle.countOf(K::Salvaged),
                empty, food, water);
}

// Журнал ивентов по порядку: начало и конец ивентов, кто что забрал из грузов;
// вести — числом на ивент, иначе журнал утонет в них.
void printEventLog(const lw::World& world) {
    using K = lw::ChronicleKind;
    std::vector<std::pair<lw::EntityId, int>> learned;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind == K::LearnedOfEvent) {
            auto it = std::find_if(learned.begin(), learned.end(),
                                   [&](const auto& item) { return item.first == event.object; });
            if (it == learned.end()) {
                learned.emplace_back(event.object, 0);
                it = learned.end() - 1;
            }
            ++it->second;
        }
    }
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind != K::EventBegan && event.kind != K::EventEnded && event.kind != K::Salvaged) {
            continue;
        }
        const lw::CalendarTime time = lw::calendarAt(event.tick);
        std::printf("  day %2d %02d:%02d %-12s %s -> %s a=%d b=%d", time.day, time.hour,
                    time.minute, lw::chronicleKindName(event.kind),
                    label(world, event.subject).c_str(), label(world, event.object).c_str(),
                    event.a, event.b);
        if (event.kind == K::EventBegan) {
            const auto it = std::find_if(learned.begin(), learned.end(),
                                         [&](const auto& item) { return item.first == event.subject; });
            std::printf(" (%s, learned by %d)", lw::eventSpec(static_cast<lw::EventKind>(event.a)).name,
                        it != learned.end() ? it->second : 0);
        }
        std::printf("\n");
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
    config.events = false;
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
// «day 9 14:00».
std::string dayTime(std::uint64_t tick) {
    const lw::CalendarTime time = lw::calendarAt(tick);
    char text[32];
    std::snprintf(text, sizeof(text), "day %d %02d:%02d", time.day, time.hour, time.minute);
    return text;
}

// Сценарий и фазы мира (этап 7.4): фаза, когда начиналась каждая, беда в цифрах.
void printScenario(const lw::World& world) {
    const lw::Scenario* scenario = lw::scenarioOf(world);
    if (scenario == nullptr) {
        std::printf("scenario: none (world without peacetime lives by survival rules)\n");
        return;
    }
    std::printf("scenario #%llu zombie virus: phase %s; began:",
                static_cast<unsigned long long>(lw::scenarioIdOf(world)),
                lw::phaseRules(scenario->phase).name);
    const char* separator = " ";
    for (std::size_t i = 0; i < lw::kWorldPhases; ++i) {
        if (scenario->began[i] != lw::Scenario::kNever) {
            std::printf("%s%s %s", separator, lw::phaseRules(static_cast<lw::WorldPhase>(i)).name,
                        dayTime(scenario->began[i]).c_str());
            separator = ", ";
        }
    }
    std::printf("\n");
    const lw::ScenarioStats stats = lw::scenarioStats(world);
    std::printf("  population %d, alive %d (infected %d, sick %d), lost %d, casualties %d; zombies %d "
                "(target %d, arrivals %d a day)",
                stats.population, stats.alive, stats.infected, stats.sick, stats.lost,
                stats.casualties(), stats.zombies, lw::zombieTarget(world), lw::arrivalsPerDay(world));
    if (const std::uint64_t deadline = lw::phaseDeadline(*scenario); deadline != lw::Scenario::kNever) {
        std::printf("; next phase by %s", dayTime(deadline).c_str());
        if (const int trigger = lw::phaseTrigger(*scenario); trigger > 0) {
            std::printf(" or at %d casualties", trigger);
        }
    }
    std::printf("\n");
    std::size_t left[lw::kWorldPhases] = {};
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind == lw::ChronicleKind::LeftWork) {
            ++left[std::min<std::size_t>(static_cast<std::size_t>(event.a), lw::kWorldPhases - 1)];
        }
    }
    std::printf("  left work: panic %zu, collapse %zu, survival %zu\n",
                left[static_cast<std::size_t>(lw::WorldPhase::Panic)],
                left[static_cast<std::size_t>(lw::WorldPhase::Collapse)],
                left[static_cast<std::size_t>(lw::WorldPhase::Survival)]);
}

// Вспышка (этап 7.5): нулевой пациент, сколько заражений каким путём, поколения
// «кто от кого» и сколько заразил в среднем каждый из поколения; с detailed —
// каждое заражение по порядку.
void printOutbreak(const lw::World& world, bool detailed) {
    using K = lw::ChronicleKind;
    const entt::registry& registry = world.registry();
    const lw::Scenario* scenario = lw::scenarioOf(world);
    constexpr auto kRoutes = static_cast<std::size_t>(lw::InfectionRoute::Count);
    std::size_t routes[kRoutes] = {};
    // Первое заражение каждого: поколение (нулевой пациент — 0; от зомби
    // издалека — -1, его потомки в поколения не входят), от кого, сколько
    // заразил сам.
    struct Case {
        int generation = -1;
        lw::EntityId from = lw::kInvalidId;
        int spread = 0;
    };
    std::unordered_map<lw::EntityId, Case> cases;
    std::vector<const lw::ChronicleEvent*> order;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind != K::PatientZero && event.kind != K::Infected && event.kind != K::Bitten) {
            continue;
        }
        if (cases.count(event.subject) != 0) {
            continue; // укушен ещё раз — не новое заражение
        }
        Case infection;
        lw::InfectionRoute route = lw::InfectionRoute::Origin;
        if (event.kind == K::PatientZero) {
            infection.generation = 0;
        } else {
            route = event.kind == K::Bitten ? lw::InfectionRoute::Bite
                                            : static_cast<lw::InfectionRoute>(event.a);
            infection.from = event.object;
            if (const auto it = cases.find(event.object); it != cases.end()) {
                ++it->second.spread;
                if (it->second.generation >= 0) {
                    infection.generation = it->second.generation + 1;
                }
            }
        }
        ++routes[static_cast<std::size_t>(route)];
        cases.emplace(event.subject, infection);
        order.push_back(&event);
    }
    if (scenario == nullptr && order.empty()) {
        return;
    }
    if (scenario != nullptr && scenario->patientZero != lw::kInvalidId) {
        std::printf("outbreak: patient zero %s, %s;", label(world, scenario->patientZero).c_str(),
                    dayTime(scenario->began[static_cast<std::size_t>(lw::WorldPhase::Outbreak)]).c_str());
    } else {
        std::printf("outbreak: no patient zero;");
    }
    std::printf(" infections %zu:", order.size());
    for (std::size_t i = 1; i < kRoutes; ++i) {
        if (routes[i] > 0) {
            std::printf(" %s %zu", lw::infectionRouteName(static_cast<lw::InfectionRoute>(i)), routes[i]);
        }
    }
    const lw::ScenarioStats stats = lw::scenarioStats(world);
    std::printf("; infected now %d (sick %d); turned %zu; zombies %d (residents %d)\n", stats.infected,
                stats.sick, world.chronicle().countOf(K::Turned), stats.zombies, stats.residents);
    // Поколения от нулевого пациента: сколько в каждом и сколько в среднем
    // заразил каждый из них.
    std::vector<int> size;
    std::vector<int> spread;
    for (const auto& [id, infection] : cases) {
        if (infection.generation < 0) {
            continue;
        }
        const auto g = static_cast<std::size_t>(infection.generation);
        if (size.size() <= g) {
            size.resize(g + 1, 0);
            spread.resize(g + 1, 0);
        }
        ++size[g];
        spread[g] += infection.spread;
    }
    if (!size.empty()) {
        std::printf("  generations (count/spread each):");
        for (std::size_t g = 0; g < size.size(); ++g) {
            std::printf(" %zu:%d/%.1f", g, size[g],
                        static_cast<double>(spread[g]) / static_cast<double>(std::max(size[g], 1)));
        }
        std::printf("\n");
    }
    // До скольких поселений дошло: где сейчас заразившиеся (живые, тела и
    // зомби, кто ещё в мире), — у какого поселения.
    std::vector<lw::EntityId> places;
    for (const lw::ChronicleEvent* event : order) {
        const entt::entity entity = world.find(event->subject);
        if (const auto* at = entity != entt::null ? registry.try_get<lw::Position>(entity) : nullptr) {
            places.push_back(world.places().nearestSettlement(at->x, at->y));
        }
    }
    std::sort(places.begin(), places.end());
    places.erase(std::unique(places.begin(), places.end()), places.end());
    std::printf("  infected are near %zu of %zu settlements\n", places.size(),
                registry.view<const lw::Settlement>().size());
    if (!detailed) {
        return;
    }
    std::printf("infections:\n");
    for (const lw::ChronicleEvent* event : order) {
        const Case& infection = cases.at(event->subject);
        if (event->kind == K::PatientZero) {
            std::printf("  %s  %s - patient zero\n", dayTime(event->tick).c_str(),
                        label(world, event->subject).c_str());
            continue;
        }
        const char* how = event->kind == K::Bitten
                              ? "bite"
                              : lw::infectionRouteName(static_cast<lw::InfectionRoute>(event->a));
        std::printf("  %s  %s <- %s (%s), gen %d\n", dayTime(event->tick).c_str(),
                    label(world, event->subject).c_str(), label(world, infection.from).c_str(), how,
                    infection.generation);
    }
}

// Реакция общества (этап 7.6): тревога по стадиям и как узнавали, передачи,
// карантины, эвакуация, солдаты, «к своим». detailed — журнал по порядку.
void printSociety(const lw::World& world, bool detailed) {
    using K = lw::ChronicleKind;
    const entt::registry& registry = world.registry();
    if (lw::scenarioOf(world) == nullptr) {
        return;
    }
    std::size_t stages[4] = {};
    for (const auto [entity, person, alarm] : registry.view<const lw::Person, const lw::Alarm>().each()) {
        ++stages[static_cast<std::size_t>(lw::alarmStage(alarm.level))];
    }
    constexpr auto kClues = static_cast<std::size_t>(lw::AlarmClue::Count);
    std::size_t clues[kClues] = {};
    std::size_t firstClue[kClues] = {};
    std::unordered_map<lw::EntityId, bool> seen;
    const lw::Chronicle& chronicle = world.chronicle();
    int fetched = 0;
    int movedIn = 0;
    int expelledSick = 0;
    int refused = 0;
    int soldiersLeft = 0;
    for (const lw::ChronicleEvent& event : chronicle.events()) {
        if (event.kind == K::Alarmed) {
            ++clues[static_cast<std::size_t>(event.b)];
            if (!seen[event.subject]) {
                seen[event.subject] = true;
                ++firstClue[static_cast<std::size_t>(event.b)];
            }
        } else if (event.kind == K::Reunited) {
            (event.b == 1 ? movedIn : fetched) += 1;
        } else if (event.kind == K::Expelled && event.a == 1) {
            ++expelledSick;
        } else if (event.kind == K::EvacuationRefused) {
            ++refused;
        } else if (event.kind == K::Evacuated && event.a == 1) {
            ++soldiersLeft;
        }
    }
    constexpr const char* kClueNames[] = {"none", "fever", "sick", "body", "bitten", "zombie", "told", "broadcast"};
    static_assert(std::size(kClueNames) == kClues);
    const lw::ScenarioStats stats = lw::scenarioStats(world);
    std::printf("society: alarm calm %zu, uneasy %zu, alarmed %zu, terrified %zu; first learned:", stages[0],
                stages[1], stages[2], stages[3]);
    for (std::size_t i = 1; i < kClues; ++i) {
        std::printf(" %s %zu%s", kClueNames[i], firstClue[i], i + 1 < kClues ? "," : "");
    }
    std::printf("; stage rises:");
    for (std::size_t i = 1; i < kClues; ++i) {
        std::printf(" %s %zu%s", kClueNames[i], clues[i], i + 1 < kClues ? "," : "\n");
    }
    std::size_t quarantined = registry.view<const lw::Quarantine>().size();
    std::printf("  quarantines declared %zu, lifted %zu, active %zu, broke cordon %zu; broadcasts %zu\n",
                chronicle.countOf(K::QuarantineDeclared), chronicle.countOf(K::QuarantineLifted), quarantined,
                chronicle.countOf(K::BrokeQuarantine), chronicle.countOf(K::Broadcast));
    std::printf("  evacuation points %zu, convoys with people %zu, evacuated %d (refused %d), soldiers alive %d, "
                "left with army %d, stayed %zu\n",
                chronicle.countOf(K::EvacuationOpened), chronicle.countOf(K::ConvoyLeft), stats.evacuated, refused,
                stats.soldiers, soldiersLeft, chronicle.countOf(K::StayedBehind));
    std::printf("  reunited: fetched child %d, moved in with kin %d; expelled infected from groups %d\n", fetched,
                movedIn, expelledSick);
    if (!detailed) {
        return;
    }
    std::printf("society log:\n");
    for (const lw::ChronicleEvent& event : chronicle.events()) {
        switch (event.kind) {
        case K::Broadcast:
        case K::QuarantineDeclared:
        case K::QuarantineLifted:
        case K::BrokeQuarantine:
        case K::EvacuationOpened:
        case K::Evacuated:
        case K::EvacuationRefused:
        case K::ConvoyLeft:
        case K::EvacuationEnded:
        case K::StayedBehind:
        case K::Reunited:
            break;
        case K::Alarmed:
            if (event.a < static_cast<int>(lw::AlarmStage::Alarmed)) {
                continue; // «странная болезнь» — слишком много строк
            }
            break;
        case K::Expelled:
            if (event.a != 1) {
                continue;
            }
            break;
        default:
            continue;
        }
        std::printf("  %s %-18s %s", dayTime(event.tick).c_str(), lw::chronicleKindName(event.kind),
                    label(world, event.subject).c_str());
        if (event.object != lw::kInvalidId) {
            std::printf(" -> %s", label(world, event.object).c_str());
        }
        if (event.kind == K::Alarmed) {
            std::printf(" (stage %d, %s)", event.a, kClueNames[static_cast<std::size_t>(event.b)]);
        } else {
            std::printf(" (a %d, b %d)", event.a, event.b);
        }
        if (event.third != lw::kInvalidId) {
            std::printf(" [%s]", label(world, event.third).c_str());
        }
        std::printf("\n");
    }
}

void printDailyHeader() {
    std::printf("day season phase | people (hungry thirsty) | zombies hordes | animals | died: wounds "
                "bleed starve thirst exhaust infect | turned killedZ hunted | food bld/carried "
                "forest | ammo | bought money | workers shops | infected sick zres | alarmed quar evac\n");
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
    long long money = 0;
    for (const auto [entity, person, wallet] : registry.view<const lw::Person, const lw::Wallet>().each()) {
        money += wallet.money;
    }
    // Держится ли порядок (этап 7.4): сколько людей ещё выходит на работу и в
    // скольких магазинах ещё работают.
    std::size_t onDuty = 0;
    for (const auto [entity, person, job] : registry.view<const lw::Person, const lw::Job>().each()) {
        onDuty += lw::keepsWorking(world, entity) ? 1 : 0;
    }
    std::size_t shops = 0;
    for (const lw::EntityId workplace : lw::mannedWorkplaces(world)) {
        const entt::entity building = world.find(workplace);
        shops += building != entt::null &&
                         registry.get<lw::Building>(building).kind == lw::BuildingKind::Shop
                     ? 1
                     : 0;
    }
    const lw::Chronicle& chronicle = world.chronicle();
    const lw::ScenarioStats stats = lw::scenarioStats(world);
    std::size_t alarmed = 0;
    for (const auto [entity, person, alarm] : registry.view<const lw::Person, const lw::Alarm>().each()) {
        alarmed += alarm.level >= lw::kAlarmedLevel ? 1 : 0;
    }
    std::printf("%3d %d %-8s | %3zu (%3zu %3zu) | %3zu %2zu | %3zu | %3zu %3zu %3zu %3zu %3zu %3zu | "
                "%3zu %4zu %3zu | %5lld/%3lld %5lld | %4lld | %5zu %4.0f | %3zu %3zu | %3d %3d %3d | "
                "%3zu %2zu %3d\n",
                time.day, static_cast<int>(time.season), lw::rulesOf(world).name, people, hungry,
                thirsty,
                registry.view<const lw::Zombie>().size(), registry.view<const lw::Horde>().size(),
                registry.view<const lw::Animal>().size(), died[0], died[1], died[2], died[3],
                died[4], died[5], chronicle.countOf(lw::ChronicleKind::Turned),
                chronicle.countOf(lw::ChronicleKind::KilledZombie),
                chronicle.countOf(lw::ChronicleKind::Hunted), food, carried,
                world.resources().forestTotal(), ammo, chronicle.countOf(lw::ChronicleKind::Bought),
                people > 0 ? static_cast<double>(money) / static_cast<double>(people) : 0.0, onDuty,
                shops, stats.infected, stats.sick, stats.residents, alarmed,
                registry.view<const lw::Quarantine>().size(), stats.evacuated);
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
    bool social = false;
    bool profile = false;
    bool eventLog = false;
    bool storyLog = false;
    bool foresightLog = false;
    bool castLog = false;
    bool familyLog = false;
    bool infectionLog = false;
    bool societyLog = false;
    // Ивент вручную: вид и через сколько игровых часов.
    std::optional<lw::EventKind> manualEvent;
    double manualHour = 0.0;
    // Фаза мира с самого начала (этап 7.4).
    std::optional<lw::WorldPhase> startPhase;
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
        } else if (std::strcmp(argv[i], "--social") == 0) {
            social = true;
        } else if (std::strcmp(argv[i], "--profile") == 0) {
            profile = true;
        } else if (std::strcmp(argv[i], "--events") == 0) {
            eventLog = true;
        } else if (std::strcmp(argv[i], "--stories") == 0) {
            storyLog = true;
        } else if (std::strcmp(argv[i], "--foresight") == 0) {
            foresightLog = true;
        } else if (std::strcmp(argv[i], "--families") == 0) {
            familyLog = true;
        } else if (std::strcmp(argv[i], "--infections") == 0) {
            infectionLog = true;
        } else if (std::strcmp(argv[i], "--society") == 0) {
            societyLog = true;
        } else if (std::strcmp(argv[i], "--cast") == 0) {
            castLog = true;
        } else if (std::strcmp(argv[i], "--event-schedule") == 0) {
            config.events = true;
        } else if (std::strcmp(argv[i], "--no-events") == 0) {
            config.phaseEvents = false;
        } else if (std::strcmp(argv[i], "--no-peace") == 0) {
            config.peacetime = false;
        } else if (std::strcmp(argv[i], "--outbreak") == 0 && hasValue) {
            config.outbreakDays = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--phase") == 0 && hasValue && lw::phaseByName(argv[i + 1])) {
            startPhase = lw::phaseByName(argv[++i]);
        } else if (std::strcmp(argv[i], "--event") == 0 && i + 2 < argc &&
                   lw::eventKindByName(argv[i + 1])) {
            manualEvent = lw::eventKindByName(argv[++i]);
            manualHour = std::strtod(argv[++i], nullptr);
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
                        "[--zombies N] [--animals N] [--flocks N] [--paths N [--optimal]] [--daily] "
                        "[--social] [--events] [--event-schedule] [--no-events] "
                        "[--event drop|crash|convoy HOURS] [--no-peace] "
                        "[--outbreak DAYS] [--phase peace|outbreak|panic|collapse|survival] "
                        "[--stories] [--foresight] [--cast] [--families] [--infections] [--society] "
                        "[--profile]\n");
            return 2;
        }
    }

    if (climate) {
        printClimate(seed);
        return 0;
    }

    const auto genStart = std::chrono::steady_clock::now();
    lw::World world(seed, config);
    if (startPhase) {
        lw::enterPhase(world, *startPhase);
    }
    const std::chrono::duration<double> genElapsed = std::chrono::steady_clock::now() - genStart;
    printMapSummary(world, genElapsed.count());
    if (paths > 0) {
        benchmarkPaths(world, paths, compare);
    }

    world.setProfiling(profile);
    const auto start = std::chrono::steady_clock::now();
    if (daily) {
        printDailyHeader();
    }
    ForesightLog foresight;
    const auto manualTick =
        static_cast<std::uint64_t>(std::max(0.0, manualHour) * lw::kTicksPerGameHour);
    for (std::uint64_t i = 0; i < ticks; ++i) {
        if (manualEvent && world.tick() == manualTick) {
            const lw::EntityId id = lw::startEvent(world, *manualEvent);
            std::printf("event #%llu started at tick %llu\n", static_cast<unsigned long long>(id),
                        static_cast<unsigned long long>(world.tick()));
        }
        world.step();
        if (foresightLog && world.tick() % lw::kTicksPerGameMinute == 0) {
            foresight.sample(world);
        }
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
    printScenario(world);
    printOutbreak(world, infectionLog);
    printSociety(world, societyLog);
    printPeople(world);
    printWork(world);
    printRelations(world);
    printFamilies(world, familyLog);
    printMemories(world);
    printGroups(world);
    printPerception(world);
    printZombies(world);
    printAnimals(world);
    printCombat(world);
    printEvents(world);
    printChronicle(world);
    if (social) {
        std::printf("social events:\n");
        printSocialLog(world);
    }
    if (eventLog) {
        std::printf("event log:\n");
        printEventLog(world);
    }
    if (storyLog) {
        printStories(world);
    }
    if (foresightLog) {
        foresight.print(world);
    }
    if (castLog) {
        printCast(world);
    }
    if (elapsed.count() > 0.0) {
        std::printf("%.3f s, %.0f ticks/s\n", elapsed.count(),
                    static_cast<double>(ticks) / elapsed.count());
    }
    if (profile && elapsed.count() > 0.0) {
        std::printf("profile (share of tick time):\n");
        for (int phase = 0; phase < lw::World::kStepPhases; ++phase) {
            const double seconds = world.stepPhaseSeconds(phase);
            std::printf("  %-13s %7.3f s  %5.1f %%\n", lw::World::stepPhaseName(phase), seconds,
                        100.0 * seconds / elapsed.count());
        }
    }
    return 0;
}
