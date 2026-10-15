// Природная часть карты: рельеф, вода, поверхность, растительность.

#include "sim/gen/GenContext.h"

#include <FastNoiseLite.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace lw::gen {

namespace {

constexpr float kCell = Map::kCellSize;
constexpr float kCellArea = kCell * kCell;

// --- Рельеф -----------------------------------------------------------------
// Материк задаётся величиной c: ноль на берегу, растёт вглубь суши, падает в
// море. Через карту она меняется на два (от -1 до 1 вдоль направления к морю).
constexpr float kCoastline = 0.35f;      // сдвиг берега: при 0 суша и море делят карту пополам
constexpr float kCoastWarp = 0.2f;       // изрезанность берега (в единицах c)
constexpr float kCoastWavelength = 3200.0f; // метры
constexpr float kSeaSlope = 120.0f;      // метров глубины на единицу c
constexpr float kPlainRise = 15.0f;      // метров высоты на единицу c
constexpr float kUplandHeight = 160.0f;  // высота нагорья вдали от берега, метры
constexpr float kUplandAt = 1.1f;        // c, на котором нагорье набирает полную высоту
constexpr float kHillWavelength = 1800.0f;
constexpr float kShoreRelief = 2.5f;     // размах холмов у берега, метры
constexpr float kHillRelief = 75.0f;     // прибавка к размаху вдали от берега, метры
constexpr float kHillsFullAt = 0.5f;     // c, на котором холмы набирают полный размах

// --- Вода -------------------------------------------------------------------
constexpr std::uint8_t kNoFlow = 0xFF;
constexpr float kMinLakeBasin = 150000.0f; // м²: в меньшей впадине озера нет
constexpr float kMaxLakeArea = 90000.0f;   // м²
constexpr int kMinLakeDepth = 15;          // глубина впадины, единицы kElevationStep
constexpr int kLakeShoreDepth = 3;         // глубина озера у берега, единицы kDepthStep

// Водоток появляется там, где на клетку стекает вода с площади minBasin.
struct RiverClass {
    float minBasin;     // м²
    int width;          // клетки
    std::uint8_t depth; // единицы kDepthStep
};
// От крупных к мелким. Ручей (0,4 м) переходят вброд, реку — вплавь или по мосту.
constexpr RiverClass kRiverClasses[] = {
    {12.0e6f, 3, 25},
    {3.5e6f, 2, 12},
    {0.6e6f, 1, 4},
};

// --- Поверхность и растительность -------------------------------------------
constexpr int kRockSlope = 45;   // проценты
constexpr int kBeachWidth = 6;   // клетки от моря
constexpr int kBeachHeight = 20; // единицы kElevationStep
constexpr int kClearing = 5;     // клетки без леса и полей вокруг поселения
// Граница вырубки в единицах слоя близости и размах её неровности: лес
// начинается на расстоянии от 0,6 до 1,4 радиуса вырубки.
constexpr int kClearingEdge = 100;
constexpr int kClearingRagged = 40;
constexpr float kClearingWavelength = 350.0f; // метры
constexpr int kFieldBelt = 110;  // ширина пояса полей вокруг деревни, клетки
constexpr int kFieldMaxSlope = 9;
constexpr float kFieldPatch = 150.0f;     // размер одного поля, метры
constexpr float kFieldThreshold = -0.15f; // доля распаханных участков: чем ниже, тем больше
constexpr float kForestWavelength = 1100.0f;
constexpr int kForestMaxSlope = 40;
constexpr float kForestBias = -0.1f;          // меньше — реже лес
constexpr float kForestUplandBias = 0.12f;    // в нагорье леса больше
constexpr float kForestUplandHeight = 120.0f; // метры

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Фрактальный шум со значениями около [-1, 1]. Координаты — метры мира.
FastNoiseLite makeFbm(Rng& rng, float wavelength, int octaves) {
    FastNoiseLite noise(static_cast<int>(rng.nextU32()));
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(octaves);
    noise.SetFrequency(1.0f / wavelength);
    return noise;
}

// Море — всё, что ниже нуля и соединено с краем карты.
void floodSea(Context& ctx) {
    Map& map = ctx.map;
    std::vector<Cell> queue;
    const auto visit = [&](int x, int y) {
        if (map.elevation.at(x, y) < 0 && map.water.at(x, y) == Water::None) {
            map.water.at(x, y) = Water::Sea;
            queue.push_back({x, y});
        }
    };
    for (int x = 0; x < ctx.width; ++x) {
        visit(x, 0);
        visit(x, ctx.height - 1);
    }
    for (int y = 0; y < ctx.height; ++y) {
        visit(0, y);
        visit(ctx.width - 1, y);
    }
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Cell cell = queue[i];
        for (int d = 0; d < 4; ++d) {
            const Cell next{cell.x + kDx[d], cell.y + kDy[d]};
            if (map.contains(next)) {
                visit(next.x, next.y);
            }
        }
    }
    for (const Cell cell : queue) {
        const int depth = -map.elevation.at(cell.x, cell.y);
        map.depth.at(cell.x, cell.y) = static_cast<std::uint8_t>(std::min(depth, 255));
    }
}

// Сток воды по суше.
struct Drainage {
    // Уровень воды в клетке, если залить все впадины до краёв.
    std::vector<std::int16_t> level;
    // Номер соседа (kDx, kDy), в которого стекает вода; kNoFlow у стоков.
    std::vector<std::uint8_t> flowTo;
    // Клетки от стоков вверх по течению: сосед-приёмник всегда раньше.
    std::vector<std::uint32_t> order;
};

// Priority-flood: вода «поднимается» от стоков (берег моря, край карты), и
// каждая клетка запоминает, откуда до неё добрались, — туда она и стекает.
// Так у любой клетки есть путь до стока, даже из впадины. Высоты целые,
// поэтому вместо общей кучи — по списку клеток на каждый уровень.
// Внутри впадины (все её клетки залиты до одного уровня) первыми идут самые
// низкие: путь воды через впадину повторяет её естественное русло.
Drainage computeDrainage(const Context& ctx) {
    const Map& map = ctx.map;
    const std::vector<std::int16_t>& elevation = map.elevation.cells();
    const std::size_t count = elevation.size();

    Drainage drainage;
    drainage.level.assign(count, 0);
    drainage.flowTo.assign(count, kNoFlow);
    drainage.order.reserve(count);

    const int maxLevel = std::max<int>(0, *std::max_element(elevation.begin(), elevation.end()));
    std::vector<std::vector<std::uint32_t>> buckets(static_cast<std::size_t>(maxLevel) + 1);
    std::vector<std::uint8_t> closed(count, 0);

    for (int y = 0; y < ctx.height; ++y) {
        for (int x = 0; x < ctx.width; ++x) {
            const std::size_t i = ctx.index(x, y);
            if (map.water.at(x, y) == Water::Sea) {
                closed[i] = 1;
                bool shore = false;
                for (int d = 0; d < 8 && !shore; ++d) {
                    const Cell next{x + kDx[d], y + kDy[d]};
                    shore = map.contains(next) && map.water.at(next.x, next.y) != Water::Sea;
                }
                if (shore) {
                    buckets[0].push_back(static_cast<std::uint32_t>(i));
                }
            } else if (x == 0 || y == 0 || x == ctx.width - 1 || y == ctx.height - 1) {
                // Суша на краю карты: вода уходит за край.
                closed[i] = 1;
                const int level = std::max<int>(elevation[i], 0);
                drainage.level[i] = static_cast<std::int16_t>(level);
                buckets[static_cast<std::size_t>(level)].push_back(static_cast<std::uint32_t>(i));
            }
        }
    }

    // Клетки впадин текущего уровня: (высота дна, номер клетки). Номер клетки
    // решает спор при равной высоте, так что порядок однозначен.
    using PitCell = std::pair<std::int16_t, std::uint32_t>;
    std::priority_queue<PitCell, std::vector<PitCell>, std::greater<PitCell>> pit;

    for (int level = 0; level <= maxLevel; ++level) {
        // В список текущего уровня клетки добавляются и во время обхода.
        std::vector<std::uint32_t>& bucket = buckets[static_cast<std::size_t>(level)];
        std::size_t taken = 0;
        for (;;) {
            std::uint32_t i = 0;
            if (!pit.empty()) {
                i = pit.top().second;
                pit.pop();
            } else if (taken < bucket.size()) {
                i = bucket[taken++];
            } else {
                break;
            }
            drainage.order.push_back(i);
            const int x = static_cast<int>(i % static_cast<std::uint32_t>(ctx.width));
            const int y = static_cast<int>(i / static_cast<std::uint32_t>(ctx.width));
            for (int d = 0; d < 8; ++d) {
                const Cell next{x + kDx[d], y + kDy[d]};
                if (!map.contains(next)) {
                    continue;
                }
                const std::size_t n = ctx.index(next.x, next.y);
                if (closed[n]) {
                    continue;
                }
                closed[n] = 1;
                // Обратное направление: от соседа к текущей клетке.
                drainage.flowTo[n] = static_cast<std::uint8_t>(d < 4 ? (d + 2) % 4 : 4 + (d - 2) % 4);
                if (elevation[n] < level) {
                    drainage.level[n] = static_cast<std::int16_t>(level);
                    pit.emplace(elevation[n], static_cast<std::uint32_t>(n));
                } else {
                    drainage.level[n] = elevation[n];
                    buckets[static_cast<std::size_t>(elevation[n])].push_back(
                        static_cast<std::uint32_t>(n));
                }
            }
        }
        bucket = {};
    }
    return drainage;
}

// Впадины засыпаются до уровня перелива и становятся ровными долинами —
// иначе река выбиралась бы из ямы вверх по склону. В самой глубокой части
// большой впадины остаётся озеро.
void fillDepressions(Context& ctx, const Drainage& drainage) {
    Map& map = ctx.map;
    std::vector<std::int16_t>& elevation = map.elevation.cells();
    const std::size_t count = elevation.size();
    const auto minBasinCells = static_cast<std::size_t>(kMinLakeBasin / kCellArea);
    const auto maxLakeCells = static_cast<std::size_t>(kMaxLakeArea / kCellArea);

    const auto fillAt = [&](std::size_t i) { return drainage.level[i] - elevation[i]; };

    std::vector<std::uint8_t> seen(count, 0);
    std::vector<std::uint32_t> basin;
    std::vector<int> fills;
    for (std::size_t start = 0; start < count; ++start) {
        if (seen[start] || map.water.cells()[start] == Water::Sea || fillAt(start) <= 0) {
            continue;
        }
        basin.clear();
        basin.push_back(static_cast<std::uint32_t>(start));
        seen[start] = 1;
        int deepest = 0;
        for (std::size_t k = 0; k < basin.size(); ++k) {
            const std::uint32_t i = basin[k];
            deepest = std::max(deepest, fillAt(i));
            const int x = static_cast<int>(i % static_cast<std::uint32_t>(ctx.width));
            const int y = static_cast<int>(i / static_cast<std::uint32_t>(ctx.width));
            for (int d = 0; d < 4; ++d) {
                const Cell next{x + kDx[d], y + kDy[d]};
                if (!map.contains(next)) {
                    continue;
                }
                const std::size_t n = ctx.index(next.x, next.y);
                if (!seen[n] && map.water.cells()[n] != Water::Sea && fillAt(n) > 0) {
                    seen[n] = 1;
                    basin.push_back(static_cast<std::uint32_t>(n));
                }
            }
        }

        // Озеро занимает клетки, залитые глубже порога; порог подобран так,
        // чтобы озеро не вышло больше kMaxLakeArea.
        int shore = deepest + 1; // озера нет
        if (basin.size() >= minBasinCells && deepest >= kMinLakeDepth) {
            shore = kLakeShoreDepth;
            if (basin.size() > maxLakeCells) {
                fills.clear();
                for (const std::uint32_t i : basin) {
                    fills.push_back(fillAt(i));
                }
                const auto nth = fills.begin() + static_cast<std::ptrdiff_t>(maxLakeCells);
                std::nth_element(fills.begin(), nth, fills.end(), std::greater<int>());
                shore = std::max(shore, *nth);
            }
        }
        for (const std::uint32_t i : basin) {
            const int fill = fillAt(i);
            if (fill < shore) {
                elevation[i] = drainage.level[i];
                continue;
            }
            // Глубина растёт от берега; слой depth вмещает не больше 25,5 м.
            const int depth = std::min(fill - shore + kLakeShoreDepth, 255);
            elevation[i] = static_cast<std::int16_t>(drainage.level[i] - depth);
            map.water.cells()[i] = Water::Lake;
            map.depth.cells()[i] = static_cast<std::uint8_t>(depth);
        }
    }
}

void computeSlope(Context& ctx) {
    const Map& map = ctx.map;
    for (int y = 0; y < ctx.height; ++y) {
        const int y0 = std::max(y - 1, 0);
        const int y1 = std::min(y + 1, ctx.height - 1);
        for (int x = 0; x < ctx.width; ++x) {
            const int x0 = std::max(x - 1, 0);
            const int x1 = std::min(x + 1, ctx.width - 1);
            float dzdx = 0.0f;
            float dzdy = 0.0f;
            if (x1 > x0) {
                dzdx = static_cast<float>(map.elevation.at(x1, y) - map.elevation.at(x0, y)) *
                       Map::kElevationStep / (static_cast<float>(x1 - x0) * kCell);
            }
            if (y1 > y0) {
                dzdy = static_cast<float>(map.elevation.at(x, y1) - map.elevation.at(x, y0)) *
                       Map::kElevationStep / (static_cast<float>(y1 - y0) * kCell);
            }
            const float percent = std::sqrt(dzdx * dzdx + dzdy * dzdy) * 100.0f;
            ctx.slope.at(x, y) = static_cast<std::uint8_t>(std::min(percent, 255.0f));
        }
    }
}

// Реки: клетки, через которые стекает вода с большой площади.
void carveRivers(Context& ctx, const Drainage& drainage) {
    Map& map = ctx.map;
    const std::size_t count = drainage.level.size();

    // Площадь водосбора в клетках: обходим от верховий к стокам.
    std::vector<std::uint32_t> basin(count, 1);
    for (auto it = drainage.order.rbegin(); it != drainage.order.rend(); ++it) {
        const std::uint32_t i = *it;
        const std::uint8_t d = drainage.flowTo[i];
        if (d != kNoFlow) {
            const std::size_t target = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(i) + kDy[d] * ctx.width + kDx[d]);
            basin[target] += basin[i];
        }
    }

    for (const std::uint32_t i : drainage.order) {
        if (isOpenWater(map.water.cells()[i])) {
            continue;
        }
        const float area = static_cast<float>(basin[i]) * kCellArea;
        const RiverClass* river = nullptr;
        for (const RiverClass& candidate : kRiverClasses) {
            if (area >= candidate.minBasin) {
                river = &candidate;
                break;
            }
        }
        if (river == nullptr) {
            continue;
        }

        const int x = static_cast<int>(i % static_cast<std::uint32_t>(ctx.width));
        const int y = static_cast<int>(i / static_cast<std::uint32_t>(ctx.width));
        const int surface = drainage.level[i];
        const auto wet = [&](int cx, int cy) {
            if (!map.contains({cx, cy}) || isOpenWater(map.water.at(cx, cy))) {
                return;
            }
            map.water.at(cx, cy) = Water::River;
            map.depth.at(cx, cy) = std::max(map.depth.at(cx, cy), river->depth);
            // Русло врезается в рельеф на глубину реки.
            const int bed = std::min<int>(map.elevation.at(cx, cy), surface - river->depth);
            map.elevation.at(cx, cy) = static_cast<std::int16_t>(bed);
        };

        const int from = river->width == 3 ? -1 : 0;
        const int to = river->width == 1 ? 0 : 1;
        for (int dy = from; dy <= to; ++dy) {
            for (int dx = from; dx <= to; ++dx) {
                wet(x + dx, y + dy);
            }
        }

        // Ручей шириной в клетку, текущий по диагонали, достраиваем уголком:
        // иначе его можно было бы перейти, не замочив ног.
        const std::uint8_t d = drainage.flowTo[i];
        if (river->width == 1 && d != kNoFlow && d >= 4) {
            const Cell a{x + kDx[d], y};
            const Cell b{x, y + kDy[d]};
            const bool useA = drainage.level[ctx.index(a.x, a.y)] <= drainage.level[ctx.index(b.x, b.y)];
            wet(useA ? a.x : b.x, useA ? a.y : b.y);
        }
    }
}

// Главная суша: самый большой связный набор блоков kRoadStep × kRoadStep без
// моря и озёр (реки не мешают). Считается по блокам, а не по клеткам, потому
// что по тем же блокам прокладываются дороги: между любыми двумя точками
// главной суши дорога есть.
void findMainland(Context& ctx) {
    const Map& map = ctx.map;
    const int blocksX = ctx.width / kRoadStep;
    const int blocksY = ctx.height / kRoadStep;
    if (blocksX == 0 || blocksY == 0) {
        return;
    }

    Grid<std::int32_t> label(blocksX, blocksY, -1); // -2 — в блоке есть вода
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            for (int y = by * kRoadStep; y < (by + 1) * kRoadStep; ++y) {
                for (int x = bx * kRoadStep; x < (bx + 1) * kRoadStep; ++x) {
                    if (isOpenWater(map.water.at(x, y))) {
                        label.at(bx, by) = -2;
                    }
                }
            }
        }
    }

    std::vector<Cell> queue;
    std::int32_t labels = 0;
    std::int32_t best = -1;
    std::size_t bestSize = 0;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (label.at(bx, by) != -1) {
                continue;
            }
            queue.clear();
            queue.push_back({bx, by});
            label.at(bx, by) = labels;
            for (std::size_t k = 0; k < queue.size(); ++k) {
                const Cell block = queue[k];
                for (int d = 0; d < 4; ++d) {
                    const Cell next{block.x + kDx[d], block.y + kDy[d]};
                    if (label.contains(next.x, next.y) && label.at(next.x, next.y) == -1) {
                        label.at(next.x, next.y) = labels;
                        queue.push_back(next);
                    }
                }
            }
            if (queue.size() > bestSize) {
                bestSize = queue.size();
                best = labels;
            }
            ++labels;
        }
    }

    for (int y = 0; y < blocksY * kRoadStep; ++y) {
        for (int x = 0; x < blocksX * kRoadStep; ++x) {
            ctx.mainland.at(x, y) = label.at(x / kRoadStep, y / kRoadStep) == best ? 1 : 0;
        }
    }
}

// Расстояние в клетках (по сторонам) до ближайшей клетки, для которой isSource истинно.
template <typename IsSource>
void measureDistance(const Context& ctx, Grid<std::uint16_t>& distance, IsSource isSource) {
    std::vector<Cell> queue;
    for (int y = 0; y < ctx.height; ++y) {
        for (int x = 0; x < ctx.width; ++x) {
            if (isSource(ctx.map.water.at(x, y))) {
                distance.at(x, y) = 0;
                queue.push_back({x, y});
            }
        }
    }
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Cell cell = queue[i];
        const auto next = static_cast<std::uint16_t>(distance.at(cell.x, cell.y) + 1);
        for (int d = 0; d < 4; ++d) {
            const Cell neighbor{cell.x + kDx[d], cell.y + kDy[d]};
            if (ctx.map.contains(neighbor) && distance.at(neighbor.x, neighbor.y) == kFar) {
                distance.at(neighbor.x, neighbor.y) = next;
                queue.push_back(neighbor);
            }
        }
    }
}

} // namespace

void generateElevation(Context& ctx) {
    Map& map = ctx.map;
    Rng& rng = ctx.rng;

    // Направление к морю — случайная точка в круге, без sin/cos.
    float seaX = 0.0f;
    float seaY = 0.0f;
    float lengthSq = 0.0f;
    do {
        seaX = rng.range(-1.0f, 1.0f);
        seaY = rng.range(-1.0f, 1.0f);
        lengthSq = seaX * seaX + seaY * seaY;
    } while (lengthSq > 1.0f || lengthSq < 0.01f);
    const float length = std::sqrt(lengthSq);
    seaX /= length;
    seaY /= length;
    // Половина размаха проекции карты на это направление: после деления на
    // неё координата вдоль направления лежит в [-1, 1].
    const float extent = 0.5f * (std::abs(seaX) + std::abs(seaY));

    const FastNoiseLite coast = makeFbm(rng, kCoastWavelength, 3);
    const FastNoiseLite hills = makeFbm(rng, kHillWavelength, 5);

    for (int y = 0; y < ctx.height; ++y) {
        const float wy = Map::cellCenter(y);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(ctx.height) - 0.5f;
        for (int x = 0; x < ctx.width; ++x) {
            const float wx = Map::cellCenter(x);
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(ctx.width) - 0.5f;

            const float toSea = (u * seaX + v * seaY) / extent;
            const float c = kCoastline - toSea + kCoastWarp * coast.GetNoise(wx, wy);
            const float relief = hills.GetNoise(wx, wy);

            float meters = 0.0f;
            if (c < 0.0f) {
                meters = c * kSeaSlope + relief * kShoreRelief;
            } else {
                // У берега равнина, дальше нагорье с холмами.
                meters = c * kPlainRise + kUplandHeight * smoothstep(c / kUplandAt) +
                         relief * (kShoreRelief + kHillRelief * smoothstep(c / kHillsFullAt));
            }
            const float units = std::round(meters / Map::kElevationStep);
            map.elevation.at(x, y) = static_cast<std::int16_t>(std::clamp(units, -30000.0f, 30000.0f));
        }
    }
}

void generateWater(Context& ctx) {
    floodSea(ctx);
    const Drainage drainage = computeDrainage(ctx);
    fillDepressions(ctx, drainage);
    // Уклон считаем до рек: врезанные русла не должны выглядеть обрывами.
    computeSlope(ctx);
    carveRivers(ctx, drainage);

    findMainland(ctx);
    measureDistance(ctx, ctx.seaDistance, [](Water water) { return water == Water::Sea; });
    measureDistance(ctx, ctx.freshDistance,
                    [](Water water) { return water == Water::Lake || water == Water::River; });
}

void generateGround(Context& ctx) {
    Map& map = ctx.map;
    for (int y = 0; y < ctx.height; ++y) {
        for (int x = 0; x < ctx.width; ++x) {
            Ground ground = Ground::Grass;
            if (map.water.at(x, y) != Water::None) {
                ground = Ground::Sand;
            } else if (ctx.slope.at(x, y) >= kRockSlope) {
                ground = Ground::Rock;
            } else if (ctx.seaDistance.at(x, y) <= kBeachWidth &&
                       map.elevation.at(x, y) <= kBeachHeight) {
                ground = Ground::Sand;
            }
            map.ground.at(x, y) = ground;
        }
    }
}

void generateVegetation(Context& ctx) {
    Map& map = ctx.map;

    // Близость к поселению: расстояние до центра в долях радиуса вырубки,
    // kClearingEdge — ровно на её границе, 255 — далеко.
    Grid<std::uint8_t> proximity(ctx.width, ctx.height, 255);
    const auto forEachAround = [&](const SettlementSite& site, int radius, auto&& visit) {
        const int y0 = std::max(site.center.y - radius, 0);
        const int y1 = std::min(site.center.y + radius, ctx.height - 1);
        const int x0 = std::max(site.center.x - radius, 0);
        const int x1 = std::min(site.center.x + radius, ctx.width - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const int dx = x - site.center.x;
                const int dy = y - site.center.y;
                if (dx * dx + dy * dy <= radius * radius) {
                    visit(x, y, dx * dx + dy * dy);
                }
            }
        }
    };
    for (const SettlementSite& site : ctx.out.settlements) {
        const int clearing = site.radius + kClearing;
        const int reach = clearing * (kClearingEdge + kClearingRagged) / kClearingEdge;
        forEachAround(site, reach, [&](int x, int y, int distanceSq) {
            const float share = std::sqrt(static_cast<float>(distanceSq)) / static_cast<float>(clearing);
            const auto value = static_cast<std::uint8_t>(share * static_cast<float>(kClearingEdge));
            proximity.at(x, y) = std::min(proximity.at(x, y), value);
        });
    }

    // Поля — лоскуты вокруг деревень: ячеистый шум даёт участки с прямыми
    // границами, часть из них распахана.
    FastNoiseLite patches(static_cast<int>(ctx.rng.nextU32()));
    patches.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
    patches.SetCellularReturnType(FastNoiseLite::CellularReturnType_CellValue);
    patches.SetFrequency(1.0f / kFieldPatch);
    for (const SettlementSite& site : ctx.out.settlements) {
        if (site.kind != SettlementKind::Village) {
            continue;
        }
        forEachAround(site, site.radius + kFieldBelt, [&](int x, int y, int) {
            if (proximity.at(x, y) <= kClearingEdge || map.water.at(x, y) != Water::None ||
                map.ground.at(x, y) != Ground::Grass || ctx.slope.at(x, y) > kFieldMaxSlope) {
                return;
            }
            if (patches.GetNoise(Map::cellCenter(x), Map::cellCenter(y)) > kFieldThreshold) {
                map.ground.at(x, y) = Ground::Field;
            }
        });
    }

    const FastNoiseLite forest = makeFbm(ctx.rng, kForestWavelength, 4);
    // Край вырубки вокруг поселения неровный: где-то лес подходит к домам,
    // где-то отступает дальше.
    const FastNoiseLite edge = makeFbm(ctx.rng, kClearingWavelength, 3);
    for (int y = 0; y < ctx.height; ++y) {
        for (int x = 0; x < ctx.width; ++x) {
            if (map.water.at(x, y) != Water::None || map.ground.at(x, y) != Ground::Grass ||
                ctx.slope.at(x, y) > kForestMaxSlope) {
                continue;
            }
            const float wx = Map::cellCenter(x);
            const float wy = Map::cellCenter(y);
            if (proximity.at(x, y) != 255) {
                const float cleared = static_cast<float>(kClearingEdge) +
                                      static_cast<float>(kClearingRagged) * edge.GetNoise(wx, wy);
                if (static_cast<float>(proximity.at(x, y)) < cleared) {
                    continue;
                }
            }
            const float upland =
                std::clamp(map.elevationMeters({x, y}) / kForestUplandHeight, 0.0f, 1.0f);
            const float density = forest.GetNoise(wx, wy) + kForestBias + kForestUplandBias * upland;
            if (density > 0.0f) {
                map.cover.at(x, y) = Cover::Forest;
            }
        }
    }
}

} // namespace lw::gen
