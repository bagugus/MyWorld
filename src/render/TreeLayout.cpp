#include "render/TreeLayout.h"

#include "render/LandUse.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lw {

namespace {

// Пятна древостоя: порода и возраст меняются плавно, участками в столько клеток.
constexpr int kStandCells = 24;
constexpr int kAgeCells = 12;
// Выше этого (метры) ели заметно больше — как и сам лес в генерации (Terrain.cpp).
constexpr float kUplandHeight = 120.0f;
// У воды ближе стольких клеток — ольха и ива: лиственные.
constexpr int kWaterReach = 2;

// Вероятности на клетку.
constexpr float kTreeChance = 0.9f;      // большое дерево
constexpr float kYoungChance = 0.2f;     // подрост рядом с ним
constexpr float kBushChance = 0.4f;      // куст
constexpr float kEdgeBushChance = 0.7f;  // куст на опушке
constexpr float kSpruceBushChance = 0.2f; // под елью темно

std::uint64_t mixBits(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ULL;
    value ^= value >> 33;
    return value;
}

std::uint64_t cellBits(Cell cell, std::uint64_t salt) {
    return mixBits((static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.x)) << 32) ^
                   static_cast<std::uint32_t>(cell.y) ^ (salt * 0x9E3779B97F4A7C15ULL));
}

// Последовательность случайных чисел 0..1 клетки.
class CellRandom {
public:
    CellRandom(Cell cell, std::uint64_t salt) : state_(cellBits(cell, salt)) {}
    float next() {
        state_ = mixBits(state_ + 0x9E3779B97F4A7C15ULL);
        return static_cast<float>(state_ >> 40) / static_cast<float>(1u << 24);
    }
    std::uint32_t bits() {
        state_ = mixBits(state_ + 0x9E3779B97F4A7C15ULL);
        return static_cast<std::uint32_t>(state_ >> 32);
    }

private:
    std::uint64_t state_;
};

// Плавный шум значений 0..1 с решёткой через period клеток.
float valueNoise(Cell cell, int period, std::uint64_t salt) {
    const float gx = static_cast<float>(cell.x) / static_cast<float>(period);
    const float gy = static_cast<float>(cell.y) / static_cast<float>(period);
    const int ix = static_cast<int>(std::floor(gx));
    const int iy = static_cast<int>(std::floor(gy));
    float u = gx - static_cast<float>(ix);
    float v = gy - static_cast<float>(iy);
    u = u * u * (3.0f - 2.0f * u);
    v = v * v * (3.0f - 2.0f * v);
    const auto at = [salt](int x, int y) {
        return static_cast<float>(cellBits({x, y}, salt) >> 40) / static_cast<float>(1u << 24);
    };
    const float a = at(ix, iy);
    const float b = at(ix + 1, iy);
    const float c = at(ix, iy + 1);
    const float d = at(ix + 1, iy + 1);
    return (a + (b - a) * u) * (1.0f - v) + (c + (d - c) * u) * v;
}

bool isForest(const Map& map, Cell cell) {
    return map.contains(cell) && map.cover.at(cell.x, cell.y) == Cover::Forest;
}

// Опушка: рядом есть клетка без леса.
bool isEdge(const Map& map, Cell cell) {
    constexpr int kDx[] = {1, -1, 0, 0};
    constexpr int kDy[] = {0, 0, 1, -1};
    for (int d = 0; d < 4; ++d) {
        const Cell next = {cell.x + kDx[d], cell.y + kDy[d]};
        if (map.contains(next) && !isForest(map, next)) {
            return true;
        }
    }
    return false;
}

bool nearWater(const Map& map, Cell cell) {
    for (int dy = -kWaterReach; dy <= kWaterReach; ++dy) {
        for (int dx = -kWaterReach; dx <= kWaterReach; ++dx) {
            const Cell next = {cell.x + dx, cell.y + dy};
            if (map.contains(next) && map.water.at(next.x, next.y) != Water::None) {
                return true;
            }
        }
    }
    return false;
}

struct Site {
    bool edge = false;
    bool sand = false;
    bool wet = false;
    float conifer = 0.0f; // вероятность хвойного
    float age = 0.0f;     // 0..1: старше — выше
};

Site siteOf(const Map& map, Cell cell) {
    Site site;
    site.edge = isEdge(map, cell);
    site.sand = map.ground.at(cell.x, cell.y) == Ground::Sand;
    site.wet = nearWater(map, cell);
    const float upland = std::clamp(map.elevationMeters(cell) / kUplandHeight, 0.0f, 1.0f);
    const float stand = valueNoise(cell, kStandCells, 1u);
    float conifer = 0.2f + 0.55f * upland + 1.3f * (stand - 0.5f);
    if (site.wet) {
        conifer *= 0.35f;
    }
    if (site.edge) {
        conifer *= 0.75f;
    }
    site.conifer = site.sand ? 0.95f : std::clamp(conifer, 0.03f, 0.97f);
    site.age = valueNoise(cell, kAgeCells, 2u);
    return site;
}

TreeKind pickKind(const Site& site, CellRandom& random) {
    const float roll = random.next();
    const float second = random.next();
    if (roll < site.conifer) {
        return site.sand || second < 0.3f ? TreeKind::Pine : TreeKind::Spruce;
    }
    // Берёза — первой занимает опушки.
    return second < (site.edge ? 0.55f : 0.3f) ? TreeKind::Birch : TreeKind::Broadleaf;
}

// Рост и радиус кроны: у каждой породы свои пределы.
void sizeTree(TreeSpec& tree, float scale, CellRandom& random) {
    const float u = random.next();
    const float w = random.next();
    switch (tree.kind) {
    case TreeKind::Spruce:
        tree.height = (15.0f + 9.0f * u) * scale;
        tree.crown = tree.height * (0.17f + 0.04f * w);
        break;
    case TreeKind::Pine:
        tree.height = (16.0f + 9.0f * u) * scale;
        tree.crown = tree.height * (0.15f + 0.04f * w);
        break;
    case TreeKind::Broadleaf:
        tree.height = (11.0f + 8.0f * u) * scale;
        tree.crown = tree.height * (0.25f + 0.06f * w);
        break;
    case TreeKind::Birch:
        tree.height = (12.0f + 7.0f * u) * scale;
        tree.crown = tree.height * (0.15f + 0.04f * w);
        break;
    case TreeKind::Bush:
        tree.height = 0.9f + 1.1f * u;
        tree.crown = 0.5f + 0.6f * w;
        break;
    }
}

// Общий разброс: наклон, поворот, время листвы.
void finishTree(TreeSpec& tree, CellRandom& random) {
    // Почти все стоят прямо, редкие клонятся до 5–6°.
    const float u = random.next();
    const float lean = 0.015f + 0.085f * u * u * u;
    const float direction = random.next() * 2.0f * std::numbers::pi_v<float>;
    tree.tiltX = lean * std::cos(direction);
    tree.tiltZ = lean * std::sin(direction);
    tree.yaw = random.next() * 2.0f * std::numbers::pi_v<float>;
    tree.timing = random.next();
    tree.bits = random.bits();
}

} // namespace

int treesInCell(const Map& map, Cell cell, TreeSpec* out) {
    if (!isForest(map, cell) || map.road.at(cell.x, cell.y) != Road::None ||
        map.water.at(cell.x, cell.y) != Water::None) {
        return 0;
    }
    const Site site = siteOf(map, cell);
    CellRandom random(cell, 3u);
    const float x0 = static_cast<float>(cell.x) * Map::kCellSize;
    const float z0 = static_cast<float>(cell.y) * Map::kCellSize;
    // Точка в клетке со сдвигом от центра до spread метров, не ближе 0,3 м к краю.
    const auto place = [&](TreeSpec& tree, float spread) {
        const float half = Map::kCellSize * 0.5f;
        tree.x = x0 + std::clamp(half + (random.next() * 2.0f - 1.0f) * spread, 0.3f, Map::kCellSize - 0.3f);
        tree.z = z0 + std::clamp(half + (random.next() * 2.0f - 1.0f) * spread, 0.3f, Map::kCellSize - 0.3f);
    };
    const float scale = (0.75f + 0.45f * site.age) * (site.edge ? 0.8f : 1.0f);

    int count = 0;
    TreeKind mainKind = TreeKind::Broadleaf;
    bool hasMain = false;
    if (random.next() < kTreeChance) {
        TreeSpec& tree = out[count++];
        tree.kind = pickKind(site, random);
        mainKind = tree.kind;
        hasMain = true;
        place(tree, 1.2f);
        sizeTree(tree, scale, random);
        finishTree(tree, random);
    }
    if (random.next() < kYoungChance || !hasMain) {
        // Подрост — той же породы, что старшие вокруг, ниже и тоньше.
        TreeSpec& tree = out[count++];
        tree.kind = hasMain ? mainKind : pickKind(site, random);
        place(tree, 1.6f);
        sizeTree(tree, scale * (0.3f + 0.3f * random.next()), random);
        finishTree(tree, random);
    }
    const float bushChance = site.edge ? kEdgeBushChance
                             : hasMain && mainKind == TreeKind::Spruce ? kSpruceBushChance
                                                                       : kBushChance;
    if (random.next() < bushChance) {
        TreeSpec& tree = out[count++];
        tree.kind = TreeKind::Bush;
        place(tree, 1.5f);
        sizeTree(tree, 1.0f, random);
        finishTree(tree, random);
    }
    return count;
}

int openTreesInCell(const LandUse& land, Cell cell, TreeSpec* out) {
    const Map& map = land.map();
    if (!map.contains(cell) || map.cover.at(cell.x, cell.y) != Cover::None ||
        map.road.at(cell.x, cell.y) != Road::None || map.water.at(cell.x, cell.y) != Water::None ||
        map.ground.at(cell.x, cell.y) == Ground::Field) {
        return 0;
    }
    const std::uint8_t flags = land.flags(cell);
    if ((flags & (LandUse::kBuilding | LandUse::kGarden | LandUse::kDoor)) != 0) {
        return 0;
    }
    // Вероятности дерева и куста по месту.
    float treeChance = 0.004f;
    float bushChance = 0.015f;
    float scale = 0.8f + 0.2f * valueNoise(cell, kAgeCells, 2u);
    bool fruit = false;
    bool forestEdge = false;
    for (int d = 0; d < 4 && !forestEdge; ++d) {
        constexpr int kDx[] = {1, -1, 0, 0};
        constexpr int kDy[] = {0, 0, 1, -1};
        forestEdge = isForest(map, {cell.x + kDx[d], cell.y + kDy[d]});
    }
    if ((flags & LandUse::kYard) != 0) {
        treeChance = 0.18f; // яблони и груши
        bushChance = 0.1f;  // сирень, смородина
        fruit = true;
    } else if ((flags & LandUse::kSettled) != 0) {
        treeChance = 0.03f;
        bushChance = 0.04f;
    } else if (forestEdge) {
        treeChance = 0.12f;
        bushChance = 0.25f;
        scale *= 0.6f;
    } else if ((flags & LandUse::kShore) != 0) {
        treeChance = 0.08f; // ивы, ольха
        bushChance = 0.15f;
    } else if ((flags & LandUse::kRoadside) != 0) {
        treeChance = 0.015f;
        bushChance = 0.1f;
    } else if (valueNoise(cell, 10, 7u) > 0.78f) {
        treeChance = 0.55f; // рощица посреди луга
        bushChance = 0.3f;
    } else if (map.ground.at(cell.x, cell.y) == Ground::Rock) {
        treeChance = 0.01f;
        bushChance = 0.02f;
    }
    CellRandom random(cell, 9u);
    const bool tree = random.next() < treeChance;
    const bool bush = random.next() < bushChance;
    if (!tree && !bush) {
        return 0;
    }
    const float x0 = static_cast<float>(cell.x) * Map::kCellSize;
    const float z0 = static_cast<float>(cell.y) * Map::kCellSize;
    // Не у самого края клетки: там бывают заборы.
    const auto place = [&](TreeSpec& spec) {
        spec.x = x0 + 0.7f + random.next() * (Map::kCellSize - 1.4f);
        spec.z = z0 + 0.7f + random.next() * (Map::kCellSize - 1.4f);
    };
    int count = 0;
    if (tree) {
        TreeSpec& spec = out[count++];
        if (fruit) {
            spec.kind = TreeKind::Broadleaf;
            place(spec);
            sizeTree(spec, 0.45f + 0.15f * random.next(), random);
            spec.crown *= 1.3f; // плодовое — низкое и раскидистое
        } else {
            Site site = siteOf(map, cell);
            site.edge = true; // на просторе — как на опушке: больше лиственных и берёз
            if ((flags & LandUse::kShore) != 0) {
                site.conifer *= 0.3f;
            }
            spec.kind = pickKind(site, random);
            place(spec);
            sizeTree(spec, scale, random);
            // Одинокое дерево растёт вширь.
            spec.crown *= isConifer(spec.kind) ? 1.1f : 1.25f;
        }
        finishTree(spec, random);
    }
    if (bush) {
        TreeSpec& spec = out[count++];
        spec.kind = TreeKind::Bush;
        place(spec);
        sizeTree(spec, 1.0f, random);
        finishTree(spec, random);
    }
    return count;
}

int cellTrees(const Map& map, const LandUse* land, Cell cell, TreeSpec* out) {
    if (isForest(map, cell)) {
        return treesInCell(map, cell, out);
    }
    return land != nullptr ? openTreesInCell(*land, cell, out) : 0;
}

float deciduousShareOf(const Map& map, Cell cell) {
    TreeSpec trees[kMaxTreesPerCell];
    const int count = treesInCell(map, cell, trees);
    int all = 0;
    int deciduous = 0;
    for (int i = 0; i < count; ++i) {
        if (trees[i].kind != TreeKind::Bush) {
            ++all;
            deciduous += isConifer(trees[i].kind) ? 0 : 1;
        }
    }
    return all > 0 ? static_cast<float>(deciduous) / static_cast<float>(all) : 0.0f;
}

Foliage foliageAt(float year, float timing) {
    // Распускается в первые дни весны, желтеет и облетает во второй трети осени
    // (весна — 0..0,25 года, осень — 0,5..0,75); к зиме голые.
    const float onset = 0.02f + 0.08f * timing;
    const float grow = std::clamp((year - onset) / 0.05f, 0.0f, 1.0f);
    const float turnStart = 0.53f + 0.08f * timing;
    Foliage foliage;
    foliage.turn = std::clamp((year - turnStart) / 0.06f, 0.0f, 1.0f);
    const float fall = std::clamp((year - turnStart - 0.05f) / 0.07f, 0.0f, 1.0f);
    foliage.leaves = grow * (1.0f - fall);
    foliage.fresh = 1.0f - std::clamp((year - onset - 0.05f) / 0.12f, 0.0f, 1.0f);
    return foliage;
}

Foliage averageFoliageAt(float year) {
    constexpr int kSamples = 32;
    Foliage sum;
    for (int i = 0; i < kSamples; ++i) {
        const Foliage one = foliageAt(year, (static_cast<float>(i) + 0.5f) / kSamples);
        sum.leaves += one.leaves;
        sum.turn += one.turn;
        sum.fresh += one.fresh;
    }
    sum.leaves /= kSamples;
    sum.turn /= kSamples;
    sum.fresh /= kSamples;
    return sum;
}

} // namespace lw
