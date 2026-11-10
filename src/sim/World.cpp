#include "sim/World.h"

#include "sim/Components.h"
#include "sim/Death.h"
#include "sim/Hash.h"
#include "sim/Names.h"
#include "sim/Profiler.h"
#include "sim/Schema.h"
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
#include <tuple>
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

World::World(std::uint64_t seed, const WorldConfig& config, Blank)
    : seed_(seed),
      config_(config),
      map_(std::max(1, config.mapWidth), std::max(1, config.mapHeight)),
      spatial_(map_.worldWidth(), map_.worldHeight()) {
    registry_.on_destroy<Position>().connect<&SpatialIndex::onPositionDestroyed>(spatial_);
    activity_.reset(map_.worldWidth(), map_.worldHeight());

    const Rng root(seed);
    const auto streamCount = static_cast<std::size_t>(RngStream::Count);
    streams_.reserve(streamCount);
    for (std::size_t i = 0; i < streamCount; ++i) {
        streams_.push_back(root.fork(i + 1));
    }
}

World::World(std::uint64_t seed, const WorldConfig& config) : World(seed, config, Blank{}) {
    connectLod();
    const StartupScope total(profiler_, "world");
    generate();
    populate();
}

void World::connectLod() {
    // С LOD (этап 6а) у каждого, кто воспринимает мир, с рождения есть уровень
    // детализации — полный до первого пересчёта.
    if (config_.lod) {
        registry_.on_construct<Awareness>().connect<&attachLod>();
    }
}

void World::populate() {
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
// разреженного массива. Куча внутри компонентов — только путь Walker, состав
// групп, орд и грузов ивента и длинные имена (их считает memoryReport отдельно).
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

// Хранилища компонентов списка — один раз на обход, а не поиск на каждую сущность.
template <typename... T>
auto storagesOf(const entt::registry& registry, TypeList<T...>) {
    return std::tuple<const entt::storage_for_t<T>*...>{registry.storage<T>()...};
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

    std::size_t components = storageBytes<Id>(registry_);
    forEachType(SavedComponents{}, [&](auto type) {
        using T = typename decltype(type)::type;
        components += storageBytes<T>(registry_);
    });
    for (const auto [entity, walker] : registry_.view<const Walker>().each()) {
        components += walker.waypoints.capacity() * sizeof(Cell);
    }
    for (const auto [entity, group] : registry_.view<const Group>().each()) {
        components += group.members.capacity() * sizeof(EntityId);
    }
    for (const auto [entity, horde] : registry_.view<const Horde>().each()) {
        components += horde.members.capacity() * sizeof(EntityId);
    }
    for (const auto [entity, event] : registry_.view<const WorldEvent>().each()) {
        components += event.caches.capacity() * sizeof(EntityId);
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
    hashFields(hasher, config_.lod, weather_);
    hasher.add(static_cast<std::uint64_t>(chronicle_.events().size()));
    hasher.add(chronicle_.hash());
    hasher.add(resources_.hash());
    hasher.add(static_cast<std::uint64_t>(sounds_.size()));
    for (const Sound& sound : sounds_) {
        hashFields(hasher, config_.lod, sound);
    }

    std::vector<std::pair<EntityId, entt::entity>> order;
    order.reserve(idToEntity_.size());
    for (const auto [entity, id] : registry_.view<const Id>().each()) {
        order.emplace_back(id.value, entity);
    }
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Компоненты — по общему списку sim/Schema.h (SavedComponents): новый
    // компонент, влияющий на будущее мира, добавлять туда.
    const auto storages = storagesOf(registry_, SavedComponents{});
    HashArchive archive(hasher, config_.lod);
    for (const auto& [id, entity] : order) {
        hasher.add(id);
        forEachType(SavedComponents{}, [&](auto type) {
            using T = typename decltype(type)::type;
            const auto* storage = std::get<const entt::storage_for_t<T>*>(storages);
            if (storage != nullptr && storage->contains(entity)) {
                hasher.add(ComponentInfo<T>::kHashTag);
                io(archive, const_cast<T&>(storage->get(entity)));
            }
        });
    }
    return hasher.value();
}

std::uint64_t World::sitesHash() const {
    std::vector<std::pair<EntityId, entt::entity>> order;
    for (const auto [entity, id] : registry_.view<const Id>().each()) {
        if (registry_.any_of<Settlement, Building>(entity)) {
            order.emplace_back(id.value, entity);
        }
    }
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    Hasher hasher;
    for (const auto& [id, entity] : order) {
        hasher.add(id);
        if (const auto* settlement = registry_.try_get<Settlement>(entity)) {
            hashFields(hasher, config_.lod, *settlement);
        }
        if (const auto* building = registry_.try_get<Building>(entity)) {
            hashFields(hasher, config_.lod, *building);
        }
    }
    return hasher.value();
}

} // namespace lw
