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

// То же, но если цель близко (до kDirectWalkCells клеток) и прямая до неё
// проходима, — по прямой, без поиска пути: так ходят зомби, им не до выбора
// дороги, а путь к движущейся цели приходится обновлять каждую секунду. Прямая
// может идти через лес и реку вброд — стоимость клеток не учитывается.
inline constexpr int kDirectWalkCells = 24;
bool startWalkDirect(World& world, entt::entity entity, Cell goal);

// Продвинуть всех идущих на dt секунд симуляции. Скорость — Walker::speed ×
// Walker::pace × множитель клетки. Шаг может быть длиннее тика:
// за него сущность проходит несколько точек пути (задел под грубую симуляцию вдали).
// Velocity — фактическая скорость за шаг.
void updateMovement(World& world, float dt);

} // namespace lw
