// Дороги между поселениями по иерархии (этап 2а.3): шоссе между городами,
// трассы между районами, просёлки к деревням. Граф соседства и прокладка A*
// по стоимости рельефа; правила прокладки — у класса дороги.

#include "sim/gen/GenContext.h"

#include <algorithm>
#include <cmath>
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
// Шоссе строят с размахом: мост ему не помеха — реку оно пересекает прямо, а
// не ищет брод за километр.
constexpr int kHighwayRiver = 56;
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
// Шоссе положе: стоимость удваивает уже уклон 6 % — (0,06 · 16 / 0,1)².
constexpr int kHighwayClimbSq = 92;
// Оценка остатка пути — доля от стоимости по ровному лугу. Выше — поиск
// быстрее, но хуже замечает готовые дороги в стороне от прямой.
constexpr int kHeuristicNum = 1;
constexpr int kHeuristicDen = 2;

constexpr std::uint32_t kUnreached = 0xFFFFFFFFu;

// Кольца шоссе: связь двух соседних городов, которой нет в остовном дереве,
// тоже шоссе, если по дереву между ними дальше прямой больше чем во столько раз.
constexpr float kLoopDetour = 1.6f;
// Деревня с таким радиусом (клетки) и больше — село, центр района: к нему и
// между такими ведут трассы.
constexpr int kDistrictRadius = 29;
// Развязка: вокруг места примыкания полотно шире — съезд без острого угла.
constexpr int kJunctionApron = 2; // клетки, ромб |dx| + |dy| ≤ этого
// Развязки ближе этого к краю поселения — уже перекрёстки улиц.
constexpr int kJunctionTownMargin = 8; // клетки

// Как прокладывать дорогу класса.
struct RouteRules {
    int river = kCostRiver;
    int climbSq = kClimbSq;
};

RouteRules rulesOf(RoadClass kind) {
    if (kind == RoadClass::Highway) {
        return {kHighwayRiver, kHighwayClimbSq};
    }
    return {};
}

// Поиск пути по сетке узлов с шагом kRoadStep клеток: на полной сетке он шёл
// бы на порядок дольше, а дороге такая точность не нужна.
class Router {
public:
    explicit Router(const Context& ctx)
        : ctx_(ctx),
          width_(ctx.width / kRoadStep),
          height_(ctx.height / kRoadStep),
          terrain_(width_, height_),
          river_(width_, height_),
          elevation_(width_, height_),
          paved_(width_, height_) {
        const auto count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        cost_.assign(count, kUnreached);
        from_.assign(count, 0);

        const Map& map = ctx.map;
        for (int ny = 0; ny < height_; ++ny) {
            for (int nx = 0; nx < width_; ++nx) {
                // Узел стоит столько, сколько худшая клетка его блока; река —
                // отдельно: её цена у каждого класса своя.
                int worst = kCostOpen;
                bool river = false;
                int elevationSum = 0;
                for (int y = ny * kRoadStep; y < (ny + 1) * kRoadStep; ++y) {
                    for (int x = nx * kRoadStep; x < (nx + 1) * kRoadStep; ++x) {
                        elevationSum += map.elevation.at(x, y);
                        worst = std::max(worst, cellCost(x, y));
                        river = river || map.water.at(x, y) == Water::River;
                    }
                }
                // Вне главной суши (вода, острова) дорог нет.
                const bool passable = ctx.mainland.at(nx * kRoadStep, ny * kRoadStep) != 0;
                terrain_.at(nx, ny) = static_cast<std::uint8_t>(passable ? worst : 0);
                river_.at(nx, ny) = river ? 1 : 0;
                elevation_.at(nx, ny) =
                    static_cast<std::int16_t>(elevationSum / (kRoadStep * kRoadStep));
            }
        }
    }

    // Путь по клеткам от start до goal включительно; пусто, если пути нет.
    // Обе клетки должны быть узлами. Найденный путь считается замощённым.
    std::vector<Cell> route(Cell start, Cell goal, const RouteRules& rules) {
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
                const int ground = river_.at(nx, ny) ? std::max<int>(terrain_.at(nx, ny), rules.river)
                                                     : terrain_.at(nx, ny);
                const int terrain = paved_.at(nx, ny) ? kCostRoad : ground;
                const int climb = std::min(std::abs(elevation_.at(nx, ny) - elevation), kMaxClimb);
                const int climbSq = rules.climbSq;
                const int step =
                    diagonal ? kDiagonal * terrain * (2 * climbSq + climb * climb) / (32 * climbSq)
                             : kStraight * terrain * (climbSq + climb * climb) / (16 * climbSq);
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

    // Стоимость клетки без реки (реку учитывает river_).
    int cellCost(int x, int y) const {
        const Map& map = ctx_.map;
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
    Grid<std::uint8_t> terrain_;   // стоимость узла без реки; ноль — непроходим
    Grid<std::uint8_t> river_;     // в блоке узла есть река
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

float distance(const SettlementSite& a, const SettlementSite& b) {
    return std::sqrt(static_cast<float>(distanceSq(a, b)));
}

bool shorter(const Link& l, const Link& r) {
    if (l.lengthSq != r.lengthSq) {
        return l.lengthSq < r.lengthSq;
    }
    return l.a != r.a ? l.a < r.a : l.b < r.b;
}

bool samePair(const Link& l, const Link& r) {
    return l.a == r.a && l.b == r.b;
}

// Остовное дерево по набору поселений (алгоритм Прима): кратчайшая сеть,
// связывающая всех.
std::vector<Link> spanningTree(const std::vector<SettlementSite>& sites, const std::vector<int>& members) {
    std::vector<Link> links;
    if (members.size() < 2) {
        return links;
    }
    std::vector<int> connected = {members.front()};
    std::vector<int> rest(members.begin() + 1, members.end());
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

// Граф Габриэля по набору поселений: два — соседи, если в круге, построенном
// на отрезке между ними как на диаметре, нет третьего из набора. Связен и даёт
// кольца, а не одно дерево: между поселениями бывает несколько путей.
std::vector<Link> neighborLinks(const std::vector<SettlementSite>& sites, const std::vector<int>& members) {
    std::vector<Link> links;
    for (std::size_t i = 0; i < members.size(); ++i) {
        for (std::size_t j = i + 1; j < members.size(); ++j) {
            const int a = std::min(members[i], members[j]);
            const int b = std::max(members[i], members[j]);
            const SettlementSite& sa = sites[static_cast<std::size_t>(a)];
            const SettlementSite& sb = sites[static_cast<std::size_t>(b)];
            const int lengthSq = distanceSq(sa, sb);
            bool neighbors = true;
            for (std::size_t k = 0; k < members.size() && neighbors; ++k) {
                const int c = members[k];
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

// Длина пути между a и b по дереву (сумма прямых отрезков); дерево связно.
float treeDistance(const std::vector<SettlementSite>& sites, const std::vector<Link>& tree, int a, int b) {
    // Поиск в глубину: узлов мало (города).
    struct Visit {
        int site;
        float length;
    };
    std::vector<Visit> stack = {{a, 0.0f}};
    std::vector<int> seen = {a};
    while (!stack.empty()) {
        const Visit visit = stack.back();
        stack.pop_back();
        if (visit.site == b) {
            return visit.length;
        }
        for (const Link& link : tree) {
            const int next = link.a == visit.site ? link.b : link.b == visit.site ? link.a : -1;
            if (next < 0 || std::find(seen.begin(), seen.end(), next) != seen.end()) {
                continue;
            }
            seen.push_back(next);
            stack.push_back({next, visit.length + distance(sites[static_cast<std::size_t>(visit.site)],
                                                           sites[static_cast<std::size_t>(next)])});
        }
    }
    return 0.0f;
}

// Шоссе: остовное дерево городов и кольца там, где по дереву большой крюк.
std::vector<Link> highwayLinks(const std::vector<SettlementSite>& sites, const std::vector<int>& towns) {
    std::vector<Link> links = spanningTree(sites, towns);
    const std::vector<Link> tree = links;
    for (const Link& link : neighborLinks(sites, towns)) {
        if (std::any_of(tree.begin(), tree.end(), [&](const Link& t) { return samePair(t, link); })) {
            continue;
        }
        const SettlementSite& a = sites[static_cast<std::size_t>(link.a)];
        const SettlementSite& b = sites[static_cast<std::size_t>(link.b)];
        if (treeDistance(sites, tree, link.a, link.b) > kLoopDetour * distance(a, b)) {
            links.push_back(link);
        }
    }
    return links;
}

// Длина пути по клеткам, метры.
float pathMeters(const std::vector<Cell>& path) {
    float meters = 0.0f;
    for (std::size_t k = 1; k < path.size(); ++k) {
        const bool diagonal = path[k].x != path[k - 1].x && path[k].y != path[k - 1].y;
        meters += diagonal ? Map::kCellSize * 1.41421356f : Map::kCellSize;
    }
    return meters;
}

// Номер шага kDx/kDy от a к соседней b.
int headingOf(Cell a, Cell b) {
    const int dx = (b.x > a.x) - (b.x < a.x);
    const int dy = (b.y > a.y) - (b.y < a.y);
    for (int d = 0; d < 8; ++d) {
        if (kDx[d] == dx && kDy[d] == dy) {
            return d;
        }
    }
    return 0;
}

// Мосты участка: клетки реки на оси подряд.
void findBridges(const Map& map, const RoadSite& road, std::vector<RoadBridge>& out) {
    RoadBridge bridge;
    bridge.kind = road.kind;
    for (const Cell& cell : road.path) {
        if (map.water.at(cell.x, cell.y) == Water::River) {
            if (bridge.cells == 0) {
                bridge.first = cell;
            }
            bridge.last = cell;
            ++bridge.cells;
        } else if (bridge.cells > 0) {
            out.push_back(bridge);
            bridge.cells = 0;
        }
    }
    if (bridge.cells > 0) {
        out.push_back(bridge);
    }
}

bool nearSettlement(const std::vector<SettlementSite>& sites, Cell cell) {
    return std::any_of(sites.begin(), sites.end(), [&](const SettlementSite& site) {
        const int reach = site.radius + kJunctionTownMargin;
        const int dx = cell.x - site.center.x;
        const int dy = cell.y - site.center.y;
        return dx * dx + dy * dy <= reach * reach;
    });
}

RoadClass classOfSurface(Road road) {
    return road == Road::Highway   ? RoadClass::Highway
           : road == Road::Asphalt ? RoadClass::Regional
                                   : RoadClass::Track;
}

// Развязки: где ось дороги ниже классом входит на полотно дороги выше классом
// (или сходит с него) вне поселений. Вокруг — асфальтовый съезд.
void placeJunctions(Context& ctx) {
    Map& map = ctx.map;
    const std::vector<SettlementSite>& sites = ctx.out.settlements;
    std::vector<RoadJunction>& junctions = ctx.out.junctions;
    const auto add = [&](Cell at, RoadClass minor, int heading) {
        if (nearSettlement(sites, at)) {
            return;
        }
        const bool known = std::any_of(junctions.begin(), junctions.end(), [&](const RoadJunction& j) {
            return std::abs(j.at.x - at.x) <= kRoadStep && std::abs(j.at.y - at.y) <= kRoadStep;
        });
        if (!known) {
            junctions.push_back({at, classOfSurface(map.road.at(at.x, at.y)), minor, heading});
        }
    };
    for (const RoadSite& road : ctx.out.roads) {
        if (road.kind == RoadClass::Highway) {
            continue;
        }
        const Road own = surfaceOf(road.kind);
        for (std::size_t k = 1; k < road.path.size(); ++k) {
            const Cell before = road.path[k - 1];
            const Cell cell = road.path[k];
            const Road was = map.road.at(before.x, before.y);
            const Road is = map.road.at(cell.x, cell.y);
            if (was == own && is > own) {
                add(cell, road.kind, headingOf(before, cell));
            } else if (was > own && is == own) {
                add(before, road.kind, headingOf(cell, before));
            }
        }
    }
    for (const RoadJunction& junction : junctions) {
        for (int dy = -kJunctionApron; dy <= kJunctionApron; ++dy) {
            for (int dx = -kJunctionApron; dx <= kJunctionApron; ++dx) {
                const Cell cell{junction.at.x + dx, junction.at.y + dy};
                if (std::abs(dx) + std::abs(dy) > kJunctionApron || !map.contains(cell) ||
                    map.water.at(cell.x, cell.y) != Water::None) {
                    continue;
                }
                setRoad(map, cell.x, cell.y, Road::Asphalt);
            }
        }
    }
}

} // namespace

void buildRoads(Context& ctx) {
    const std::vector<SettlementSite>& sites = ctx.out.settlements;
    Router router(ctx);

    std::vector<int> all;
    std::vector<int> towns;
    std::vector<int> districts; // города и сёла
    for (int i = 0; i < static_cast<int>(sites.size()); ++i) {
        const SettlementSite& site = sites[static_cast<std::size_t>(i)];
        all.push_back(i);
        if (site.kind == SettlementKind::Town) {
            towns.push_back(i);
            districts.push_back(i);
        } else if (site.radius >= kDistrictRadius) {
            districts.push_back(i);
        }
    }

    std::vector<Link> built;
    const auto connect = [&](const Link& link, RoadClass kind) {
        if (std::any_of(built.begin(), built.end(), [&](const Link& b) { return samePair(b, link); })) {
            return;
        }
        built.push_back(link);
        RoadSite road;
        road.kind = kind;
        road.from = link.a;
        road.to = link.b;
        road.path = router.route(sites[static_cast<std::size_t>(link.a)].center,
                                 sites[static_cast<std::size_t>(link.b)].center, rulesOf(kind));
        if (road.path.empty()) {
            return;
        }
        road.lengthMeters = pathMeters(road.path);
        pavePath(ctx, road.path, surfaceOf(kind), widthOf(kind));
        findBridges(ctx.map, road, ctx.out.bridges);
        ctx.out.roads.push_back(std::move(road));
    };

    // Сверху вниз по иерархии: дороги ниже классом вливаются в уже
    // проложенные, а не идут рядом.
    for (const Link& link : highwayLinks(sites, towns)) {
        connect(link, RoadClass::Highway);
    }
    for (const Link& link : neighborLinks(sites, districts)) {
        connect(link, RoadClass::Regional);
    }
    for (const Link& link : neighborLinks(sites, all)) {
        connect(link, RoadClass::Track);
    }
    placeJunctions(ctx);

    // Центр поселения лежит на дороге, даже если соседей у него нет.
    for (const SettlementSite& site : sites) {
        setRoad(ctx.map, site.center.x, site.center.y, Road::Dirt);
    }
}

} // namespace lw::gen
