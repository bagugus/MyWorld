#pragma once

// Иерархия дорог (этап 2а.3): классы, их свойства и сеть дорог между
// поселениями. Без графики: сеть читают и симуляция (где встал конвой), и
// рендер (разметка).

#include "sim/Id.h"
#include "sim/Map.h"

#include <optional>
#include <vector>

namespace lw {

// Место дороги в иерархии. Слой карты Map::road хранит покрытие клетки
// (Road), класс — у участка сети: асфальт бывает и трассой, и улицей.
enum class RoadClass : std::uint8_t {
    Track,    // просёлок к деревне: грунт, одна клетка
    Regional, // трасса между районами: асфальт, две клетки
    Highway,  // шоссе между городами: асфальт, три клетки, разметка, мосты
};

// Покрытие, которое класс кладёт на клетки.
Road surfaceOf(RoadClass kind);
// Ширина полотна, клетки.
int widthOf(RoadClass kind);
const char* nameOf(RoadClass kind);
// Название покрытия клетки для интерфейса: «шоссе», «асфальт», «грунтовая».
const char* surfaceName(Road road);

// Громкость шагов по покрытию (множитель): по асфальту шаги звонче, по
// грунту и траве — как раньше.
float footstepLoudness(Road road);

// Во сколько раз медленнее идти по раскисшей земле: грунтовая дорога и пашня
// в дождь — грязь; асфальт не раскисает. wetness 0..1 — насколько мокро
// (mudWetness), 1 — ливень.
float mudSpeedFactor(Road road, Ground ground, float wetness);
// Насколько раскисла земля при такой погоде: осадки, если не мороз.
float mudWetness(float precipitation, bool snowing);

// Участок сети: дорога одного класса между двумя поселениями. Ось — клетки
// пути с шагом в узел сетки дорог (по прямой между соседними); полотно
// шириной width клеток лежит вправо и вниз от каждой клетки оси, поэтому
// середина полотна — клетка + width / 2.
struct RoadLink {
    RoadClass kind = RoadClass::Track;
    EntityId from = kInvalidId; // поселения
    EntityId to = kInvalidId;
    std::vector<Cell> path;
    float lengthMeters = 0.0f;

    // Точка оси в метрах для клетки пути.
    float axisX(Cell cell) const;
    float axisY(Cell cell) const;
};

// Развязка: дорога ниже классом примыкает к дороге выше классом вне
// поселения. Одноуровневая: съезды по-над дорогой однослойная карта не
// выражает.
struct RoadJunction {
    Cell at;           // первая клетка главной дороги на пути примыкающей
    RoadClass major = RoadClass::Highway;
    RoadClass minor = RoadClass::Track;
    // Направление примыкающей дороги к главной (номер шага kDx/kDy, 0..7).
    int heading = 0;
};

// Мост: клетки воды под дорогой подряд вдоль её оси.
struct RoadBridge {
    RoadClass kind = RoadClass::Track;
    Cell first;
    Cell last;
    int cells = 0;
};

// Сеть дорог между поселениями — производные данные генерации: зависит
// только от seed и размера карты, в хеш мира не входит (её клетки — в слое
// Map::road, он в хеше карты).
struct RoadNetwork {
    std::vector<RoadLink> links;
    std::vector<RoadJunction> junctions;
    std::vector<RoadBridge> bridges;

    // Длина участков класса, км (наложившиеся участки считаются дважды).
    float kilometers(RoadClass kind) const;
    int count(RoadClass kind) const;
};

// Класс дороги на клетке по сети: самый высокий из участков, чьё полотно её
// покрывает; nullopt — участков нет (улица или не дорога). Медленно (обход
// всей сети) — для интерфейса и проверок.
std::optional<RoadClass> roadClassAt(const RoadNetwork& network, Cell cell);

} // namespace lw
