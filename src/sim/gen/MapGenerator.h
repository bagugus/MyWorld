#pragma once

#include "sim/Components.h"
#include "sim/Map.h"

#include <string>
#include <vector>

namespace lw {

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

struct GeneratedMap {
    std::vector<SettlementSite> settlements;
    std::vector<BuildingSite> buildings;
};

// Заполняет все слои карты и размещает поселения, дороги и дома.
// Результат зависит только от размера карты и состояния rng.
// Шаги и их порядок описаны в docs/architecture.md.
GeneratedMap generateMap(Map& map, Rng& rng);

} // namespace lw
