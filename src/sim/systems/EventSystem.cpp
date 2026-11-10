#include "sim/systems/EventSystem.h"

#include "sim/Calendar.h"
#include "sim/Death.h"
#include "sim/Events.h"
#include "sim/Infection.h"
#include "sim/Perception.h"
#include "sim/Scenario.h"
#include "sim/Weapons.h"
#include "sim/World.h"
#include "sim/systems/SocietySystem.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Место падения припасов: от края поселения, метры.
constexpr float kDropFromEdgeMin = 300.0f;
constexpr float kDropFromEdgeMax = 1500.0f;
// Сколько случайных клеток пробовать; отступ от края карты, клетки.
constexpr int kSiteAttempts = 400;
constexpr int kSiteMargin = 16;
// Ивент у заданной точки (интерфейс): ближайшая проходимая клетка не дальше, клетки.
constexpr int kNearSearch = 64;
// Второй и следующие грузы — не дальше стольких клеток от места (ветер сносит парашют).
constexpr int kScatterCells = 10;
// Этап 7.7. Гуманитарный груз в панике и коллапсе: с такой вероятностью — к
// поселению на карантине (если такие есть), в 150–800 м от его края.
constexpr float kQuarantineDropChance = 0.6f;
constexpr float kAidFromEdgeMin = 150.0f;
constexpr float kAidFromEdgeMax = 800.0f;
// Вертолёт падает где придётся, но не в поселение: 500–4000 м от края.
constexpr float kCrashFromEdgeMin = 500.0f;
constexpr float kCrashFromEdgeMax = 4000.0f;
// Колонна встаёт на загородной дороге: 300–2500 м от края поселения.
constexpr float kConvoyFromEdgeMin = 300.0f;
constexpr float kConvoyFromEdgeMax = 2500.0f;
// Грузовики колонны — вдоль дороги, не дальше стольких клеток от места.
constexpr int kConvoySpread = 6;
// Экипаж — не дальше стольких клеток от места, зомби колонны — стольких.
constexpr int kCrewSpread = 3;
constexpr int kZombieSpread = 8;
// Выживший ранен: кровотечение (здоровья в час) и здоровье.
constexpr float kSurvivorBleeding = 0.2f;
constexpr float kSurvivorHealthMin = 0.45f;
constexpr float kSurvivorHealthMax = 0.8f;

float metersBetween(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

// Открытая суша: не вода, не лес, не дорога, проходимо (не здание).
bool openGround(const World& world, Cell cell) {
    const Map& map = world.map();
    return map.contains(cell) && map.water.at(cell.x, cell.y) == Water::None &&
           map.cover.at(cell.x, cell.y) == Cover::None &&
           map.road.at(cell.x, cell.y) == Road::None && world.nav().passable(cell);
}

// Люди по возрастанию id: рандом и летопись — в порядке, не зависящем от реестра.
std::vector<std::pair<EntityId, entt::entity>> peopleById(const entt::registry& registry) {
    std::vector<std::pair<EntityId, entt::entity>> people;
    for (const auto [entity, id, person, leads] :
         registry.view<const Id, const Person, const Leads>().each()) {
        people.emplace_back(id.value, entity);
    }
    std::sort(people.begin(), people.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return people;
}

// Кто услышал шум ивента — узнал о нём (спящий слышит хуже).
void hearEvent(World& world, EntityId event, const WorldEvent& info, const EventSpec& spec) {
    const entt::registry& registry = world.registry();
    const SenseConditions conditions = senseConditions(world);
    const float x = Map::cellCenter(info.site.x);
    const float y = Map::cellCenter(info.site.y);
    for (const auto& [id, entity] : peopleById(registry)) {
        const auto* senses = registry.try_get<Senses>(entity);
        if (senses == nullptr) {
            continue;
        }
        const Position& position = registry.get<Position>(entity);
        const float range =
            spec.heardRange * hearingFactor(*senses, isAsleep(registry, entity), conditions);
        if (metersBetween(position.x, position.y, x, y) <= range) {
            learnOf(world, entity, event, info.site, EventClue::Heard);
        }
    }
}

// Кто заметил знак: не спит, знак в пределах видимости (ночью, в туман и
// дождь — ближе). Лес и стены дыму не помеха: он поднимается высоко. Из дома
// видно только в окно: шанс меньше в kIndoorsNotice раз.
void noticeSignal(World& world, EntityId event, const WorldEvent& info, float sight, Rng& rng) {
    const entt::registry& registry = world.registry();
    const float range = eventSpec(info.kind).signalRange * sight;
    const float x = Map::cellCenter(info.site.x);
    const float y = Map::cellCenter(info.site.y);
    for (const auto& [id, entity] : peopleById(registry)) {
        const Person& person = registry.get<Person>(entity);
        if (person.asleep || registry.get<Leads>(entity).find(event) != nullptr) {
            continue;
        }
        const Position& position = registry.get<Position>(entity);
        const float chance = kNoticeChance * (person.indoors ? kIndoorsNotice : 1.0f);
        if (metersBetween(position.x, position.y, x, y) <= range && rng.chance(chance)) {
            learnOf(world, entity, event, info.site, EventClue::Saw);
        }
    }
}

// Расстояние от точки до края ближайшего поселения, метры (внутри — меньше
// нуля); поселений нет — nullopt.
std::optional<float> fromSettlementEdge(const World& world, float x, float y) {
    const entt::entity nearest = world.find(world.places().nearestSettlement(x, y));
    if (nearest == entt::null) {
        return std::nullopt;
    }
    const Settlement& settlement = world.registry().get<Settlement>(nearest);
    const float edge = static_cast<float>(settlement.radius) * Map::kCellSize;
    return metersBetween(x, y, Map::cellCenter(settlement.center.x), Map::cellCenter(settlement.center.y)) -
           edge;
}

// Куда упал вертолёт: суша без воды, дорог и зданий; лес — можно.
bool crashGround(const World& world, Cell cell) {
    const Map& map = world.map();
    return map.contains(cell) && map.water.at(cell.x, cell.y) == Water::None &&
           map.road.at(cell.x, cell.y) == Road::None && world.nav().passable(cell);
}

// Где встала колонна: проходимая дорога.
bool roadGround(const World& world, Cell cell) {
    const Map& map = world.map();
    return map.contains(cell) && map.road.at(cell.x, cell.y) != Road::None &&
           map.water.at(cell.x, cell.y) == Water::None && world.nav().passable(cell);
}

// Гуманитарный груз к поселению на карантине (этап 7.7): одно из таких
// поселений наугад, точка в кольце от его края. nullopt — таких нет или
// точки не нашлось.
std::optional<Cell> aidSite(const World& world, Rng& rng, int margin) {
    const entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> closed;
    for (const auto [entity, id, settlement, quarantine] :
         registry.view<const Id, const Settlement, const Quarantine>().each()) {
        closed.emplace_back(id.value, entity);
    }
    if (closed.empty() || !rng.chance(kQuarantineDropChance)) {
        return std::nullopt;
    }
    std::sort(closed.begin(), closed.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const Settlement& target =
        registry.get<Settlement>(closed[rng.below(static_cast<std::uint32_t>(closed.size()))].second);
    const Map& map = world.map();
    const float edge = static_cast<float>(target.radius) * Map::kCellSize;
    for (int attempt = 0; attempt < kSiteAttempts / 4; ++attempt) {
        const float angle = rng.range(0.0f, 6.2831853f);
        const float d = edge + rng.range(kAidFromEdgeMin, kAidFromEdgeMax);
        const Cell cell = map.clampedCellAt(Map::cellCenter(target.center.x) + std::cos(angle) * d,
                                            Map::cellCenter(target.center.y) + std::sin(angle) * d);
        if (cell.x >= margin && cell.y >= margin && cell.x < map.width() - margin &&
            cell.y < map.height() - margin && openGround(world, cell)) {
            return cell;
        }
    }
    return std::nullopt;
}

// Где поселится выживший из экипажа: здание ближайшего поселения, дверь
// которого ближе всего к месту ивента (жилое — охотнее: в магазине не
// живут). kInvalidId — поселений нет.
EntityId shelterNear(const World& world, Cell site) {
    const float x = Map::cellCenter(site.x);
    const float y = Map::cellCenter(site.y);
    const Places::SettlementPlace* place = world.places().settlement(world.places().nearestSettlement(x, y));
    if (place == nullptr) {
        return kInvalidId;
    }
    EntityId best = kInvalidId;
    float bestScore = 0.0f;
    for (const Places::BuildingPlace& building : place->buildings) {
        const bool home = building.kind == BuildingKind::House || building.kind == BuildingKind::Apartment;
        const float score =
            metersBetween(x, y, Map::cellCenter(building.door.x), Map::cellCenter(building.door.y)) *
            (home ? 1.0f : 2.0f);
        if (best == kInvalidId || score < bestScore) {
            best = building.id;
            bestScore = score;
        }
    }
    return best;
}

// Клетка рядом с местом: случайный сдвиг до spread и ближайшая проходимая.
Cell nearSite(const World& world, Cell site, int spread, Rng& rng) {
    const Cell around{site.x + rng.rangeInt(-spread, spread), site.y + rng.rangeInt(-spread, spread)};
    const auto spot = world.nav().nearestPassable(around, spread);
    return spot ? *spot : site;
}

// Экипаж ивента (этап 7.7): солдаты с именами. Выживший ранен, живёт в
// ближайшем здании (без смен: служить уже негде) и знает о месте ивента;
// погибший — тело с оружием и пайком, укушенный встанет зомби. Без поселений
// рядом не выживает никто.
void spawnCrew(World& world, EntityId event, const WorldEvent& info, const EventSpec& spec, Rng& rng) {
    entt::registry& registry = world.registry();
    const EntityId shelter = shelterNear(world, info.site);
    std::unordered_set<std::string> taken = takenNames(world);
    const int crew = rng.rangeInt(spec.crewMin, spec.crewMax);
    for (int i = 0; i < crew; ++i) {
        const Cell at = nearSite(world, info.site, kCrewSpread, rng);
        const bool survives = rng.chance(spec.crewSurvive) && shelter != kInvalidId;
        const bool bitten = rng.chance(spec.crewBitten);
        const entt::entity soldier = spawnSoldier(world, survives ? shelter : kInvalidId, at, rng, taken);
        const EntityId id = registry.get<Id>(soldier).value;
        Job& job = registry.get<Job>(soldier);
        job.workplace = survives ? shelter : kInvalidId;
        job.days = 0;
        Wounds& wounds = registry.get<Wounds>(soldier);
        if (bitten) {
            wounds.bites = 1;
            wounds.hurtAt = world.tick();
            infect(world, soldier, InfectionRoute::Bite, kInvalidId);
        }
        world.record(ChronicleKind::EventCrew, id, event, survives ? 1 : 0, bitten ? 1 : 0);
        if (survives) {
            wounds.bleeding += kSurvivorBleeding + (bitten ? kBiteBleeding : 0.0f);
            wounds.hurtAt = world.tick();
            registry.get<Needs>(soldier).health = rng.range(kSurvivorHealthMin, kSurvivorHealthMax);
            learnOf(world, soldier, event, info.site, EventClue::Saw);
        } else {
            die(world, soldier,
                info.kind == EventKind::HelicopterCrash ? DeathCause::Crash : DeathCause::Wounds, kInvalidId);
        }
    }
}

bool hasLoot(const World& world, EntityId cache) {
    const entt::entity entity = world.find(cache);
    const auto* inventory = entity != entt::null ? world.registry().try_get<Inventory>(entity) : nullptr;
    return inventory != nullptr && !inventory->empty();
}

} // namespace

std::optional<Cell> pickEventSite(const World& world, EventKind kind, Rng& rng) {
    const Map& map = world.map();
    const int margin = std::min(kSiteMargin, std::min(map.width(), map.height()) / 4);
    const auto randomCell = [&] {
        return Cell{rng.rangeInt(margin, map.width() - 1 - margin),
                    rng.rangeInt(margin, map.height() - 1 - margin)};
    };
    // Подходит ли земля виду ивента: груз — на открытое место, вертолёт — куда
    // придётся (и в лес), колонна — на дорогу.
    const auto ground = [&](Cell cell) {
        switch (kind) {
        case EventKind::HelicopterCrash:
            return crashGround(world, cell);
        case EventKind::MilitaryConvoy:
            return roadGround(world, cell);
        default:
            return openGround(world, cell);
        }
    };
    // И расстояние до поселения: припасы сбрасывают недалеко от людей, но не
    // на дома; вертолёт — подальше; колонна — на загородной дороге.
    const auto suits = [&](Cell cell) {
        if (!ground(cell)) {
            return false;
        }
        const std::optional<float> d =
            fromSettlementEdge(world, Map::cellCenter(cell.x), Map::cellCenter(cell.y));
        if (!d) {
            return true;
        }
        switch (kind) {
        case EventKind::SupplyDrop:
            return *d >= kDropFromEdgeMin && *d <= kDropFromEdgeMax;
        case EventKind::HelicopterCrash:
            return *d >= kCrashFromEdgeMin && *d <= kCrashFromEdgeMax;
        case EventKind::MilitaryConvoy:
            return *d >= kConvoyFromEdgeMin && *d <= kConvoyFromEdgeMax;
        case EventKind::Count:
            break;
        }
        return true;
    };
    // Гуманитарный груз — к поселению на карантине (этап 7.7).
    if (kind == EventKind::SupplyDrop) {
        if (const std::optional<Cell> aid = aidSite(world, rng, margin)) {
            return aid;
        }
    }
    // Военная колонна идёт большой дорогой (этап 2а.3): место — на оси шоссе,
    // а без шоссе — трассы, за городом.
    if (kind == EventKind::MilitaryConvoy) {
        for (const RoadClass major : {RoadClass::Highway, RoadClass::Regional}) {
            std::vector<const RoadLink*> links;
            for (const RoadLink& link : world.roads().links) {
                if (link.kind == major && !link.path.empty()) {
                    links.push_back(&link);
                }
            }
            for (int attempt = 0; !links.empty() && attempt < kSiteAttempts; ++attempt) {
                const RoadLink& link = *links[rng.below(static_cast<std::uint32_t>(links.size()))];
                const Cell axis = link.path[rng.below(static_cast<std::uint32_t>(link.path.size()))];
                const Cell cell = Map::cellAt(link.axisX(axis), link.axisY(axis));
                if (suits(cell)) {
                    return cell;
                }
            }
        }
    }
    for (int attempt = 0; attempt < kSiteAttempts; ++attempt) {
        if (const Cell cell = randomCell(); suits(cell)) {
            return cell;
        }
    }
    // Подходящего не нашлось (маленькая карта) — любое подходящее место.
    for (int attempt = 0; attempt < kSiteAttempts; ++attempt) {
        if (const Cell cell = randomCell(); ground(cell)) {
            return cell;
        }
    }
    return std::nullopt;
}

EntityId startEvent(World& world, EventKind kind, std::optional<Cell> near) {
    entt::registry& registry = world.registry();
    Rng& rng = world.rng(RngStream::Events);
    const std::optional<Cell> site = near ? world.nav().nearestPassable(*near, kNearSearch)
                                          : pickEventSite(world, kind, rng);
    if (!site) {
        return kInvalidId;
    }
    const EventSpec& spec = eventSpec(kind);
    const std::uint64_t tick = world.tick();
    const float x = Map::cellCenter(site->x);
    const float y = Map::cellCenter(site->y);

    // Сначала ивент, потом грузы: id грузов больше id ивента.
    const entt::entity entity = world.create();
    const EntityId id = registry.get<Id>(entity).value;
    WorldEvent event;
    event.kind = kind;
    event.site = *site;
    event.began = tick;
    event.signalUntil = tick + static_cast<std::uint64_t>(spec.signalMinutes) * kTicksPerGameMinute;
    event.settlement = world.places().nearestSettlement(x, y);
    const int caches = rng.rangeInt(spec.cachesMin, spec.cachesMax);
    for (int i = 0; i < caches; ++i) {
        Cell cell = *site;
        if (i > 0 && kind == EventKind::MilitaryConvoy) {
            // Грузовики колонны — друг за другом по дороге.
            for (int attempt = 0; attempt < 16; ++attempt) {
                const Cell around{site->x + rng.rangeInt(-kConvoySpread, kConvoySpread),
                                  site->y + rng.rangeInt(-kConvoySpread, kConvoySpread)};
                if (roadGround(world, around)) {
                    cell = around;
                    break;
                }
            }
        } else if (i > 0) {
            const Cell around{site->x + rng.rangeInt(-kScatterCells, kScatterCells),
                              site->y + rng.rangeInt(-kScatterCells, kScatterCells)};
            if (const auto spot = world.nav().nearestPassable(around, kScatterCells)) {
                cell = *spot;
            }
        }
        const entt::entity crate = world.create();
        registry.emplace<Position>(crate, Map::cellCenter(cell.x), Map::cellCenter(cell.y));
        registry.emplace<Cache>(crate, id);
        Inventory& inventory = registry.emplace<Inventory>(crate);
        fillCache(inventory, kind, rng);
        event.caches.push_back(registry.get<Id>(crate).value);
    }
    const WorldEvent& info = registry.emplace<WorldEvent>(entity, std::move(event));
    world.record(ChronicleKind::EventBegan, id, info.settlement, static_cast<std::int32_t>(kind),
                 caches);
    // Экипаж и зомби у места (этап 7.7).
    if (spec.crewMax > 0) {
        spawnCrew(world, id, info, spec, rng);
    }
    const int zombies = spec.zombiesMax > 0 ? rng.rangeInt(spec.zombiesMin, spec.zombiesMax) : 0;
    for (int i = 0; i < zombies; ++i) {
        createZombie(world, nearSite(world, *site, kZombieSpread, rng), rng);
    }
    // Позиции остальных с начала тика не менялись: индекс узнаёт только о
    // новых — грузах, экипаже и зомби.
    world.syncSpatial();
    if (spec.sound != SoundKind::None) {
        world.emitSound(x, y, spec.loudness, spec.sound, id);
        hearEvent(world, id, info, spec);
    }
    return id;
}

void updateEvents(World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return;
    }
    entt::registry& registry = world.registry();
    Rng& rng = world.rng(RngStream::Events);

    // Временное расписание (до рассказчика): днём раз в час с малой вероятностью.
    const CalendarTime time = calendarAt(tick);
    if (world.config().events && time.minute == 0 && time.hour >= kEventFirstHour &&
        time.hour <= kEventLastHour) {
        const float chance =
            kEventsPerDay / static_cast<float>(kEventLastHour - kEventFirstHour + 1);
        if (rng.chance(chance)) {
            startEvent(world, EventKind::SupplyDrop);
        }
    }
    // Ивенты по фазам сценария (этап 7.7): днём раз в час, каждый вид — с
    // частотой фазы (PhaseRules::eventsPerDay).
    if (world.config().phaseEvents && scenarioOf(world) != nullptr && time.minute == 0 &&
        time.hour >= kEventFirstHour && time.hour <= kEventLastHour) {
        const PhaseRules& rules = rulesOf(world);
        for (std::size_t i = 0; i < kEventKinds; ++i) {
            const float perHour =
                rules.eventsPerDay[i] / static_cast<float>(kEventLastHour - kEventFirstHour + 1);
            if (perHour > 0.0f && rng.chance(perHour)) {
                startEvent(world, static_cast<EventKind>(i));
            }
        }
    }

    std::vector<std::pair<EntityId, entt::entity>> events;
    for (const auto [entity, id, event] : registry.view<const Id, const WorldEvent>().each()) {
        events.emplace_back(id.value, entity);
    }
    if (!events.empty()) {
        std::sort(events.begin(), events.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const float sight = senseConditions(world).sight;
        for (const auto& [id, entity] : events) {
            const WorldEvent& event = registry.get<WorldEvent>(entity);
            if (tick < event.signalUntil) {
                noticeSignal(world, id, event, sight, rng);
            }
        }
        // Знак погас, грузы пусты (или ивент слишком стар) — ивент кончился.
        // Пустые грузы исчезают с ним, непустые остаются в мире.
        for (const auto& [id, entity] : events) {
            const WorldEvent& event = registry.get<WorldEvent>(entity);
            const int loaded = static_cast<int>(
                std::count_if(event.caches.begin(), event.caches.end(),
                              [&](EntityId cache) { return hasLoot(world, cache); }));
            if (tick < event.signalUntil || (loaded > 0 && tick < event.began + kEventMaxTicks)) {
                continue;
            }
            world.record(ChronicleKind::EventEnded, id, kInvalidId, loaded);
            for (const EntityId cache : event.caches) {
                if (!hasLoot(world, cache)) {
                    world.destroy(world.find(cache));
                }
            }
            world.destroy(entity);
        }
    }

    // Пустые грузы кончившихся ивентов — убрать.
    std::vector<std::pair<EntityId, entt::entity>> emptied;
    for (const auto [entity, id, cache, inventory] :
         registry.view<const Id, const Cache, const Inventory>().each()) {
        if (inventory.empty() && world.find(cache.event) == entt::null) {
            emptied.emplace_back(id.value, entity);
        }
    }
    std::sort(emptied.begin(), emptied.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, entity] : emptied) {
        world.destroy(entity);
    }
}

} // namespace lw
