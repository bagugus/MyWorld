#pragma once

#include "sim/Id.h"
#include "sim/Map.h"

#include <cstdint>
#include <string>

namespace lw {

// Единица длины мира — метр, времени — секунда.

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

// Метры в секунду.
struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
};

// Значения перечислений попадают в хеш мира и в сохранения:
// новые добавлять только в конец.
enum class SettlementKind : std::uint8_t { Village, Town };
enum class BuildingKind : std::uint8_t { House, Barn, Apartment, Shop };

// Поселение: круг клеток вокруг центра. Центр всегда лежит на дороге.
struct Settlement {
    SettlementKind kind = SettlementKind::Village;
    std::string name;
    Cell center;
    int radius = 0; // клетки
};

// Здание занимает прямоугольник клеток [origin, origin + size).
struct Building {
    BuildingKind kind = BuildingKind::House;
    Cell origin;
    int width = 0;  // клетки по x
    int height = 0; // клетки по y
    // Клетка перед входом: снаружи, вплотную к зданию, со стороны дороги.
    Cell door;
    EntityId settlement = kInvalidId;
};

} // namespace lw
