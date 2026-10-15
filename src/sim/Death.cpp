#include "sim/Death.h"

#include "sim/World.h"

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
    }

    // Поведение снимается; позиция, черты, ноша и оружие остаются при теле.
    registry.remove<Person, Mind, Needs, Wounds, Zombie, Animal>(entity);
    registry.remove<Walker, Senses, Awareness, Combat>(entity);
    if (auto* velocity = registry.try_get<Velocity>(entity)) {
        *velocity = {};
    }
    registry.emplace<Corpse>(entity, corpse);
}

} // namespace lw
