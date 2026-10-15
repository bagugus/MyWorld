// Дороги между поселениями: граф соседства и прокладка A* по стоимости рельефа.

#include "sim/gen/GenContext.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace lw::gen {

namespace {

// Стоимость узла на единицу длины (16 — ровный луг). Готовая дорога дешевле
// всего, поэтому новые дороги вливаются в уже проложенные, а не идут рядом.
constexpr int kCostRoad = 5;
constexpr int kCostOpen = 16;
constexpr int kCostSand = 22;
constexpr int kCostForest = 24;
constexpr int kCostRock = 48;
constexpr int kCostRiver = 192; // мост: короткий и только там, где иначе никак
// Длина шага между узлами: по стороне и по диагонали.
constexpr int kStraight = 10;
constexpr int kDiagonal = 14;
// Перепад высот между соседними узлами (единицы kElevationStep), дальше
// которого штраф не растёт.
constexpr int kMaxClimb = 100;
// Уклон 8 % удваивает стоимость шага, дальше штраф растёт квадратично: дорога
// огибает холмы, а не лезет в лоб. Число — квадрат перепада высот между
// соседними узлами при таком уклоне: (0,08 · 16 м / 0,1 м)².
constexpr int kClimbSq = 164;
// Оценка остатка пути — доля от стоимости по ровному лугу. Выше — поиск
// быстрее, но хуже замечает готовые дороги в стороне от прямой.
constexpr int kHeuristicNum = 1;
constexpr int kHeuristicDen = 2;

constexpr std::uint32_t kUnreached = 0xFFFFFFFFu;

// Поиск пути по сетке узлов с шагом kRoadStep клеток: на полной сетке он шёл
// бы на порядок дольше, а дороге такая точность не нужна.
class Router {
public:
    explicit Router(const Context& ctx)
        : ctx_(ctx),
          width_(ctx.width / kRoadStep),
          height_(ctx.height / kRoadStep),
          terrain_(width_, height_),
          elevation_(width_, height_),
          paved_(width_, height_) {
        const auto count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        cost_.assign(count, kUnreached);
        from_.assign(count, 0);

        const Map& map = ctx.map;
        for (int ny = 0; ny < height_; ++ny) {
            for (int nx = 0; nx < width_; ++nx) {
                // Узел стоит столько, сколько худшая клетка его блока.
                int worst = kCostOpen;
                int elevationSum = 0;
                for (int y = ny * kRoadStep; y < (ny + 1) * kRoadStep; ++y) {
                    for (int x = nx * kRoadStep; x < (nx + 1) * kRoadStep; ++x) {
                        elevationSum += map.elevation.at(x, y);
                        worst = std::max(worst, cellCost(x, y));
                    }
                }
                // Вне главной суши (вода, острова) дорог нет.
                const bool passable = ctx.mainland.at(nx * kRoadStep, ny * kRoadStep) != 0;
                terrain_.at(nx, ny) = static_cast<std::uint8_t>(passable ? worst : 0);
                elevation_.at(nx, ny) =
                    static_cast<std::int16_t>(elevationSum / (kRoadStep * kRoadStep));
            }
        }
    }

    // Путь по клеткам от start до goal включительно; пусто, если пути нет.
    // Обе клетки должны быть узлами. Найденный путь считается замощённым.
    std::vector<Cell> route(Cell start, Cell goal) {
        for (const std::uint32_t i : touched_) {
            cost_[i] = kUnreached;
        }
        touched_.clear();

        const Cell first{start.x / kRoadStep, start.y / kRoadStep};
        const Cell last{goal.x / kRoadStep, goal.y / kRoadStep};

        // Пары (оценка, узел): при равной оценке порядок задаёт номер узла,
        // так что результат не зависит от реализации очереди.
        using Entry = std::pair<std::uint32_t, std::uint32_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;

        const auto estimate = [&](int x, int y) {
            const int dx = std::abs(x - last.x);
            const int dy = std::abs(y - last.y);
            const int octile = kStraight * std::max(dx, dy) + (kDiagonal - kStraight) * std::min(dx, dy);
            return static_cast<std::uint32_t>(octile * kHeuristicNum / kHeuristicDen);
        };

        const std::uint32_t firstIndex = index(first.x, first.y);
        const std::uint32_t lastIndex = index(last.x, last.y);
        cost_[firstIndex] = 0;
        touched_.push_back(firstIndex);
        open.emplace(estimate(first.x, first.y), firstIndex);

        bool found = false;
        while (!open.empty()) {
            const auto [score, i] = open.top();
            open.pop();
            const int x = static_cast<int>(i % static_cast<std::uint32_t>(width_));
            const int y = static_cast<int>(i / static_cast<std::uint32_t>(width_));
            const std::uint32_t cost = cost_[i];
            if (score != cost + estimate(x, y)) {
                continue; // устаревшая запись: до узла уже нашли путь короче
            }
            if (i == lastIndex) {
                found = true;
                break;
            }

            const int elevation = elevation_.at(x, y);
            for (int d = 0; d < 8; ++d) {
                const int nx = x + kDx[d];
                const int ny = y + kDy[d];
                if (!terrain_.contains(nx, ny) || terrain_.at(nx, ny) == 0) {
                    continue;
                }
                const bool diagonal = d >= 4;
                // Диагональ задевает два соседних блока: вода в них недопустима.
                if (diagonal && (terrain_.at(nx, y) == 0 || terrain_.at(x, ny) == 0)) {
                    continue;
                }
                const int terrain = paved_.at(nx, ny) ? kCostRoad : terrain_.at(nx, ny);
                const int climb = std::min(std::abs(elevation_.at(nx, ny) - elevation), kMaxClimb);
                const int step =
                    diagonal ? kDiagonal * terrain * (2 * kClimbSq + climb * climb) / (32 * kClimbSq)
                             : kStraight * terrain * (kClimbSq + climb * climb) / (16 * kClimbSq);
                const std::uint32_t next = cost + static_cast<std::uint32_t>(std::max(step, 1));
                const std::uint32_t n = index(nx, ny);
                if (next < cost_[n]) {
                    if (cost_[n] == kUnreached) {
                        touched_.push_back(n);
                    }
                    cost_[n] = next;
                    from_[n] = static_cast<std::uint8_t>(d);
                    open.emplace(next + estimate(nx, ny), n);
                }
            }
        }

        std::vector<Cell> path;
        if (!found) {
            return path;
        }
        // От цели к началу; между соседними узлами — kRoadStep клеток по прямой.
        Cell node = last;
        path.push_back({node.x * kRoadStep, node.y * kRoadStep});
        while (!(node == first)) {
            paved_.at(node.x, node.y) = 1;
            const std::uint8_t d = from_[index(node.x, node.y)];
            for (int k = 0; k < kRoadStep; ++k) {
                const Cell back = path.back();
                path.push_back({back.x - kDx[d], back.y - kDy[d]});
            }
            node = {node.x - kDx[d], node.y - kDy[d]};
        }
        paved_.at(first.x, first.y) = 1;
        std::reverse(path.begin(), path.end());
        return path;
    }

private:
    std::uint32_t index(int x, int y) const {
        return static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(width_) +
               static_cast<std::uint32_t>(x);
    }

    int cellCost(int x, int y) const {
        const Map& map = ctx_.map;
        if (map.water.at(x, y) == Water::River) {
            return kCostRiver;
        }
        if (map.ground.at(x, y) == Ground::Rock) {
            return kCostRock;
        }
        if (map.cover.at(x, y) == Cover::Forest) {
            return kCostForest;
        }
        return map.ground.at(x, y) == Ground::Sand ? kCostSand : kCostOpen;
    }

    const Context& ctx_;
    int width_; // узлы
    int height_;
    Grid<std::uint8_t> terrain_;   // стоимость узла; ноль — непроходим
    Grid<std::int16_t> elevation_; // средняя высота блока
    Grid<std::uint8_t> paved_;     // через узел уже идёт дорога
    std::vector<std::uint32_t> cost_;
    std::vector<std::uint8_t> from_;
    std::vector<std::uint32_t> touched_;
};

// Кладёт дорогу вдоль пути. Клетки дороги связаны сторонами, а не углами.
void pavePath(Context& ctx, const std::vector<Cell>& path, Road type, int width) {
    Map& map = ctx.map;
    const auto pave = [&](int x, int y) {
        for (int dy = 0; dy < width; ++dy) {
            for (int dx = 0; dx < width; ++dx) {
                const Cell cell{x + dx, y + dy};
                if (map.contains(cell) && !isOpenWater(map.water.at(cell.x, cell.y))) {
                    setRoad(map, cell.x, cell.y, type);
                }
            }
        }
    };
    for (std::size_t k = 0; k < path.size(); ++k) {
        const Cell cell = path[k];
        pave(cell.x, cell.y);
        if (k > 0 && path[k - 1].x != cell.x && path[k - 1].y != cell.y) {
            // Диагональный шаг: уголок на той клетке, где дорога уже есть.
            const Cell a{cell.x, path[k - 1].y};
            const Cell b{path[k - 1].x, cell.y};
            const bool useB = map.road.at(a.x, a.y) == Road::None && map.road.at(b.x, b.y) != Road::None;
            pave(useB ? b.x : a.x, useB ? b.y : a.y);
        }
    }
}

struct Link {
    int a = 0;
    int b = 0;
    int lengthSq = 0;
};

int distanceSq(const SettlementSite& a, const SettlementSite& b) {
    const int dx = a.center.x - b.center.x;
    const int dy = a.center.y - b.center.y;
    return dx * dx + dy * dy;
}

bool shorter(const Link& l, const Link& r) {
    if (l.lengthSq != r.lengthSq) {
        return l.lengthSq < r.lengthSq;
    }
    return l.a != r.a ? l.a < r.a : l.b < r.b;
}

// Трассы между городами: минимальное остовное дерево (алгоритм Прима).
std::vector<Link> townHighways(const std::vector<SettlementSite>& sites) {
    std::vector<int> towns;
    for (int i = 0; i < static_cast<int>(sites.size()); ++i) {
        if (sites[static_cast<std::size_t>(i)].kind == SettlementKind::Town) {
            towns.push_back(i);
        }
    }
    std::vector<Link> links;
    if (towns.size() < 2) {
        return links;
    }
    std::vector<int> connected = {towns.front()};
    std::vector<int> rest(towns.begin() + 1, towns.end());
    while (!rest.empty()) {
        Link best;
        std::size_t bestSlot = 0;
        bool have = false;
        for (std::size_t slot = 0; slot < rest.size(); ++slot) {
            for (const int from : connected) {
                const Link link{std::min(from, rest[slot]), std::max(from, rest[slot]),
                                distanceSq(sites[static_cast<std::size_t>(from)],
                                           sites[static_cast<std::size_t>(rest[slot])])};
                if (!have || shorter(link, best)) {
                    best = link;
                    bestSlot = slot;
                    have = true;
                }
            }
        }
        links.push_back(best);
        connected.push_back(rest[bestSlot]);
        rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(bestSlot));
    }
    return links;
}

// Граф Габриэля: два поселения — соседи, если в круге, построенном на отрезке
// между ними как на диаметре, нет третьего. Связен и даёт кольца, а не одно
// дерево: между поселениями бывает несколько путей.
std::vector<Link> neighborLinks(const std::vector<SettlementSite>& sites) {
    std::vector<Link> links;
    const int count = static_cast<int>(sites.size());
    for (int a = 0; a < count; ++a) {
        for (int b = a + 1; b < count; ++b) {
            const SettlementSite& sa = sites[static_cast<std::size_t>(a)];
            const SettlementSite& sb = sites[static_cast<std::size_t>(b)];
            const int lengthSq = distanceSq(sa, sb);
            bool neighbors = true;
            for (int c = 0; c < count && neighbors; ++c) {
                if (c == a || c == b) {
                    continue;
                }
                // Угол при третьей точке тупой или прямой — она внутри круга.
                const SettlementSite& sc = sites[static_cast<std::size_t>(c)];
                neighbors = distanceSq(sa, sc) + distanceSq(sb, sc) > lengthSq;
            }
            if (neighbors) {
                links.push_back({a, b, lengthSq});
            }
        }
    }
    std::sort(links.begin(), links.end(), shorter);
    return links;
}

} // namespace

void buildRoads(Context& ctx) {
    const std::vector<SettlementSite>& sites = ctx.out.settlements;
    Router router(ctx);

    const auto connect = [&](const Link& link, Road type, int width) {
        const std::vector<Cell> path = router.route(sites[static_cast<std::size_t>(link.a)].center,
                                                    sites[static_cast<std::size_t>(link.b)].center);
        pavePath(ctx, path, type, width);
    };

    // Сначала трассы: остальные дороги к ним примыкают.
    const std::vector<Link> highways = townHighways(sites);
    for (const Link& link : highways) {
        connect(link, Road::Asphalt, 2);
    }
    for (const Link& link : neighborLinks(sites)) {
        const bool paved = std::any_of(highways.begin(), highways.end(), [&](const Link& highway) {
            return highway.a == link.a && highway.b == link.b;
        });
        if (paved) {
            continue;
        }
        const bool toTown = sites[static_cast<std::size_t>(link.a)].kind == SettlementKind::Town ||
                            sites[static_cast<std::size_t>(link.b)].kind == SettlementKind::Town;
        connect(link, toTown ? Road::Asphalt : Road::Dirt, 1);
    }

    // Центр поселения лежит на дороге, даже если соседей у него нет.
    for (const SettlementSite& site : sites) {
        setRoad(ctx.map, site.center.x, site.center.y, Road::Dirt);
    }
}

} // namespace lw::gen
