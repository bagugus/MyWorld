#pragma once

#include "sim/Id.h"
#include "sim/Map.h"

#include <cstdint>
#include <string>
#include <vector>

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

// Человек. Дом — жилое здание, у двери которого он ночует; kInvalidId — бездомный.
struct Person {
    EntityId home = kInvalidId;
    // Спит: нужды меняются медленнее, усталость уходит.
    bool asleep = false;
};

// Нужды человека (NeedsSystem). Голод, жажда, усталость, страх, одиночество —
// от 0 (всё хорошо) до 1 (предел): на пределе голода, жажды и усталости уходит
// здоровье. Здоровье — от 1 (здоров) до 0.
struct Needs {
    float hunger = 0.0f;
    float thirst = 0.0f;
    float fatigue = 0.0f;
    float fear = 0.0f;
    float loneliness = 0.0f;
    float health = 1.0f;
};

// Движение пешком по пути (MovementSystem).
struct Walker {
    // Скорость по дороге, м/с; по другой местности медленнее (walkSpeedFactor).
    float speed = 1.4f;
    // Сглаженный путь из Pathfinder: точки — центры клеток. Пусто — стоит на месте.
    std::vector<Cell> waypoints;
    // Номер точки, к которой идёт.
    std::uint32_t next = 0;

    bool moving() const { return next < waypoints.size(); }
};

} // namespace lw
