#pragma once

#include "sim/Map.h"

#include <cstdint>

namespace lw {

class LandUse;

// Деревья леса в 3D-виде (этап 11.6): какие деревья стоят в клетке леса, какого
// роста и как наклонены. Только облик — симуляция о деревьях не знает: для неё
// лес — клетки Cover::Forest (обзор, скорость, ягоды). Раскладка — чистая
// функция карты и номера клетки, без рандома мира: одна карта — одни деревья.
//
// Порода — по местности: в нагорье и пятнами — ели, на песке — сосны, у воды и
// на опушках — лиственные и берёзы. Деревья стоят густо, почти в каждой клетке,
// а под ними — кусты и подрост: сквозь лес видно на несколько десятков метров,
// как в симуляции (kForestSightPenalty, sim/Perception.h).

enum class TreeKind : std::uint8_t { Spruce, Pine, Broadleaf, Birch, Bush };

inline bool isConifer(TreeKind kind) {
    return kind == TreeKind::Spruce || kind == TreeKind::Pine;
}

struct TreeSpec {
    TreeKind kind = TreeKind::Spruce;
    float x = 0.0f; // корень в мире, метры (x — восток, z — юг)
    float z = 0.0f;
    float height = 0.0f; // метры
    float crown = 0.0f;  // радиус кроны, метры
    // Наклон: на сколько метров по x и z уходит ствол на метр высоты.
    float tiltX = 0.0f;
    float tiltZ = 0.0f;
    float yaw = 0.0f;       // поворот модели, радианы
    float timing = 0.0f;    // 0..1: когда распускается и желтеет — у каждого дерева своё
    std::uint32_t bits = 0; // остальной разброс облика
};

inline constexpr int kMaxTreesPerCell = 3;

// Деревья и кусты клетки (от нуля до kMaxTreesPerCell) в out; у клеток без
// леса и у дорог через лес — ни одного.
int treesInCell(const Map& map, Cell cell, TreeSpec* out);

// Деревья и кусты вне леса (этап 11.9, только облик): одиночные деревья на
// лугах, рощицы, кусты у дорог, у воды и на опушках, плодовые деревья и сирень
// во дворах (дворы — render/LandUse). На полях, дорогах, под зданиями, в
// огородах и перед входом — ни одного. Не больше kMaxTreesPerCell.
int openTreesInCell(const LandUse& land, Cell cell, TreeSpec* out);
// Все деревья клетки: лесные или, если land задан, вне леса.
int cellTrees(const Map& map, const LandUse* land, Cell cell, TreeSpec* out);

// Доля лиственных среди деревьев клетки, 0..1: дальний лес — текстурой земли —
// желтеет осенью и сереет зимой так же, как деревья вблизи.
float deciduousShareOf(const Map& map, Cell cell);

// Листва по времени года (доля года с 1 марта, как CalendarTime::yearFraction).
// timing — TreeSpec::timing. Те же формулы — в шейдере деревьев (render/Trees3D).
struct Foliage {
    float leaves = 0.0f; // сколько листвы: 0 — голые ветви, 1 — полная крона
    float turn = 0.0f;   // насколько пожелтела: 0 — зелёная, 1 — осенний цвет
    float fresh = 0.0f;  // молодая весенняя листва: светлее
};
Foliage foliageAt(float yearFraction, float timing);
// Среднее по всем лиственным деревьям: для текстуры леса на земле.
Foliage averageFoliageAt(float yearFraction);

} // namespace lw
