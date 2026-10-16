#include "sim/World.h"

#include "sim/Components.h"
#include "sim/Death.h"
#include "sim/Hash.h"
#include "sim/Population.h"
#include "sim/gen/MapGenerator.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/CombatSystem.h"
#include "sim/systems/MemorySystem.h"
#include "sim/systems/MovementSystem.h"
#include "sim/systems/NeedsSystem.h"
#include "sim/systems/PerceptionSystem.h"
#include "sim/systems/ResourceSystem.h"
#include "sim/systems/SocialSystem.h"
#include "sim/systems/UtilityAiSystem.h"
#include "sim/systems/WeatherSystem.h"
#include "sim/systems/ZombieSystem.h"

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
    spawnLoot(*this);
    spawnPeople(*this, config_.people);
    // Зомби селятся подальше от людей: индекс должен их уже видеть.
    spatial_.sync(registry_);
    spawnZombies(*this, config_.zombies);
    spatial_.sync(registry_);
    // Животные и птицы — вдали от людей.
    spawnAnimals(*this, config_.animals, config_.flocks);
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
    resources_.build(places_);
}

void World::step() {
    updateWeather(*this);
    updateResources(*this);
    tickNeeds(*this);
    // Отношения — по тому, кто был рядом и чем занят на начало тика.
    tickRelations(*this);
    // Память — угасание и находки (тела близких, близкие среди зомби).
    tickMemories(*this);
    // Укушенные встают, тела разлагаются — до восприятия: вставший виден сразу.
    updateBodies(*this);
    // До решений и движения: восприятие видит мир на начало тика.
    updatePerception(*this);
    // Зомби и люди решают по тому, что заметили; сами шаги — в движении.
    updateZombies(*this);
    updateAnimals(*this);
    updateUtilityAi(*this);
    // Удары и выстрелы — по целям, которые выставили решения; погибшие
    // становятся телами до движения.
    updateCombat(*this);
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
