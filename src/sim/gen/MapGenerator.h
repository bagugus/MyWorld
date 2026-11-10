#pragma once

#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Roads.h"

#include <string>
#include <vector>

namespace lw {

class Profiler;
class Rng;

// Поселения и здания — сущности, а не клетки карты. Генератор о реестре
// не знает: он возвращает описания, сущности из них создаёт World.

struct SettlementSite {
    SettlementKind kind = SettlementKind::Village;
    std::string name;
    Cell center;
    int radius = 0; // клетки
};

struct BuildingSite {
    BuildingKind kind = BuildingKind::House;
    Cell origin;
    int width = 0;
    int height = 0;
    Cell door;
    int settlement = 0; // индекс в GeneratedMap::settlements
};

// Участок дороги между поселениями (этап 2а.3); from и to — индексы в
// GeneratedMap::settlements.
struct RoadSite {
    RoadClass kind = RoadClass::Track;
    int from = 0;
    int to = 0;
    std::vector<Cell> path;
    float lengthMeters = 0.0f;
};

struct GeneratedMap {
    std::vector<SettlementSite> settlements;
    std::vector<BuildingSite> buildings;
    std::vector<RoadSite> roads;
    std::vector<RoadJunction> junctions;
    std::vector<RoadBridge> bridges;
};

// Заполняет все слои карты и размещает поселения, дороги и дома.
// Результат зависит только от размера карты и состояния rng.
// Шаги и их порядок описаны в docs/architecture.md. profiler — куда записать
// время шагов (этап 2а.1), на результат не влияет.
GeneratedMap generateMap(Map& map, Rng& rng, Profiler* profiler = nullptr);

} // namespace lw
