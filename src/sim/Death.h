#pragma once

#include "sim/Components.h"

#include <entt/entt.hpp>

namespace lw {

class World;

// Смерть (этап 5.4). Погибшее существо становится телом — той же сущностью с
// тем же id: остаются позиция и то, что было при себе (черты, ноша, оружие),
// а поведение снимается (Person, Zombie, Animal, Mind, Walker, Senses,
// Awareness и т. д.), и появляется Corpse. Поэтому биография в летописи
// продолжается, а системы, которые ищут живых по их компонентам, тела не видят.
//
// Вызывать вне обхода видов с компонентами погибшего: компоненты снимаются.
// Летопись: смерть человека — Died (кто убил, причина); зомби и животных
// отмечает тот, кто убил (KilledZombie, Hunted).
void die(World& world, entt::entity entity, DeathCause cause, EntityId killer);

// Порций мяса в туше животного этого вида.
int carcassMeat(AnimalKind kind);

} // namespace lw
