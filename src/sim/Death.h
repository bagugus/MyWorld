#pragma once

#include "sim/Components.h"
#include "sim/TimeScale.h"

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
// отмечает тот, кто убил (KilledZombie, Hunted). Укушенный человек встанет
// зомби (updateBodies).
void die(World& world, entt::entity entity, DeathCause cause, EntityId killer);

// Порций мяса в туше животного этого вида.
int carcassMeat(AnimalKind kind);

// Тела (этап 5.5), раз в игровую минуту:
// - укушенный погибший встаёт зомби (Corpse::turnsAt) — той же сущностью, с
//   тем же id; оружие и ноша остаются при нём;
// - мясо в туше портится через kMeatSpoilTicks;
// - тела разлагаются и исчезают: человека и зомби — через kBodyDecayTicks,
//   туша — через kCarcassDecayTicks.
inline constexpr std::uint64_t kMeatSpoilTicks = kTicksPerGameDay;
inline constexpr std::uint64_t kCarcassDecayTicks = 3 * kTicksPerGameDay;
inline constexpr std::uint64_t kBodyDecayTicks = 6 * kTicksPerGameDay;
// Укушенный встаёт через столько игровых минут после смерти.
inline constexpr int kTurnMinMinutes = 10;
inline constexpr int kTurnMaxMinutes = 40;
void updateBodies(World& world);

} // namespace lw
