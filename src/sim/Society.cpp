#include "sim/Society.h"

#include "sim/Calendar.h"
#include "sim/Infection.h"
#include "sim/Scenario.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Уровни тревоги от увиденного своими глазами. Сам слёг — не знает чем, это
// просто болезнь; больной в жару рядом — странная болезнь; тело — от неё
// умирают; укушенный — больные нападают; зомби — мёртвые встают.
constexpr float kFeverLevel = 0.3f;
constexpr float kSickLevel = 0.3f;
constexpr float kBodyLevel = 0.45f;
constexpr float kBittenLevel = 0.7f;
constexpr float kZombieLevel = 0.95f;
// Официальные передачи: эпидемия, нападения и эвакуация, последняя передача.
constexpr float kEpidemicLevel = 0.4f;
constexpr float kEvacuationLevel = 0.7f;
constexpr float kFarewellLevel = 0.85f;
// Рассказывают, только если слушатель поверит хоть на столько больше, чем знал.
constexpr float kShareMargin = 0.05f;
// Знакомый — с таким знакомством; друг — по отметке связи.
constexpr float kKnownFamiliarity = 0.3f;

bool sameGroupOf(const entt::registry& registry, entt::entity a, entt::entity b) {
    const auto* first = registry.try_get<Person>(a);
    const auto* second = registry.try_get<Person>(b);
    return first != nullptr && second != nullptr && first->group != kInvalidId &&
           first->group == second->group;
}

float metersBetween(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

} // namespace

AlarmStage alarmStage(float level) {
    if (level >= kTerrifiedLevel) {
        return AlarmStage::Terrified;
    }
    if (level >= kAlarmedLevel) {
        return AlarmStage::Alarmed;
    }
    if (level >= kUneasyLevel) {
        return AlarmStage::Uneasy;
    }
    return AlarmStage::Calm;
}

float alarmOf(const entt::registry& registry, entt::entity entity) {
    const auto* alarm = registry.try_get<Alarm>(entity);
    return alarm != nullptr ? alarm->level : 0.0f;
}

float alarmWorry(float level) {
    return std::clamp((level - kUneasyLevel) / (1.0f - kUneasyLevel), 0.0f, 1.0f);
}

float clueLevel(AlarmClue clue) {
    switch (clue) {
    case AlarmClue::Fever:
        return kFeverLevel;
    case AlarmClue::Sick:
        return kSickLevel;
    case AlarmClue::Body:
        return kBodyLevel;
    case AlarmClue::Bitten:
        return kBittenLevel;
    case AlarmClue::Zombie:
        return kZombieLevel;
    default:
        return 0.0f;
    }
}

bool raiseAlarm(World& world, entt::entity person, float level, AlarmClue clue, EntityId source) {
    auto* alarm = world.registry().try_get<Alarm>(person);
    level = std::min(level, 1.0f);
    if (alarm == nullptr || level <= alarm->level + 1e-4f) {
        return false;
    }
    const AlarmStage before = alarmStage(alarm->level);
    alarm->level = level;
    alarm->clue = clue;
    alarm->source = source;
    const AlarmStage after = alarmStage(level);
    if (alarm->since == 0 && after != AlarmStage::Calm) {
        alarm->since = world.tick();
    }
    if (after > before) {
        world.record(ChronicleKind::Alarmed, world.registry().get<Id>(person).value, source,
                     static_cast<std::int32_t>(after), static_cast<std::int32_t>(clue));
    }
    return true;
}

float credibility(const World& world, entt::entity listener, EntityId teller) {
    const entt::registry& registry = world.registry();
    const entt::entity other = world.find(teller);
    const auto* relations = registry.try_get<Relations>(listener);
    const Bond* bond = relations != nullptr ? relations->find(teller) : nullptr;
    if ((bond != nullptr && (bond->origin == BondOrigin::Kin || bond->friendly())) ||
        (other != entt::null && sameGroupOf(registry, listener, other))) {
        return kCloseCredibility;
    }
    if (bond != nullptr && bond->hostile()) {
        return kStrangerCredibility; // недругу не верят больше, чем чужому
    }
    return bond != nullptr && bond->familiarity >= kKnownFamiliarity ? kKnownCredibility
                                                                     : kStrangerCredibility;
}

float alarmToShare(const World& world, entt::entity teller, entt::entity listener) {
    const entt::registry& registry = world.registry();
    const float told = alarmOf(registry, teller);
    if (told < kUneasyLevel || !registry.all_of<Alarm>(listener)) {
        return 0.0f;
    }
    const float believed = told * credibility(world, listener, registry.get<Id>(teller).value);
    return believed > alarmOf(registry, listener) + kShareMargin ? believed : 0.0f;
}

float broadcastLevel(BroadcastKind kind) {
    switch (kind) {
    case BroadcastKind::Epidemic:
        return kEpidemicLevel;
    case BroadcastKind::Evacuation:
        return kEvacuationLevel;
    case BroadcastKind::Farewell:
        return kFarewellLevel;
    default:
        return 0.0f;
    }
}

void broadcast(World& world, BroadcastKind kind, EntityId settlement) {
    const EntityId id = scenarioIdOf(world);
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return;
    }
    Scenario& scenario = world.registry().get<Scenario>(entity);
    if (scenario.broadcast >= static_cast<std::uint8_t>(kind)) {
        return;
    }
    scenario.broadcast = static_cast<std::uint8_t>(kind);
    scenario.broadcastAt = world.tick();
    world.record(ChronicleKind::Broadcast, id, settlement, static_cast<std::int32_t>(kind));
}

bool authorityPresent(const World& world) {
    const WorldPhase phase = worldPhase(world);
    if (scenarioOf(world) == nullptr || (phase != WorldPhase::Outbreak && phase != WorldPhase::Panic)) {
        return false;
    }
    const entt::registry& registry = world.registry();
    for (const EntityId workplace : mannedWorkplaces(world)) {
        const entt::entity building = world.find(workplace);
        if (building == entt::null) {
            continue;
        }
        const BuildingKind kind = registry.get<Building>(building).kind;
        if (kind == BuildingKind::PoliceStation || kind == BuildingKind::Hospital) {
            return true;
        }
    }
    return false;
}

EntityId quarantineAt(const World& world, float x, float y) {
    const entt::registry& registry = world.registry();
    EntityId found = kInvalidId;
    for (const auto [entity, id, settlement, quarantine] :
         registry.view<const Id, const Settlement, const Quarantine>().each()) {
        const float r = static_cast<float>(settlement.radius) * Map::kCellSize + kCordonMargin;
        const float d = metersBetween(x, y, Map::cellCenter(settlement.center.x),
                                      Map::cellCenter(settlement.center.y));
        if (d <= r && (found == kInvalidId || id.value < found)) {
            found = id.value;
        }
    }
    return found;
}

int quarantineCases(const World& world, EntityId settlementId) {
    const entt::registry& registry = world.registry();
    const entt::entity place = world.find(settlementId);
    if (place == entt::null) {
        return 0;
    }
    const Settlement& settlement = registry.get<Settlement>(place);
    const float cx = Map::cellCenter(settlement.center.x);
    const float cy = Map::cellCenter(settlement.center.y);
    const float r = static_cast<float>(settlement.radius) * Map::kCellSize;
    int cases = 0;
    for (const auto [entity, self, wounds] : registry.view<const Person, const Wounds>().each()) {
        if (!isVisiblyIll(wounds)) {
            continue;
        }
        const entt::entity home = world.find(self.home);
        if (home != entt::null && registry.get<Building>(home).settlement == settlementId) {
            ++cases;
        }
    }
    for (const auto [entity, zombie, position] : registry.view<const Zombie, const Position>().each()) {
        cases += metersBetween(position.x, position.y, cx, cy) <= r ? 1 : 0;
    }
    for (const auto [entity, corpse, position] : registry.view<const Corpse, const Position>().each()) {
        if (corpse.kind == CorpseKind::Human && corpse.turnsAt > world.tick() &&
            metersBetween(position.x, position.y, cx, cy) <= r) {
            ++cases;
        }
    }
    return cases;
}

std::vector<Cell> cordonExits(const World& world, EntityId settlementId) {
    std::vector<Cell> exits;
    const entt::entity entity = world.find(settlementId);
    if (entity == entt::null) {
        return exits;
    }
    const Settlement& settlement = world.registry().get<Settlement>(entity);
    const Map& map = world.map();
    // Кольцо клеток на краю кордона: дороги, по которым выходят из поселения.
    const int inner = settlement.radius + 1;
    const int outer = settlement.radius + 1 + static_cast<int>(kCordonMargin / Map::kCellSize);
    for (int dy = -outer; dy <= outer; ++dy) {
        for (int dx = -outer; dx <= outer; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 < inner * inner || d2 > outer * outer) {
                continue;
            }
            const Cell cell{settlement.center.x + dx, settlement.center.y + dy};
            if (map.contains(cell) && map.road.at(cell.x, cell.y) != Road::None &&
                world.nav().passable(cell)) {
                exits.push_back(cell);
            }
        }
    }
    std::sort(exits.begin(), exits.end(),
              [](Cell a, Cell b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    return exits;
}

std::optional<EvacuationSite> nearestEvacuation(const World& world, float x, float y, float range) {
    const entt::registry& registry = world.registry();
    std::optional<EvacuationSite> best;
    for (const auto [entity, id, building, point] :
         registry.view<const Id, const Building, const EvacuationPoint>().each()) {
        const float d = metersBetween(x, y, Map::cellCenter(building.door.x), Map::cellCenter(building.door.y));
        if (d > range) {
            continue;
        }
        if (!best || d < best->distance || (d == best->distance && id.value < best->id)) {
            best = EvacuationSite{id.value, building.door, d};
        }
    }
    return best;
}

bool evacuationOpen(const World& world) {
    return !world.registry().view<const EvacuationPoint>().empty();
}

std::uint64_t nextConvoyTick(std::uint64_t tick) {
    const CalendarTime time = calendarAt(tick);
    const std::uint64_t dayStart = time.gameTick - time.gameTick % kTicksPerGameDay;
    for (int day = 0; day < 2; ++day) {
        for (const int hour : kConvoyHours) {
            const std::uint64_t at = dayStart + static_cast<std::uint64_t>(day) * kTicksPerGameDay +
                                     static_cast<std::uint64_t>(hour) * kTicksPerGameHour;
            if (at > time.gameTick) {
                return at - kStartGameTick;
            }
        }
    }
    return tick + kTicksPerGameDay;
}

bool isSoldier(const entt::registry& registry, entt::entity entity) {
    const auto* job = registry.try_get<Job>(entity);
    return job != nullptr && job->profession == Profession::Soldier;
}

float soldierDiscipline(const Traits& traits) {
    return std::clamp(0.3f + 0.4f * traits.bravery + 0.3f * traits.leadership, 0.0f, 1.0f);
}

void evacuate(World& world, entt::entity entity, EntityId point, bool soldier) {
    entt::registry& registry = world.registry();
    if (!registry.valid(entity) || !registry.all_of<Person>(entity)) {
        return;
    }
    const EntityId id = registry.get<Id>(entity).value;
    world.record(ChronicleKind::Evacuated, id, point, soldier ? 1 : 0);
    if (const entt::entity building = world.find(point); building != entt::null) {
        if (auto* site = registry.try_get<EvacuationPoint>(building); site != nullptr && !soldier) {
            ++site->evacuated;
        }
    }
    // Уехал со всем, что при себе; в мире его больше нет. Имя, возраст, родня,
    // работа и черты остаются — это его прошлое.
    registry.remove<Person, Mind, Needs, Wounds, Walker, Senses, Awareness, Combat>(entity);
    registry.remove<Relations, Memories, Leads, Alarm, Inventory, Weapon, Wallet>(entity);
    registry.remove<Velocity, Position>(entity);
    registry.emplace<Evacuated>(entity, Evacuated{world.tick(), point});
}

} // namespace lw
