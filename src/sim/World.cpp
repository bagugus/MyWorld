#include "sim/World.h"

#include "sim/Components.h"
#include "sim/Hash.h"
#include "sim/Population.h"
#include "sim/gen/MapGenerator.h"
#include "sim/systems/MovementSystem.h"
#include "sim/systems/NeedsSystem.h"
#include "sim/systems/UtilityAiSystem.h"
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
    spawnSupplies(*this);
    spawnPeople(*this, config_.people);
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
    places_.build(map_, nav_, registry_);
}

void World::step() {
    updateWeather(*this);
    tickNeeds(*this);
    updateUtilityAi(*this);
    updateMovement(*this, kTickDt);
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
    hasher.add(static_cast<std::uint64_t>(chronicle_.events().size()));
    hasher.add(chronicle_.hash());

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
            hasher.add(static_cast<std::uint64_t>(walker->waypoints.size()));
            for (const Cell cell : walker->waypoints) {
                hasher.add(cell.x);
                hasher.add(cell.y);
            }
            hasher.add(walker->next);
        }
        if (const auto* supplies = registry_.try_get<Supplies>(entity)) {
            hasher.add(std::uint64_t{11});
            hasher.add(supplies->food);
            hasher.add(supplies->water);
        }
        if (const auto* carry = registry_.try_get<Carry>(entity)) {
            hasher.add(std::uint64_t{12});
            hasher.add(carry->food);
            hasher.add(carry->water);
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
            for (const EntityId building : mind->searched) {
                hasher.add(building);
            }
            hasher.add(mind->searchedNext);
        }
    }
    return hasher.value();
}

} // namespace lw
