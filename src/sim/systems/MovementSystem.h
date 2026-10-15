#pragma once

#include "sim/Map.h"

#include <entt/entt.hpp>

#include <cstdint>

namespace lw {

class World;

// Во сколько раз медленнее дороги идти по клетке со стоимостью cost (NavGrid):
// по суше — обратно стоимости, по воде — по глубине: вброд, по грудь, вплавь.
// У непроходимой клетки — ноль.
float walkSpeedFactor(std::uint8_t cost);

// Проложить путь сущности с Position и Walker от её клетки до goal и пойти.
// false — пути нет; тогда сущность останавливается.
bool startWalk(World& world, entt::entity entity, Cell goal);

// Продвинуть всех идущих на dt секунд симуляции. Шаг может быть длиннее тика:
// за него сущность проходит несколько точек пути (задел под грубую симуляцию вдали).
// Velocity — фактическая скорость за шаг.
void updateMovement(World& world, float dt);

} // namespace lw
