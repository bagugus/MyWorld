#pragma once

#include "sim/Perception.h"

#include <entt/entt.hpp>

namespace lw {

class World;

// Восприятие существ: обновляет Awareness у всех, у кого есть Senses, — раз в
// kPerceptionIntervalTicks, вразнобой по id. Правила зрения и слуха — в
// sim/Perception.h.
//
// Идёт в тике до решений и движения: видит всех там, где они были в начале
// тика (снимок SpatialIndex), и слышит то, что они делали в прошлом тике.
// Поэтому результат не зависит от порядка обхода существ. Разовые звуки старше
// kSoundLifetimeTicks здесь же убираются из мира.
void updatePerception(World& world);

// Обновить восприятие одного существа сейчас, вне очереди.
void perceive(World& world, entt::entity entity, const SenseConditions& conditions);

} // namespace lw
