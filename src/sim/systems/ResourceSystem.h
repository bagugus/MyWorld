#pragma once

#include "sim/Components.h"

#include <cstdint>

namespace lw {

class World;

// Восстановление ресурсов (sim/Resources.h): дождь наполняет бочки у зданий,
// лес каждую полночь прирастает ягодами и грибами по сезону, а зимой пустеет.
// Рандома нет.

// Сколько порций воды помещается в бочку здания; 0 — здание дождь не собирает.
std::uint16_t rainBarrel(BuildingKind kind);

// Вызывается каждый тик; работает раз в игровую минуту.
void updateResources(World& world);

} // namespace lw
