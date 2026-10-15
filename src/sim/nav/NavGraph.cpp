#include "sim/nav/NavGraph.h"

#include "sim/nav/SearchSpace.h"

#include <algorithm>
#include <array>
#include <limits>

namespace lw {

namespace {

// Клетка куска, ближайшая к его центру масс; при равенстве — верхняя, затем левая.
Cell centralCell(const std::vector<Cell>& cells) {
    const auto count = static_cast<std::int64_t>(cells.size());
    std::int64_t sumX = 0;
    std::int64_t sumY = 0;
    for (const Cell cell : cells) {
        sumX += cell.x;
        sumY += cell.y;
    }
    Cell best = cells.front();
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
    for (const Cell cell : cells) {
        // Расстояние до центра, умноженное на число клеток: без деления.
        const std::int64_t dx = cell.x * count - sumX;
        const std::int64_t dy = cell.y * count - sumY;
        const std::int64_t distance = dx * dx + dy * dy;
        if (distance < bestDistance ||
            (distance == bestDistance && (cell.y < best.y || (cell.y == best.y && cell.x < best.x)))) {
            best = cell;
            bestDistance = distance;
        }
    }
    return best;
}

// Оценка пути от клетки узла до клетки своего куска: по прямой, с ценой куска.
// Октильная оценка считается по цене дороги, поэтому делится на неё.
std::uint32_t partCost(const NavGraph::Node& node, Cell cell) {
    return nav::octileCost(node.cell, cell) / NavGrid::kRoadCost * node.cost;
}

} // namespace

void NavGraph::build(const NavGrid& nav) {
    nodes_.clear();
    edges_.clear();
    sectorFirst_.clear();
    const int width = nav.width();
    const int height = nav.height();
    sectorsX_ = (width + kSectorSize - 1) >> kSectorShift;
    sectorsY_ = (height + kSectorSize - 1) >> kSectorShift;
    const auto sectorBounds = [&](int sx, int sy, int& x0, int& y0, int& x1, int& y1) {
        x0 = sx * kSectorSize;
        y0 = sy * kSectorSize;
        x1 = std::min(x0 + kSectorSize, width);
        y1 = std::min(y0 + kSectorSize, height);
    };

    // Номер узла для каждой проходимой клетки; нужен только на время построения.
    std::vector<std::uint32_t> label(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                                     kNoNode);
    const auto labelAt = [&](Cell cell) -> std::uint32_t& {
        return label[static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(cell.x)];
    };

    // Узлы: куски клеток одного вида внутри секторов.
    std::vector<Cell> piece;
    sectorFirst_.reserve(static_cast<std::size_t>(sectorsX_) * static_cast<std::size_t>(sectorsY_) + 1);
    for (int sy = 0; sy < sectorsY_; ++sy) {
        for (int sx = 0; sx < sectorsX_; ++sx) {
            sectorFirst_.push_back(static_cast<std::uint32_t>(nodes_.size()));
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            sectorBounds(sx, sy, x0, y0, x1, y1);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const NavClass kind = NavGrid::classOf(nav.cost(x, y));
                    if (kind == NavClass::Blocked || labelAt({x, y}) != kNoNode) {
                        continue;
                    }
                    const auto node = static_cast<std::uint32_t>(nodes_.size());
                    labelAt({x, y}) = node;
                    piece.assign(1, Cell{x, y});
                    std::uint64_t costSum = 0;
                    for (std::size_t k = 0; k < piece.size(); ++k) {
                        const Cell cell = piece[k];
                        costSum += nav.cost(cell);
                        for (int d = 0; d < 4; ++d) {
                            const Cell next{cell.x + kNavDx[d], cell.y + kNavDy[d]};
                            if (next.x < x0 || next.y < y0 || next.x >= x1 || next.y >= y1 ||
                                NavGrid::classOf(nav.cost(next)) != kind || labelAt(next) != kNoNode) {
                                continue;
                            }
                            labelAt(next) = node;
                            piece.push_back(next);
                        }
                    }
                    const std::uint64_t count = piece.size();
                    const auto meanCost = static_cast<std::uint8_t>((costSum + count / 2) / count);
                    nodes_.push_back({centralCell(piece), kind, meanCost});
                }
            }
        }
    }
    sectorFirst_.push_back(static_cast<std::uint32_t>(nodes_.size()));

    // Рёбра. Пара кусков находится ровно в одном секторе: соседи справа и снизу
    // принадлежат тому, чьи клетки обходим, а диагональ через угол четырёх секторов —
    // сектору слева сверху от угла. Поэтому повторы убираются внутри сектора.
    struct Link {
        std::uint32_t a;
        std::uint32_t b;
        std::uint32_t cost;
    };
    std::vector<Link> local;
    std::vector<Link> links;
    const auto consider = [&](Cell from, Cell to) {
        if (!nav.contains(from) || !nav.contains(to)) {
            return;
        }
        const std::uint32_t a = labelAt(from);
        const std::uint32_t b = labelAt(to);
        if (a == kNoNode || b == kNoNode || a == b) {
            return;
        }
        const std::uint32_t step = nav.stepCost(from, to);
        if (step == 0) {
            return; // диагональ мимо непроходимой клетки
        }
        const std::uint32_t cost = partCost(nodes_[a], from) + step + partCost(nodes_[b], to);
        local.push_back({std::min(a, b), std::max(a, b), cost});
    };
    for (int sy = 0; sy < sectorsY_; ++sy) {
        for (int sx = 0; sx < sectorsX_; ++sx) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            sectorBounds(sx, sy, x0, y0, x1, y1);
            local.clear();
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    consider({x, y}, {x + 1, y});
                    consider({x, y}, {x, y + 1});
                }
            }
            // Угол справа снизу: обе диагонали между четырьмя секторами.
            consider({x1 - 1, y1 - 1}, {x1, y1});
            consider({x1, y1 - 1}, {x1 - 1, y1});
            std::sort(local.begin(), local.end(), [](const Link& l, const Link& r) {
                return l.a != r.a ? l.a < r.a : l.b != r.b ? l.b < r.b : l.cost < r.cost;
            });
            for (std::size_t i = 0; i < local.size(); ++i) {
                if (i == 0 || local[i].a != local[i - 1].a || local[i].b != local[i - 1].b) {
                    links.push_back(local[i]); // самая дешёвая точка касания пары
                }
            }
        }
    }

    std::vector<Link> directed;
    directed.reserve(links.size() * 2);
    for (const Link& link : links) {
        directed.push_back(link);
        directed.push_back({link.b, link.a, link.cost});
    }
    std::sort(directed.begin(), directed.end(), [](const Link& l, const Link& r) {
        return l.a != r.a ? l.a < r.a : l.b < r.b;
    });
    edges_.reserve(directed.size());
    for (const Link& link : directed) {
        Node& node = nodes_[link.a];
        if (node.edgeCount == 0) {
            node.firstEdge = static_cast<std::uint32_t>(edges_.size());
        }
        ++node.edgeCount;
        edges_.push_back({link.b, link.cost});
    }
}

std::uint32_t NavGraph::nodeOf(const NavGrid& nav, Cell cell) const {
    const NavClass kind = NavGrid::classOf(nav.cost(cell));
    if (kind == NavClass::Blocked) {
        return kNoNode;
    }
    const int sx = cell.x >> kSectorShift;
    const int sy = cell.y >> kSectorShift;
    const std::size_t sector =
        static_cast<std::size_t>(sy) * static_cast<std::size_t>(sectorsX_) + static_cast<std::size_t>(sx);
    const std::uint32_t first = sectorFirst_[sector];
    const std::uint32_t last = sectorFirst_[sector + 1];

    // Чаще всего кусок этого вида в секторе один.
    std::uint32_t only = kNoNode;
    int sameKind = 0;
    for (std::uint32_t n = first; n < last; ++n) {
        if (nodes_[n].kind == kind) {
            only = n;
            ++sameKind;
        }
    }
    if (sameKind == 1) {
        return only;
    }

    // Иначе — обход куска от клетки, пока не встретится клетка одного из узлов.
    const int x0 = sx << kSectorShift;
    const int y0 = sy << kSectorShift;
    const int x1 = std::min(x0 + kSectorSize, nav.width());
    const int y1 = std::min(y0 + kSectorSize, nav.height());
    constexpr std::size_t kArea = static_cast<std::size_t>(kSectorSize) * kSectorSize;
    std::array<bool, kArea> seen{};
    std::array<Cell, kArea> queue;
    const auto local = [&](Cell c) {
        return static_cast<std::size_t>((c.y - y0) * kSectorSize + (c.x - x0));
    };
    std::size_t head = 0;
    std::size_t tail = 0;
    queue[tail++] = cell;
    seen[local(cell)] = true;
    while (head < tail) {
        const Cell current = queue[head++];
        for (std::uint32_t n = first; n < last; ++n) {
            if (nodes_[n].cell == current) {
                return n;
            }
        }
        for (int d = 0; d < 4; ++d) {
            const Cell next{current.x + kNavDx[d], current.y + kNavDy[d]};
            if (next.x < x0 || next.y < y0 || next.x >= x1 || next.y >= y1 || seen[local(next)] ||
                NavGrid::classOf(nav.cost(next)) != kind) {
                continue;
            }
            seen[local(next)] = true;
            queue[tail++] = next;
        }
    }
    return kNoNode; // не бывает: клетка узла лежит в его куске
}

} // namespace lw
