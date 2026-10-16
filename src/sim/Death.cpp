#include "sim/Death.h"

#include "sim/Hash.h"
#include "sim/World.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace lw {

int carcassMeat(AnimalKind kind) {
    switch (kind) {
    case AnimalKind::Deer:
        return 8;
    case AnimalKind::Boar:
        return 6;
    case AnimalKind::Hare:
        return 2;
    case AnimalKind::Count:
        break;
    }
    return 0;
}

void die(World& world, entt::entity entity, DeathCause cause, EntityId killer) {
    entt::registry& registry = world.registry();
    if (!registry.valid(entity) || registry.all_of<Corpse>(entity)) {
        return;
    }
    const EntityId id = registry.get<Id>(entity).value;
    Corpse corpse;
    corpse.cause = cause;
    corpse.killer = killer;
    corpse.diedAt = world.tick();
    if (registry.all_of<Person>(entity)) {
        corpse.kind = CorpseKind::Human;
    } else if (registry.all_of<Zombie>(entity)) {
        corpse.kind = CorpseKind::Zombie;
    } else if (const auto* animal = registry.try_get<Animal>(entity)) {
        corpse.kind = CorpseKind::Animal;
        corpse.animal = animal->kind;
        corpse.meat = static_cast<std::uint8_t>(carcassMeat(animal->kind));
    } else {
        return; // не существо
    }
    if (corpse.kind == CorpseKind::Human) {
        world.record(ChronicleKind::Died, id, killer, static_cast<std::int32_t>(cause),
                     static_cast<std::int32_t>(corpse.kind));
        // Укушенный встанет: когда — из хеша id, без рандома (смерть случается
        // в разных системах, а время подъёма не должно зависеть от того, в какой).
        if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && wounds->bites > 0) {
            Hasher hasher;
            hasher.add(id);
            const auto span = static_cast<std::uint64_t>(kTurnMaxMinutes - kTurnMinMinutes + 1);
            const std::uint64_t minutes = kTurnMinMinutes + hasher.value() % span;
            corpse.turnsAt = world.tick() + minutes * kTicksPerGameMinute;
        }
    }

    // Поведение снимается; позиция, черты, ноша и оружие остаются при теле.
    registry.remove<Person, Mind, Needs, Wounds, Zombie, Animal>(entity);
    // Отношения — тоже: память о погибшем хранят живые, а не он сам.
    registry.remove<Walker, Senses, Awareness, Combat, Relations>(entity);
    if (auto* velocity = registry.try_get<Velocity>(entity)) {
        *velocity = {};
    }
    registry.emplace<Corpse>(entity, corpse);
}

void updateBodies(World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return;
    }
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> rising;
    std::vector<std::pair<EntityId, entt::entity>> decayed;
    for (const auto [entity, id, corpse] : registry.view<const Id, Corpse>().each()) {
        if (corpse.turnsAt != 0 && tick >= corpse.turnsAt) {
            rising.emplace_back(id.value, entity);
            continue;
        }
        const std::uint64_t age = tick - corpse.diedAt;
        if (corpse.kind == CorpseKind::Animal && age >= kMeatSpoilTicks) {
            corpse.meat = 0;
        }
        if (age >= (corpse.kind == CorpseKind::Animal ? kCarcassDecayTicks : kBodyDecayTicks)) {
            decayed.emplace_back(id.value, entity);
        }
    }
    // По возрастанию id: создание зомби тянет рандом.
    std::sort(rising.begin(), rising.end());
    std::sort(decayed.begin(), decayed.end());
    Rng& rng = world.rng(RngStream::Zombies);
    for (const auto& [id, entity] : rising) {
        const Position& position = registry.get<Position>(entity);
        registry.remove<Corpse>(entity);
        raiseZombie(world, entity, Map::cellAt(position.x, position.y), rng);
        world.record(ChronicleKind::Turned, id);
    }
    for (const auto& [id, entity] : decayed) {
        world.destroy(entity);
    }
}

} // namespace lw
