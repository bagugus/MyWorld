#include "sim/World.h"

#include "sim/Components.h"
#include "sim/Hash.h"
#include "sim/gen/MapGenerator.h"
#include "sim/systems/WanderSystem.h"
#include "sim/systems/WeatherSystem.h"

#include <algorithm>
#include <utility>

namespace lw {

World::World(std::uint64_t seed, const WorldConfig& config)
    : seed_(seed),
      config_(config),
      map_(std::max(1, config.mapWidth), std::max(1, config.mapHeight)),
      spatial_(map_.worldWidth(), map_.worldHeight()) {
    registry_.on_destroy<Position>().connect<&SpatialIndex::onPositionDestroyed>(spatial_);

    const Rng root(seed);
    const auto streamCount = static_cast<std::size_t>(RngStream::Count);
    streams_.reserve(streamCount);
    for (std::size_t i = 0; i < streamCount; ++i) {
        streams_.push_back(root.fork(i + 1));
    }

    generate();
    initWeather(*this);
    spawnWanderers(*this, config_.wanderers);
    spatial_.sync(registry_);
}

void World::generate() {
    const GeneratedMap generated = generateMap(map_, rng(RngStream::MapGen));
    mapHash_ = map_.hash();

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

    nav_.build(map_, registry_);
    graph_.build(nav_);
}

void World::step() {
    updateWeather(*this);
    updateWander(*this);
    // Последним: между тиками индекс совпадает с позициями всех сущностей.
    spatial_.sync(registry_);
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

entt::entity World::find(EntityId id) const {
    const auto it = idToEntity_.find(id);
    if (it == idToEntity_.end()) {
        return entt::null;
    }
    return it->second;
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
        if (const auto* wander = registry_.try_get<Wander>(entity)) {
            hasher.add(std::uint64_t{3});
            hasher.add(static_cast<std::uint64_t>(wander->ticksUntilTurn));
            hasher.add(wander->speed);
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
    }
    return hasher.value();
}

} // namespace lw
