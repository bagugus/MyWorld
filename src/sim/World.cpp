#include "sim/World.h"

#include "sim/Components.h"
#include "sim/Death.h"
#include "sim/Hash.h"
#include "sim/Names.h"
#include "sim/Profiler.h"
#include "sim/Population.h"
#include "sim/Work.h"
#include "sim/gen/MapGenerator.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/CombatSystem.h"
#include "sim/systems/EventSystem.h"
#include "sim/systems/FamilySystem.h"
#include "sim/systems/GroupSystem.h"
#include "sim/systems/InfectionSystem.h"
#include "sim/systems/LodSystem.h"
#include "sim/systems/MemorySystem.h"
#include "sim/systems/MovementSystem.h"
#include "sim/systems/NeedsSystem.h"
#include "sim/systems/PerceptionSystem.h"
#include "sim/systems/ResourceSystem.h"
#include "sim/systems/ScenarioSystem.h"
#include "sim/systems/SocialSystem.h"
#include "sim/systems/SocietySystem.h"
#include "sim/systems/UtilityAiSystem.h"
#include "sim/systems/WeatherSystem.h"
#include "sim/systems/WorkSystem.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <type_traits>
#include <utility>

namespace lw {

Population populationFor(int width, int height) {
    const WorldConfig defaults;
    const double share = static_cast<double>(width) * static_cast<double>(height) /
                         (static_cast<double>(kDefaultMapCells) * static_cast<double>(kDefaultMapCells));
    const auto scaled = [share](int count) { return static_cast<int>(std::lround(count * share)); };
    return {scaled(defaults.people), scaled(defaults.animals), scaled(defaults.flocks)};
}

World::World(std::uint64_t seed, const WorldConfig& config)
    : seed_(seed),
      config_(config),
      map_(std::max(1, config.mapWidth), std::max(1, config.mapHeight)),
      spatial_(map_.worldWidth(), map_.worldHeight()) {
    registry_.on_destroy<Position>().connect<&SpatialIndex::onPositionDestroyed>(spatial_);
    activity_.reset(map_.worldWidth(), map_.worldHeight());
    // С LOD (этап 6а) у каждого, кто воспринимает мир, с рождения есть уровень
    // детализации — полный до первого пересчёта.
    if (config_.lod) {
        registry_.on_construct<Awareness>().connect<&attachLod>();
    }

    const Rng root(seed);
    const auto streamCount = static_cast<std::size_t>(RngStream::Count);
    streams_.reserve(streamCount);
    for (std::size_t i = 0; i < streamCount; ++i) {
        streams_.push_back(root.fork(i + 1));
    }

    const StartupScope total(profiler_, "world");
    generate();
    {
        const StartupScope step(profiler_, "world/weather");
        initWeather(*this);
    }
    {
        const StartupScope step(profiler_, "world/loot");
        spawnLoot(*this);
    }
    {
        const StartupScope step(profiler_, "world/people");
        spawnPeople(*this, config_.people);
        // Профессии и сбережения (этап 7.2) — свой поток рандома: остальное не сдвигается.
        if (config_.peacetime) {
            assignJobs(*this);
        }
    }
    {
        const StartupScope step(profiler_, "world/zombies");
        // Зомби селятся подальше от людей: индекс должен их уже видеть.
        spatial_.sync(registry_);
        spawnZombies(*this, config_.zombies);
        spatial_.sync(registry_);
    }
    {
        const StartupScope step(profiler_, "world/animals");
        // Животные и птицы — вдали от людей.
        spawnAnimals(*this, config_.animals, config_.flocks);
        spatial_.sync(registry_);
    }
    // Сценарий (этап 7.4) — последним: id остальных сущностей не сдвигаются.
    if (config_.peacetime) {
        const StartupScope step(profiler_, "world/scenario");
        createScenario(*this);
    }
}

void World::generate() {
    GeneratedMap generated;
    {
        const StartupScope step(profiler_, "world/map");
        generated = generateMap(map_, rng(RngStream::MapGen), &profiler_);
        mapHash_ = map_.hash();
    }
    {
        const StartupScope step(profiler_, "world/sites");

        // Порядок создания задаёт id: сначала поселения, затем здания.
        std::vector<EntityId> settlementIds;
        settlementIds.reserve(generated.settlements.size());
        for (const SettlementSite& site : generated.settlements) {
            const entt::entity entity = create();
            registry_.emplace<Settlement>(entity, site.kind, site.name, site.center, site.radius);
            settlementIds.push_back(registry_.get<Id>(entity).value);
        }
        for (const BuildingSite& site : generated.buildings) {
            const entt::entity entity = create();
            registry_.emplace<Building>(entity, site.kind, site.origin, site.width, site.height,
                                        site.door,
                                        settlementIds[static_cast<std::size_t>(site.settlement)]);
        }
        // Сеть дорог (этап 2а.3) — с id поселений вместо индексов.
        roads_.links.reserve(generated.roads.size());
        for (RoadSite& site : generated.roads) {
            RoadLink link;
            link.kind = site.kind;
            link.from = settlementIds[static_cast<std::size_t>(site.from)];
            link.to = settlementIds[static_cast<std::size_t>(site.to)];
            link.path = std::move(site.path);
            link.lengthMeters = site.lengthMeters;
            roads_.links.push_back(std::move(link));
        }
        roads_.junctions = std::move(generated.junctions);
        roads_.bridges = std::move(generated.bridges);
    }
    {
        const StartupScope step(profiler_, "world/nav grid");
        nav_.build(map_, registry_);
    }
    {
        const StartupScope step(profiler_, "world/nav graph");
        graph_.build(nav_);
    }
    {
        const StartupScope step(profiler_, "world/places");
        places_.build(map_, nav_, registry_);
        resources_.build(places_);
    }
}

void World::step() {
    Profiler& p = profiler_;
    const bool profiling = p.enabled();
    const Profiler::Clock::time_point tickStart =
        profiling ? Profiler::Clock::now() : Profiler::Clock::time_point{};
    {
        LW_PROFILE(p, "weather");
        updateWeather(*this);
    }
    {
        LW_PROFILE(p, "resources");
        updateResources(*this);
    }
    {
        // Сценарий — до ивентов, работы и решений: фаза задаёт правила этой минуты.
        LW_PROFILE(p, "scenario");
        updateScenario(*this);
    }
    {
        // Ивенты — до восприятия: шум начавшегося ивента слышат в этом же тике.
        LW_PROFILE(p, "events");
        updateEvents(*this);
    }
    {
        // Работа — до решений: кто на месте (открыт ли магазин), плата, завоз.
        LW_PROFILE(p, "work");
        updateWork(*this);
    }
    {
        LW_PROFILE(p, "needs");
        tickNeeds(*this);
    }
    {
        // Заражение от больных (этап 7.5) — после нужд: заражение уже подросло за
        // эту минуту, кто стал заразен — заразен сразу.
        LW_PROFILE(p, "infection");
        tickInfection(*this);
    }
    {
        // Реакция общества (этап 7.6): передачи, увиденное, карантин, колонны
        // эвакуации — до отношений и решений: кто уехал, того в мире уже нет.
        LW_PROFILE(p, "society");
        updateSociety(*this);
    }
    {
        // Отношения — по тому, кто был рядом и чем занят на начало тика.
        LW_PROFILE(p, "relations");
        tickRelations(*this);
    }
    {
        // Память — угасание и находки (тела близких, близкие среди зомби).
        LW_PROFILE(p, "memories");
        tickMemories(*this);
    }
    {
        // Семьи — раз в час: у каждого ребёнка опекун (этап 7.3); до групп —
        // ребёнок идёт в группу за опекуном.
        LW_PROFILE(p, "families");
        tickFamilies(*this);
    }
    {
        // Группы — раз в час, по отношениям: кто вступил, ушёл, кто вожак.
        LW_PROFILE(p, "groups");
        tickGroups(*this);
    }
    {
        // Укушенные встают, тела разлагаются — до восприятия: вставший виден сразу.
        LW_PROFILE(p, "bodies");
        updateBodies(*this);
    }
    {
        // Уровни детализации (этап 6а) — до восприятия и решений: в этом тике
        // все системы видят одни и те же уровни. Вставший из тела уже зомби.
        LW_PROFILE(p, "lod");
        updateLod(*this);
    }
    {
        // До решений и движения: восприятие видит мир на начало тика.
        LW_PROFILE(p, "perception");
        updatePerception(*this);
    }
    {
        // Зомби и люди решают по тому, что заметили; сами шаги — в движении.
        LW_PROFILE(p, "zombies");
        updateZombies(*this);
    }
    {
        LW_PROFILE(p, "animals");
        updateAnimals(*this);
    }
    {
        LW_PROFILE(p, "utility ai");
        updateUtilityAi(*this);
    }
    {
        // Удары и выстрелы — по целям, которые выставили решения; погибшие
        // становятся телами до движения.
        LW_PROFILE(p, "combat");
        updateCombat(*this);
    }
    {
        LW_PROFILE(p, "movement");
        updateMovement(*this, kTickDt);
    }
    {
        // Последним: между тиками индекс совпадает с позициями всех сущностей.
        LW_PROFILE(p, "spatial sync");
        spatial_.sync(registry_);
    }
    if (profiling) {
        p.countTick(std::chrono::duration<double>(Profiler::Clock::now() - tickStart).count());
    }
    ++tick_;
}

entt::entity World::create() {
    const entt::entity entity = registry_.create();
    const EntityId id = nextId_++;
    registry_.emplace<Id>(entity, id);
    idToEntity_.emplace(id, entity);
    return entity;
}

void World::destroy(entt::entity entity) {
    if (!registry_.valid(entity)) {
        return;
    }
    idToEntity_.erase(registry_.get<Id>(entity).value);
    registry_.destroy(entity);
}

void World::forgetSounds(std::uint64_t before) {
    const auto keep = std::find_if(sounds_.begin(), sounds_.end(),
                                   [before](const Sound& sound) { return sound.tick >= before; });
    sounds_.erase(sounds_.begin(), keep);
}

entt::entity World::find(EntityId id) const {
    const auto it = idToEntity_.find(id);
    if (it == idToEntity_.end()) {
        return entt::null;
    }
    return it->second;
}

namespace {

// Память хранилища компонента T: плотный массив значений и сущностей, страницы
// разреженного массива. Куча внутри компонентов — только путь Walker и состав
// групп и орд (их считает memoryReport отдельно).
template <typename T>
std::size_t storageBytes(const entt::registry& registry) {
    const auto* storage = registry.storage<T>();
    if (storage == nullptr) {
        return 0;
    }
    constexpr std::size_t kSparsePage = 4096; // ENTT_SPARSE_PAGE
    const std::size_t payload = std::is_empty_v<T> ? 0 : sizeof(T);
    return storage->capacity() * (payload + sizeof(entt::entity)) +
           storage->extent() / kSparsePage * kSparsePage * sizeof(entt::entity);
}

template <typename... T>
std::size_t storagesBytes(const entt::registry& registry) {
    return (storageBytes<T>(registry) + ...);
}

} // namespace

std::vector<MemoryItem> World::memoryReport() const {
    std::vector<MemoryItem> report;
    report.push_back({"map layers", map_.memoryBytes()});
    report.push_back({"nav grid", nav_.memoryBytes()});
    report.push_back({"nav graph", graph_.memoryBytes()});
    report.push_back({"pathfinder buffers", pathfinder_.memoryBytes()});
    report.push_back({"places", places_.memoryBytes()});
    report.push_back({"resources", resources_.memoryBytes()});
    report.push_back({"spatial index", spatial_.memoryBytes()});
    report.push_back({"activity map", activity_.memoryBytes()});
    report.push_back({"chronicle", chronicle_.memoryBytes()});

    std::size_t components =
        storagesBytes<Id, Name, Position, Velocity, Settlement, Building, Person, Needs, Traits, Age,
                      Kin, Inventory, Mind, Relations, Memories, Group, Job, Wallet, Workplace,
                      WorldEvent, Cache, Leads, Scenario, Alarm, Quarantine, EvacuationPoint,
                      Evacuated, Senses, Awareness, Lod, Walker, Zombie, Horde, Animal, Flock,
                      Weapon, Combat, Wounds, Corpse>(registry_);
    for (const auto [entity, walker] : registry_.view<const Walker>().each()) {
        components += walker.waypoints.capacity() * sizeof(Cell);
    }
    for (const auto [entity, group] : registry_.view<const Group>().each()) {
        components += group.members.capacity() * sizeof(EntityId);
    }
    for (const auto [entity, horde] : registry_.view<const Horde>().each()) {
        components += horde.members.capacity() * sizeof(EntityId);
    }
    for (const auto [entity, name] : registry_.view<const Name>().each()) {
        components += name.first.capacity() > 15 ? name.first.capacity() + 1 : 0;
        components += name.last.capacity() > 15 ? name.last.capacity() + 1 : 0;
    }
    report.push_back({"components", components});
    report.push_back({"id lookup", idToEntity_.bucket_count() * sizeof(void*) +
                                       idToEntity_.size() * (sizeof(EntityId) + sizeof(entt::entity) +
                                                             2 * sizeof(void*))});
    return report;
}

std::uint64_t World::stateHash() const {
    Hasher hasher;
    hasher.add(tick_);
    hasher.add(nextId_);
    hasher.add(mapHash_);
    for (const Rng& stream : streams_) {
        hasher.add(stream.state());
        hasher.add(stream.increment());
    }
    hasher.add(static_cast<std::uint8_t>(weather_.kind));
    hasher.add(weather_.minutesLeft);
    hasher.add(weather_.cloudCover);
    hasher.add(weather_.precipitation);
    hasher.add(weather_.fog);
    hasher.add(weather_.windSpeed);
    hasher.add(weather_.windTarget);
    hasher.add(weather_.windFrom);
    hasher.add(weather_.anomaly);
    hasher.add(weather_.snowCover);
    hasher.add(static_cast<std::uint64_t>(chronicle_.events().size()));
    hasher.add(chronicle_.hash());
    hasher.add(resources_.hash());
    hasher.add(static_cast<std::uint64_t>(sounds_.size()));
    for (const Sound& sound : sounds_) {
        hasher.add(sound.tick);
        hasher.add(sound.x);
        hasher.add(sound.y);
        hasher.add(sound.loudness);
        hasher.add(sound.source);
        hasher.add(static_cast<std::uint8_t>(sound.kind));
    }

    std::vector<std::pair<EntityId, entt::entity>> order;
    order.reserve(idToEntity_.size());
    for (const auto [entity, id] : registry_.view<const Id>().each()) {
        order.emplace_back(id.value, entity);
    }
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Каждый новый компонент, влияющий на будущее мира, добавлять сюда.
    for (const auto& [id, entity] : order) {
        hasher.add(id);
        if (const auto* position = registry_.try_get<Position>(entity)) {
            hasher.add(std::uint64_t{1});
            hasher.add(position->x);
            hasher.add(position->y);
        }
        if (const auto* velocity = registry_.try_get<Velocity>(entity)) {
            hasher.add(std::uint64_t{2});
            hasher.add(velocity->x);
            hasher.add(velocity->y);
        }
        if (const auto* settlement = registry_.try_get<Settlement>(entity)) {
            hasher.add(std::uint64_t{4});
            hasher.add(static_cast<std::uint8_t>(settlement->kind));
            hasher.add(static_cast<std::uint64_t>(settlement->name.size()));
            for (const char c : settlement->name) {
                hasher.add(static_cast<std::uint8_t>(c));
            }
            hasher.add(settlement->center.x);
            hasher.add(settlement->center.y);
            hasher.add(settlement->radius);
        }
        if (const auto* building = registry_.try_get<Building>(entity)) {
            hasher.add(std::uint64_t{5});
            hasher.add(static_cast<std::uint8_t>(building->kind));
            hasher.add(building->origin.x);
            hasher.add(building->origin.y);
            hasher.add(building->width);
            hasher.add(building->height);
            hasher.add(building->door.x);
            hasher.add(building->door.y);
            hasher.add(building->settlement);
        }
        if (const auto* person = registry_.try_get<Person>(entity)) {
            hasher.add(std::uint64_t{6});
            hasher.add(person->home);
            hasher.add(static_cast<std::uint8_t>(person->asleep));
            hasher.add(static_cast<std::uint8_t>(person->indoors));
            hasher.add(person->group);
            hasher.add(person->formerHome);
        }
        if (const auto* job = registry_.try_get<Job>(entity)) {
            hasher.add(std::uint64_t{32});
            hasher.add(static_cast<std::uint8_t>(job->profession));
            hasher.add(job->workplace);
            hasher.add(job->start);
            hasher.add(job->end);
            hasher.add(job->days);
        }
        if (const auto* wallet = registry_.try_get<Wallet>(entity)) {
            hasher.add(std::uint64_t{33});
            hasher.add(wallet->money);
        }
        if (const auto* workplace = registry_.try_get<Workplace>(entity)) {
            hasher.add(std::uint64_t{34});
            hasher.add(workplace->onDuty);
            hasher.add(workplace->till);
            hasher.add(workplace->harvest);
        }
        if (const auto* age = registry_.try_get<Age>(entity)) {
            hasher.add(std::uint64_t{35});
            hasher.add(age->years);
        }
        if (const auto* kin = registry_.try_get<Kin>(entity)) {
            hasher.add(std::uint64_t{36});
            hasher.add(kin->count);
            for (std::size_t i = 0; i < kin->count; ++i) {
                hasher.add(kin->relatives[i].id);
                hasher.add(static_cast<std::uint8_t>(kin->relatives[i].kind));
            }
            hasher.add(kin->guardian);
        }
        if (const auto* name = registry_.try_get<Name>(entity)) {
            hasher.add(std::uint64_t{31});
            for (const char c : name->first) {
                hasher.add(static_cast<std::uint8_t>(c));
            }
            hasher.add(std::uint8_t{0});
            for (const char c : name->last) {
                hasher.add(static_cast<std::uint8_t>(c));
            }
        }
        if (const auto* group = registry_.try_get<Group>(entity)) {
            hasher.add(std::uint64_t{27});
            hasher.add(group->leader);
            hasher.add(static_cast<std::uint64_t>(group->members.size()));
            for (const EntityId member : group->members) {
                hasher.add(member);
            }
            hasher.add(group->base);
            hasher.add(group->formed);
        }
        if (const auto* event = registry_.try_get<WorldEvent>(entity)) {
            hasher.add(std::uint64_t{28});
            hasher.add(static_cast<std::uint8_t>(event->kind));
            hasher.add(event->site.x);
            hasher.add(event->site.y);
            hasher.add(event->began);
            hasher.add(event->signalUntil);
            hasher.add(event->settlement);
            hasher.add(static_cast<std::uint64_t>(event->caches.size()));
            for (const EntityId cache : event->caches) {
                hasher.add(cache);
            }
        }
        if (const auto* scenario = registry_.try_get<Scenario>(entity)) {
            hasher.add(std::uint64_t{35});
            hasher.add(static_cast<std::uint8_t>(scenario->kind));
            hasher.add(static_cast<std::uint8_t>(scenario->phase));
            for (const std::uint64_t began : scenario->began) {
                hasher.add(began);
            }
            hasher.add(scenario->outbreakAt);
            hasher.add(scenario->population);
            hasher.add(scenario->patientZero);
            hasher.add(scenario->quietSince);
            hasher.add(scenario->broadcast);
            hasher.add(scenario->broadcastAt);
        }
        if (const auto* alarm = registry_.try_get<Alarm>(entity)) {
            hasher.add(std::uint64_t{37});
            hasher.add(alarm->level);
            hasher.add(static_cast<std::uint8_t>(alarm->clue));
            hasher.add(alarm->source);
            hasher.add(alarm->since);
            hasher.add(alarm->refusedAt);
        }
        if (const auto* quarantine = registry_.try_get<Quarantine>(entity)) {
            hasher.add(std::uint64_t{38});
            hasher.add(quarantine->since);
            hasher.add(quarantine->cases);
            hasher.add(quarantine->clearSince);
        }
        if (const auto* point = registry_.try_get<EvacuationPoint>(entity)) {
            hasher.add(std::uint64_t{39});
            hasher.add(point->settlement);
            hasher.add(point->opened);
            hasher.add(point->convoys);
            hasher.add(point->evacuated);
        }
        if (const auto* evacuated = registry_.try_get<Evacuated>(entity)) {
            hasher.add(std::uint64_t{40});
            hasher.add(evacuated->tick);
            hasher.add(evacuated->point);
        }
        if (const auto* cache = registry_.try_get<Cache>(entity)) {
            hasher.add(std::uint64_t{29});
            hasher.add(cache->event);
        }
        if (const auto* leads = registry_.try_get<Leads>(entity)) {
            hasher.add(std::uint64_t{30});
            hasher.add(leads->count);
            for (std::size_t i = 0; i < leads->count; ++i) {
                const Lead& lead = leads->items[i];
                hasher.add(lead.event);
                hasher.add(lead.site.x);
                hasher.add(lead.site.y);
                hasher.add(lead.since);
                hasher.add(lead.source);
                hasher.add(static_cast<std::uint8_t>(lead.visited));
                hasher.add(static_cast<std::uint8_t>(lead.empty));
            }
        }
        if (const auto* needs = registry_.try_get<Needs>(entity)) {
            hasher.add(std::uint64_t{9});
            hasher.add(needs->hunger);
            hasher.add(needs->thirst);
            hasher.add(needs->fatigue);
            hasher.add(needs->fear);
            hasher.add(needs->loneliness);
            hasher.add(needs->health);
        }
        if (const auto* traits = registry_.try_get<Traits>(entity)) {
            hasher.add(std::uint64_t{10});
            hasher.add(traits->bravery);
            hasher.add(traits->greed);
            hasher.add(traits->altruism);
            hasher.add(traits->aggression);
            hasher.add(traits->leadership);
            hasher.add(traits->sociability);
            hasher.add(traits->curiosity);
            hasher.add(traits->prudence);
            hasher.add(traits->chronotype);
        }
        if (const auto* walker = registry_.try_get<Walker>(entity)) {
            hasher.add(std::uint64_t{7});
            hasher.add(walker->speed);
            hasher.add(walker->pace);
            hasher.add(static_cast<std::uint64_t>(walker->waypoints.size()));
            for (const Cell cell : walker->waypoints) {
                hasher.add(cell.x);
                hasher.add(cell.y);
            }
            hasher.add(walker->next);
        }
        if (const auto* inventory = registry_.try_get<Inventory>(entity)) {
            hasher.add(std::uint64_t{24});
            for (const std::uint16_t count : inventory->items) {
                hasher.add(count);
            }
        }
        if (const auto* mind = registry_.try_get<Mind>(entity)) {
            hasher.add(std::uint64_t{13});
            hasher.add(static_cast<std::uint8_t>(mind->action));
            hasher.add(static_cast<std::uint8_t>(mind->phase));
            hasher.add(mind->score);
            hasher.add(mind->target);
            hasher.add(mind->spot.x);
            hasher.add(mind->spot.y);
            hasher.add(mind->until);
            hasher.add(mind->nextThink);
            hasher.add(mind->failures);
            hasher.add(mind->emptySearches);
            for (const EntityId building : mind->searched) {
                hasher.add(building);
            }
            hasher.add(mind->searchedNext);
            hasher.add(mind->fledAt);
            for (const Danger& danger : mind->dangers) {
                hasher.add(danger.cell.x);
                hasher.add(danger.cell.y);
                hasher.add(danger.until);
            }
        }
        if (const auto* senses = registry_.try_get<Senses>(entity)) {
            hasher.add(std::uint64_t{14});
            hasher.add(senses->sight);
            hasher.add(senses->hearing);
        }
        // Уровень детализации (этап 6а) — только с LOD: без него компонентов нет
        // и хеш тот же, что до этапа. Причина (cause) и окно восприятия
        // (Awareness::window — производное от уровня) на будущее не влияют.
        if (const auto* lod = registry_.try_get<Lod>(entity)) {
            hasher.add(std::uint64_t{41});
            hasher.add(static_cast<std::uint8_t>(lod->level));
            hasher.add(lod->fullUntil);
        }
        if (const auto* awareness = registry_.try_get<Awareness>(entity)) {
            hasher.add(std::uint64_t{15});
            hasher.add(awareness->count);
            for (std::size_t i = 0; i < awareness->count; ++i) {
                const Contact& contact = awareness->contacts[i];
                hasher.add(contact.id);
                hasher.add(contact.x);
                hasher.add(contact.y);
                hasher.add(contact.tick);
                hasher.add(static_cast<std::uint8_t>(contact.seen));
                hasher.add(static_cast<std::uint8_t>(contact.heard));
            }
            // С какого тика слышны звуки (этап 6а) — только с LOD: без него
            // обновления идут ровно через kPerceptionIntervalTicks, и это
            // поле ничего не меняет, а хеш остаётся прежним.
            if (config_.lod) {
                hasher.add(awareness->updated);
            }
        }
        if (const auto* zombie = registry_.try_get<Zombie>(entity)) {
            hasher.add(std::uint64_t{16});
            hasher.add(static_cast<std::uint8_t>(zombie->state));
            hasher.add(zombie->target);
            hasher.add(zombie->goal.x);
            hasher.add(zombie->goal.y);
            hasher.add(zombie->anchor.x);
            hasher.add(zombie->anchor.y);
            hasher.add(zombie->horde);
            hasher.add(zombie->until);
            hasher.add(zombie->checked);
            hasher.add(zombie->alerted);
            hasher.add(zombie->lastGroan);
            hasher.add(zombie->health);
        }
        if (const auto* horde = registry_.try_get<Horde>(entity)) {
            hasher.add(std::uint64_t{17});
            hasher.add(horde->leader);
            hasher.add(static_cast<std::uint64_t>(horde->members.size()));
            for (const EntityId member : horde->members) {
                hasher.add(member);
            }
            hasher.add(horde->goal.x);
            hasher.add(horde->goal.y);
            hasher.add(static_cast<std::uint8_t>(horde->travelling));
            hasher.add(horde->until);
            hasher.add(horde->x);
            hasher.add(horde->y);
            hasher.add(horde->settlement);
        }
        if (const auto* animal = registry_.try_get<Animal>(entity)) {
            hasher.add(std::uint64_t{18});
            hasher.add(static_cast<std::uint8_t>(animal->kind));
            hasher.add(static_cast<std::uint8_t>(animal->state));
            hasher.add(animal->anchor.x);
            hasher.add(animal->anchor.y);
            hasher.add(animal->goal.x);
            hasher.add(animal->goal.y);
            hasher.add(animal->until);
            hasher.add(animal->threat);
            hasher.add(animal->health);
            hasher.add(animal->hurtAt);
            hasher.add(animal->hurtBy);
        }
        if (const auto* flock = registry_.try_get<Flock>(entity)) {
            hasher.add(std::uint64_t{19});
            hasher.add(flock->birds);
            hasher.add(static_cast<std::uint8_t>(flock->flying));
            hasher.add(flock->toX);
            hasher.add(flock->toY);
            hasher.add(flock->until);
        }
        if (const auto* weapon = registry_.try_get<Weapon>(entity)) {
            hasher.add(std::uint64_t{20});
            hasher.add(static_cast<std::uint8_t>(weapon->kind));
            hasher.add(weapon->ammo);
        }
        if (const auto* combat = registry_.try_get<Combat>(entity)) {
            hasher.add(std::uint64_t{21});
            hasher.add(combat->target);
            hasher.add(combat->ready);
        }
        if (const auto* wounds = registry_.try_get<Wounds>(entity)) {
            hasher.add(std::uint64_t{22});
            hasher.add(wounds->bleeding);
            hasher.add(wounds->bites);
            hasher.add(wounds->infection);
            hasher.add(wounds->hurtAt);
            hasher.add(wounds->hurtBy);
            hasher.add(static_cast<std::uint8_t>(wounds->route));
            hasher.add(wounds->infectedBy);
            hasher.add(wounds->infectedAt);
        }
        if (const auto* relations = registry_.try_get<Relations>(entity)) {
            hasher.add(std::uint64_t{25});
            hasher.add(relations->count);
            for (std::size_t i = 0; i < relations->count; ++i) {
                const Bond& bond = relations->bonds[i];
                hasher.add(bond.other);
                hasher.add(bond.familiarity);
                hasher.add(bond.affinity);
                hasher.add(bond.trust);
                hasher.add(bond.chemistry);
                hasher.add(bond.met);
                hasher.add(bond.seen);
                hasher.add(bond.flags);
                hasher.add(bond.debt);
                hasher.add(bond.grudge);
                hasher.add(static_cast<std::uint8_t>(bond.origin));
            }
        }
        if (const auto* memories = registry_.try_get<Memories>(entity)) {
            hasher.add(std::uint64_t{26});
            hasher.add(memories->count);
            for (std::size_t i = 0; i < memories->count; ++i) {
                const Memory& memory = memories->items[i];
                hasher.add(static_cast<std::uint8_t>(memory.kind));
                hasher.add(memory.who);
                hasher.add(memory.victim);
                hasher.add(memory.about);
                hasher.add(memory.source);
                hasher.add(memory.tick);
                hasher.add(memory.strength);
            }
        }
        if (const auto* corpse = registry_.try_get<Corpse>(entity)) {
            hasher.add(std::uint64_t{23});
            hasher.add(static_cast<std::uint8_t>(corpse->kind));
            hasher.add(static_cast<std::uint8_t>(corpse->animal));
            hasher.add(static_cast<std::uint8_t>(corpse->cause));
            hasher.add(corpse->killer);
            hasher.add(corpse->diedAt);
            hasher.add(corpse->meat);
            hasher.add(corpse->turnsAt);
        }
    }
    return hasher.value();
}

} // namespace lw
