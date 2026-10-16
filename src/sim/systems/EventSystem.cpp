#include "sim/systems/EventSystem.h"

#include "sim/Calendar.h"
#include "sim/Events.h"
#include "sim/Perception.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
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

bool hasLoot(const World& world, EntityId cache) {
    const entt::entity entity = world.find(cache);
    const auto* inventory = entity != entt::null ? world.registry().try_get<Inventory>(entity) : nullptr;
    return inventory != nullptr && !inventory->empty();
}

} // namespace

std::optional<Cell> pickEventSite(const World& world, EventKind kind, Rng& rng) {
    const Map& map = world.map();
    const entt::registry& registry = world.registry();
    const int margin = std::min(kSiteMargin, std::min(map.width(), map.height()) / 4);
    const auto randomCell = [&] {
        return Cell{rng.rangeInt(margin, map.width() - 1 - margin),
                    rng.rangeInt(margin, map.height() - 1 - margin)};
    };
    // Подходит ли место виду ивента: припасы сбрасывают недалеко от людей, но
    // не на дома.
    const auto suits = [&](Cell cell) {
        if (!openGround(world, cell)) {
            return false;
        }
        const float x = Map::cellCenter(cell.x);
        const float y = Map::cellCenter(cell.y);
        const entt::entity nearest = world.find(world.places().nearestSettlement(x, y));
        if (nearest == entt::null) {
            return true;
        }
        const Settlement& settlement = registry.get<Settlement>(nearest);
        const float edge = static_cast<float>(settlement.radius) * Map::kCellSize;
        const float d = metersBetween(x, y, Map::cellCenter(settlement.center.x),
                                      Map::cellCenter(settlement.center.y)) - edge;
        switch (kind) {
        case EventKind::SupplyDrop:
            return d >= kDropFromEdgeMin && d <= kDropFromEdgeMax;
        case EventKind::Count:
            break;
        }
        return true;
    };
    for (int attempt = 0; attempt < kSiteAttempts; ++attempt) {
        if (const Cell cell = randomCell(); suits(cell)) {
            return cell;
        }
    }
    // Подходящего не нашлось (маленькая карта) — любое открытое место.
    for (int attempt = 0; attempt < kSiteAttempts; ++attempt) {
        if (const Cell cell = randomCell(); openGround(world, cell)) {
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
        if (i > 0) {
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
    // Позиции остальных с начала тика не менялись: индекс узнаёт только о грузах.
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
