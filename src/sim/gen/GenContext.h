#pragma once

// Внутренний заголовок генератора карты: общее состояние шагов.

#include "sim/Grid.h"
#include "sim/Map.h"
#include "sim/Rng.h"
#include "sim/gen/MapGenerator.h"

#include <cstddef>
#include <cstdint>

namespace lw::gen {

// Соседи клетки: сначала четыре по сторонам, затем четыре по диагонали.
inline constexpr int kDx[8] = {1, 0, -1, 0, 1, -1, -1, 1};
inline constexpr int kDy[8] = {0, 1, 0, -1, 1, 1, -1, -1};

inline constexpr std::uint16_t kFar = 0xFFFF;

// Шаг сетки узлов, по которой прокладываются дороги между поселениями, в
// клетках. Узел (i, j) — клетка (i * kRoadStep, j * kRoadStep) и блок клеток
// от неё вправо и вниз. Центры поселений обязаны попадать в узлы.
inline constexpr int kRoadStep = 4;

struct Context {
    Context(Map& targetMap, Rng& random, GeneratedMap& result)
        : map(targetMap),
          rng(random),
          out(result),
          width(targetMap.width()),
          height(targetMap.height()),
          slope(width, height),
          mainland(width, height),
          seaDistance(width, height, kFar),
          freshDistance(width, height, kFar),
          occupied(width, height) {}

    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(x);
    }

    Map& map;
    Rng& rng;
    GeneratedMap& out;
    int width;
    int height;

    // Уклон рельефа в процентах (не больше 255), до того как реки врезаны в русла.
    Grid<std::uint8_t> slope;
    // 1 — клетка главной суши: самого большого связного набора блоков
    // kRoadStep × kRoadStep без моря и озёр. Реки в неё входят: их переходят
    // вброд и по мостам. Поселения ставятся только здесь, поэтому дорога
    // между любыми двумя существует.
    Grid<std::uint8_t> mainland;
    // Расстояние в клетках до моря и до пресной воды (озера, реки); kFar — воды нет.
    Grid<std::uint16_t> seaDistance;
    Grid<std::uint16_t> freshDistance;
    // 1 — клетка занята зданием.
    Grid<std::uint8_t> occupied;
};

// Шаги генерации в порядке выполнения.
void generateElevation(Context& ctx);
void generateWater(Context& ctx);
void generateGround(Context& ctx);
void placeSettlements(Context& ctx);
void generateVegetation(Context& ctx);
void buildRoads(Context& ctx);
void buildStreets(Context& ctx);
void placeBuildings(Context& ctx);
void clearRoadsides(Context& ctx);

// Кладёт дорогу на клетку: покрытие не ухудшает, лес убирает.
inline void setRoad(Map& map, int x, int y, Road type) {
    Road& road = map.road.at(x, y);
    if (type > road) {
        road = type;
    }
    map.cover.at(x, y) = Cover::None;
}

inline bool isOpenWater(Water water) {
    return water == Water::Sea || water == Water::Lake;
}

} // namespace lw::gen
