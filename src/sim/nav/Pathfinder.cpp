#include "sim/nav/Pathfinder.h"

#include "sim/nav/NavGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace lw {

namespace {

int chebyshev(Cell a, Cell b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

// Коридор уточнения размечается блоками 8 × 8 клеток: мельче сектора графа,
// чтобы запас вокруг маршрута можно было задавать точнее.
constexpr int kBlockShift = 3;

// Оценка пути по прямой между клетками при средней стоимости клеток cost.
std::uint32_t straightEstimate(Cell a, Cell b, std::uint32_t cost) {
    return nav::octileCost(a, b) / NavGrid::kRoadCost * cost;
}

} // namespace

Pathfinder::Pathfinder(const NavGrid& nav, const NavGraph& graph, const PathfinderSettings& settings)
    : nav_(nav), graph_(graph), settings_(settings) {}

void Pathfinder::prepare() {
    // Сетку мир строит после создания Pathfinder, поэтому буферы — при первом запросе.
    if (preparedWidth_ == nav_.width() && preparedHeight_ == nav_.height()) {
        return;
    }
    preparedWidth_ = nav_.width();
    preparedHeight_ = nav_.height();
    space_.resize(nav_.width(), nav_.height());
    blocksX_ = (nav_.width() + (1 << kBlockShift) - 1) >> kBlockShift;
    blocksY_ = (nav_.height() + (1 << kBlockShift) - 1) >> kBlockShift;
    corridor_.assign(static_cast<std::size_t>(blocksX_) * static_cast<std::size_t>(blocksY_), 0);
    corridorUsed_.clear();
}

Path Pathfinder::find(Cell start, Cell goal) {
    Path path;
    if (!nav_.passable(start) || !nav_.passable(goal)) {
        path.status = PathStatus::Blocked;
        return path;
    }
    if (!nav_.connected(start, goal)) {
        path.status = PathStatus::Unreachable;
        return path;
    }
    prepare();

    bool found = false;
    if (chebyshev(start, goal) <= settings_.directRange) {
        found = searchDirect(start, goal, settings_.directBudget, path);
        path.method = PathMethod::Direct;
    }
    if (!found && graph_.nodeCount() > 0) {
        found = searchGraph(start, goal, path);
        path.method = PathMethod::Graph;
    }
    if (!found) {
        found = searchDirect(start, goal, nav::kNoBudget, path);
        path.method = PathMethod::Full;
    }
    if (!found) {
        path.status = PathStatus::NotFound;
        path.method = PathMethod::None;
        path.cells.clear();
        path.cost = 0;
        return path;
    }

    path.status = PathStatus::Found;
    smooth(path);
    for (std::size_t i = 1; i < path.waypoints.size(); ++i) {
        const float dx = static_cast<float>(path.waypoints[i].x - path.waypoints[i - 1].x);
        const float dy = static_cast<float>(path.waypoints[i].y - path.waypoints[i - 1].y);
        path.length += std::sqrt(dx * dx + dy * dy) * Map::kCellSize;
    }
    return path;
}

bool Pathfinder::searchDirect(Cell start, Cell goal, std::uint32_t budget, Path& path) {
    const nav::SearchOutcome outcome = nav::gridSearch(
        nav_, space_, open_, start, budget, [](int, int) { return true; },
        [goal](int x, int y) { return nav::octileCost({x, y}, goal); },
        [goal](Cell cell, std::uint32_t) { return cell == goal; });
    path.expanded += outcome.expanded;
    if (!outcome.stopped) {
        return false;
    }
    nav::tracePath(space_, goal, path.cells);
    path.cost = space_.g(goal.x, goal.y);
    return true;
}

void Pathfinder::collectAccess(Cell cell, std::vector<Access>& out) const {
    // Из клетки можно идти к клетке своего узла или прямо к соседнему куску,
    // если клетка лежит у его края: соседи получают оценку по прямой от клетки.
    out.clear();
    const std::uint32_t own = graph_.nodeOf(nav_, cell);
    const std::vector<NavGraph::Node>& nodes = graph_.nodes();
    const std::uint32_t ownCost = nodes[own].cost;
    out.push_back({own, straightEstimate(cell, nodes[own].cell, ownCost)});
    for (const NavGraph::Edge& edge : graph_.edges(own)) {
        const NavGraph::Node& next = nodes[edge.to];
        out.push_back({edge.to, straightEstimate(cell, next.cell, (ownCost + next.cost + 1) / 2)});
    }
}

bool Pathfinder::searchGraph(Cell start, Cell goal, Path& path) {
    // A* по графу. Виртуальный узел «цель» связан с узлом куска цели и его
    // соседями оценкой отхода. Оценка остатка — октильное расстояние по дороге до
    // клетки цели: стоимости рёбер, подходов и отходов не меньше октильного
    // расстояния между своими концами, поэтому она монотонна.
    collectAccess(start, starts_);
    collectAccess(goal, exits_);
    const auto count = static_cast<std::uint32_t>(graph_.nodeCount());
    const std::uint32_t target = count;
    graphCost_.assign(count + 1, nav::kUnreached);
    graphParent_.assign(count + 1, NavGraph::kNoNode);
    open_.clear();
    const std::vector<NavGraph::Node>& nodes = graph_.nodes();
    const auto relax = [&](std::uint32_t node, std::uint32_t cost, std::uint32_t parent) {
        if (cost >= graphCost_[node]) {
            return;
        }
        graphCost_[node] = cost;
        graphParent_[node] = parent;
        const std::uint32_t h = node == target ? 0 : nav::octileCost(nodes[node].cell, goal);
        open_.push({cost + h, h, node});
    };
    for (const Access& access : starts_) {
        relax(access.node, access.cost, NavGraph::kNoNode);
    }

    bool reached = false;
    while (!open_.empty()) {
        const nav::OpenEntry entry = open_.pop();
        const std::uint32_t node = entry.key;
        const std::uint32_t cost = graphCost_[node];
        if (entry.f != cost + entry.h) {
            continue;
        }
        if (node == target) {
            reached = true;
            break;
        }
        ++path.graphExpanded;
        for (const Access& exit : exits_) {
            if (exit.node == node) {
                relax(target, cost + exit.cost, node);
            }
        }
        for (const NavGraph::Edge& edge : graph_.edges(node)) {
            relax(edge.to, cost + edge.cost, node);
        }
    }
    if (!reached) {
        return false;
    }
    route_.clear();
    for (std::uint32_t n = graphParent_[target]; n != NavGraph::kNoNode; n = graphParent_[n]) {
        route_.push_back(n);
        path.roadNodes += nodes[n].kind == NavClass::Road ? 1 : 0;
    }
    std::reverse(route_.begin(), route_.end());

    markCorridor(start, goal);
    return refineRoute(start, goal, path);
}

void Pathfinder::markCorridor(Cell start, Cell goal) {
    for (const std::uint32_t block : corridorUsed_) {
        corridor_[block] = 0;
    }
    corridorUsed_.clear();
    markSector(start);
    for (const std::uint32_t node : route_) {
        markSector(graph_.nodes()[node].cell);
    }
    markSector(goal);
}

// Сектор графа, в котором лежит клетка, вместе с запасом corridorMargin клеток.
void Pathfinder::markSector(Cell cell) {
    const int margin = std::max(0, settings_.corridorMargin);
    const int x0 = (cell.x >> NavGraph::kSectorShift) << NavGraph::kSectorShift;
    const int y0 = (cell.y >> NavGraph::kSectorShift) << NavGraph::kSectorShift;
    const int bx0 = std::max(0, (x0 - margin) >> kBlockShift);
    const int by0 = std::max(0, (y0 - margin) >> kBlockShift);
    const int bx1 = std::min(blocksX_ - 1, (x0 + NavGraph::kSectorSize - 1 + margin) >> kBlockShift);
    const int by1 = std::min(blocksY_ - 1, (y0 + NavGraph::kSectorSize - 1 + margin) >> kBlockShift);
    for (int by = by0; by <= by1; ++by) {
        for (int bx = bx0; bx <= bx1; ++bx) {
            const std::size_t block =
                static_cast<std::size_t>(by) * static_cast<std::size_t>(blocksX_) + static_cast<std::size_t>(bx);
            if (corridor_[block] == 0) {
                corridor_[block] = 1;
                corridorUsed_.push_back(static_cast<std::uint32_t>(block));
            }
        }
    }
}

bool Pathfinder::refineRoute(Cell start, Cell goal, Path& path) {
    // Промежуточная цель — клетка первого узла маршрута, отстоящего от предыдущей
    // цели на refineSpan клеток и больше; последняя — сама цель. На коротком
    // участке оценка остатка почти точна, и A* идёт узкой полосой; один поиск на
    // весь извилистый коридор раскрыл бы его почти целиком. Куски маршрута связны
    // внутри своих секторов, а секторы маршрута, начала и цели целиком в
    // коридоре, поэтому каждый участок в коридоре проходим.
    const auto blocksX = static_cast<std::size_t>(blocksX_);
    const auto inCorridor = [&](int x, int y) {
        return corridor_[static_cast<std::size_t>(y >> kBlockShift) * blocksX +
                         static_cast<std::size_t>(x >> kBlockShift)] != 0;
    };
    const std::vector<NavGraph::Node>& nodes = graph_.nodes();
    const int span = std::max(1, settings_.refineSpan);

    path.cells.assign(1, start);
    path.cost = 0;
    Cell from = start;
    std::size_t next = 0;
    while (from != goal) {
        Cell to = goal;
        for (; next < route_.size(); ++next) {
            const Cell cell = nodes[route_[next]].cell;
            if (chebyshev(from, cell) >= span) {
                to = cell;
                ++next;
                break;
            }
        }
        const nav::SearchOutcome outcome = nav::gridSearch(
            nav_, space_, open_, from, nav::kNoBudget, inCorridor,
            [to](int x, int y) { return nav::octileCost({x, y}, to); },
            [to](Cell cell, std::uint32_t) { return cell == to; });
        path.expanded += outcome.expanded;
        if (!outcome.stopped) {
            return false;
        }
        nav::tracePath(space_, to, piece_);
        path.cells.insert(path.cells.end(), piece_.begin() + 1, piece_.end());
        path.cost += space_.g(to.x, to.y);
        from = to;
    }
    return true;
}

// Сглаживание: из каждой точки пути — как можно дальше по прямой, пока прямая
// не хуже исходного участка. «Не хуже» — все клетки под прямой проходимы и не
// дороже самой дорогой клетки участка (путь не срезает через лес, реку или
// поле, если обходил их), а оценка стоимости прямой не больше стоимости участка.
void Pathfinder::smooth(Path& path) {
    const std::vector<Cell>& cells = path.cells;
    path.waypoints.clear();
    path.waypoints.push_back(cells.front());

    prefixCost_.assign(cells.size(), 0);
    for (std::size_t i = 1; i < cells.size(); ++i) {
        prefixCost_[i] = prefixCost_[i - 1] + nav_.stepCost(cells[i - 1], cells[i]);
    }

    const std::size_t last = cells.size() - 1;
    const auto span = static_cast<std::size_t>(std::max(1, settings_.smoothSpan));
    std::size_t anchor = 0;
    while (anchor < last) {
        std::size_t best = anchor + 1;
        std::uint8_t worst = std::max(nav_.cost(cells[anchor]), nav_.cost(cells[best]));
        for (std::size_t j = anchor + 2; j <= last && j - anchor <= span; ++j) {
            worst = std::max(worst, nav_.cost(cells[j]));
            if (!straightIsNoWorse(cells[anchor], cells[j], worst, prefixCost_[j] - prefixCost_[anchor])) {
                break;
            }
            best = j;
        }
        path.waypoints.push_back(cells[best]);
        anchor = best;
    }
}

bool Pathfinder::straightIsNoWorse(Cell a, Cell b, std::uint8_t worst, std::uint32_t pathCost) const {
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    const bool clear = forEachCellOnSegment(a, b, [&](int x, int y) {
        const std::uint8_t cost = nav_.cost(x, y);
        if (cost == NavGrid::kBlocked || cost > worst) {
            return false;
        }
        sum += cost;
        ++count;
        return true;
    });
    if (!clear) {
        return false;
    }
    // Оценка стоимости прямой: длина в клетках × 2·kStraightFactor × средняя
    // стоимость клеток под ней. Сравнение в квадратах, чтобы обойтись без корня:
    // length · perCell · sum / count <= pathCost.
    const auto dx = static_cast<std::uint64_t>(std::abs(b.x - a.x));
    const auto dy = static_cast<std::uint64_t>(std::abs(b.y - a.y));
    const std::uint64_t lengthSq = dx * dx + dy * dy;
    const std::uint64_t perLength = 2 * NavGrid::kStraightFactor * sum;
    const std::uint64_t budget = static_cast<std::uint64_t>(pathCost) * count;
    return lengthSq * perLength * perLength <= budget * budget;
}

} // namespace lw
