#pragma once

// Поиск по сетке клеток (A*) и октильная оценка — общая часть Pathfinder и NavGraph.

#include "sim/Map.h"
#include "sim/nav/NavGrid.h"

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

namespace lw::nav {

inline constexpr std::uint32_t kUnreached = 0xFFFFFFFFu;
inline constexpr std::uint32_t kNoBudget = 0xFFFFFFFFu;
inline constexpr std::uint8_t kNoDirection = 8;

// Клетка одним числом: (y << 16) | x. Порядок чисел — порядок клеток по строкам.
inline std::uint32_t packCell(int x, int y) {
    return (static_cast<std::uint32_t>(y) << 16) | static_cast<std::uint32_t>(x);
}
inline Cell unpackCell(std::uint32_t packed) {
    return {static_cast<int>(packed & 0xFFFFu), static_cast<int>(packed >> 16)};
}

// Нижняя оценка стоимости пути между клетками: октильное расстояние по дороге.
// Допустима и монотонна, потому что дешевле дороги клеток нет.
static_assert(NavGrid::kRoadStraightStep % 10 == 0 && NavGrid::kRoadDiagonalStep % 10 == 0,
              "взвешенная оценка делит октильную на 10 без остатка");
inline std::uint32_t octileCost(Cell a, Cell b) {
    const auto dx = static_cast<std::uint32_t>(std::abs(a.x - b.x));
    const auto dy = static_cast<std::uint32_t>(std::abs(a.y - b.y));
    const std::uint32_t low = std::min(dx, dy);
    const std::uint32_t high = std::max(dx, dy);
    return NavGrid::kRoadStraightStep * (high - low) + NavGrid::kRoadDiagonalStep * low;
}

// Записи поиска — стоимость пути от начала и направление последнего шага — для
// клеток, которых поиск коснулся. Память выделяется страницами 32 × 32 клетки по
// мере надобности: поиск по соседней улице не держит массивов на всю карту, а
// очистка стоит столько, сколько страниц было занято.
class SearchSpace {
public:
    void resize(int width, int height) {
        assert(width <= 0x10000 && height <= 0x10000); // packCell
        pagesX_ = (width + kPageSize - 1) >> kPageShift;
        const int pagesY = (height + kPageSize - 1) >> kPageShift;
        table_.assign(static_cast<std::size_t>(pagesX_) * static_cast<std::size_t>(pagesY), kNoPage);
        used_.clear();
    }

    void clear() {
        for (const std::uint32_t page : used_) {
            table_[page] = kNoPage;
        }
        used_.clear();
    }

    std::uint32_t g(int x, int y) const {
        const std::uint32_t page = table_[pageOf(x, y)];
        return page == kNoPage ? kUnreached : g_[slot(page, x, y)];
    }
    std::uint8_t direction(int x, int y) const {
        const std::uint32_t page = table_[pageOf(x, y)];
        return page == kNoPage ? kNoDirection : direction_[slot(page, x, y)];
    }

    void set(int x, int y, std::uint32_t g, std::uint8_t direction) {
        const std::size_t tableIndex = pageOf(x, y);
        std::uint32_t page = table_[tableIndex];
        if (page == kNoPage) {
            page = static_cast<std::uint32_t>(used_.size());
            table_[tableIndex] = page;
            used_.push_back(static_cast<std::uint32_t>(tableIndex));
            const std::size_t begin = static_cast<std::size_t>(page) * kPageArea;
            if (g_.size() < begin + kPageArea) {
                g_.resize(begin + kPageArea);
                direction_.resize(begin + kPageArea);
            }
            std::fill_n(g_.begin() + static_cast<std::ptrdiff_t>(begin), kPageArea, kUnreached);
        }
        const std::size_t i = slot(page, x, y);
        g_[i] = g;
        direction_[i] = direction;
    }

private:
    static constexpr int kPageShift = 5;
    static constexpr int kPageSize = 1 << kPageShift;
    static constexpr std::size_t kPageArea = static_cast<std::size_t>(kPageSize) * kPageSize;
    static constexpr std::uint32_t kNoPage = 0xFFFFFFFFu;

    std::size_t pageOf(int x, int y) const {
        return static_cast<std::size_t>(y >> kPageShift) * static_cast<std::size_t>(pagesX_) +
               static_cast<std::size_t>(x >> kPageShift);
    }
    static std::size_t slot(std::uint32_t page, int x, int y) {
        return static_cast<std::size_t>(page) * kPageArea +
               static_cast<std::size_t>(((y & (kPageSize - 1)) << kPageShift) | (x & (kPageSize - 1)));
    }

    int pagesX_ = 0;
    std::vector<std::uint32_t> table_; // страница карты → номер в пуле или kNoPage
    std::vector<std::uint32_t> used_;  // занятые страницы карты, по порядку выдачи
    std::vector<std::uint32_t> g_;
    std::vector<std::uint8_t> direction_;
};

// Запись открытого списка. Ключ (f, h, key) — полный порядок: при равной оценке
// раньше раскрывается то, что ближе к цели, затем — меньший key (клетка или узел).
// Поэтому порядок раскрытия не зависит от реализации кучи.
struct OpenEntry {
    std::uint32_t f = 0;
    std::uint32_t h = 0;
    std::uint32_t key = 0;

    friend auto operator<=>(const OpenEntry&, const OpenEntry&) = default;
};

class OpenList {
public:
    void clear() { heap_.clear(); }
    bool empty() const { return heap_.empty(); }
    void push(const OpenEntry& entry) {
        heap_.push_back(entry);
        std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
    }
    OpenEntry pop() {
        std::pop_heap(heap_.begin(), heap_.end(), std::greater<>{});
        const OpenEntry entry = heap_.back();
        heap_.pop_back();
        return entry;
    }

private:
    std::vector<OpenEntry> heap_;
};

struct SearchOutcome {
    bool stopped = false; // onSettle остановил поиск
    std::uint32_t expanded = 0;
};

// A* от клетки start (при нулевой оценке — Дейкстра). allowed(x, y) — можно ли
// войти в клетку карты; heuristic(x, y) — допустимая и монотонная оценка остатка;
// onSettle(cell, g) вызывается, когда стоимость пути до клетки окончательна, и
// возвращает true, чтобы остановить поиск. Раскрывается не больше budget клеток.
template <typename Allowed, typename Heuristic, typename OnSettle>
SearchOutcome gridSearch(const NavGrid& nav, SearchSpace& space, OpenList& open, Cell start,
                         std::uint32_t budget, Allowed&& allowed, Heuristic&& heuristic,
                         OnSettle&& onSettle) {
    SearchOutcome outcome;
    space.clear();
    open.clear();
    space.set(start.x, start.y, 0, kNoDirection);
    const std::uint32_t startH = heuristic(start.x, start.y);
    open.push({startH, startH, packCell(start.x, start.y)});
    while (!open.empty()) {
        const OpenEntry entry = open.pop();
        const Cell cell = unpackCell(entry.key);
        const std::uint32_t g = space.g(cell.x, cell.y);
        if (entry.f != g + entry.h) {
            continue; // устаревшая запись: до клетки уже нашли путь дешевле
        }
        if (outcome.expanded == budget) {
            break;
        }
        ++outcome.expanded;
        if (onSettle(cell, g)) {
            outcome.stopped = true;
            break;
        }
        for (int d = 0; d < 8; ++d) {
            const std::uint32_t step = nav.stepCost(cell.x, cell.y, d);
            if (step == 0) {
                continue;
            }
            const int nx = cell.x + kNavDx[d];
            const int ny = cell.y + kNavDy[d];
            if (!allowed(nx, ny)) {
                continue;
            }
            const std::uint32_t next = g + step;
            if (next < space.g(nx, ny)) {
                space.set(nx, ny, next, static_cast<std::uint8_t>(d));
                const std::uint32_t h = heuristic(nx, ny);
                open.push({next + h, h, packCell(nx, ny)});
            }
        }
    }
    return outcome;
}

// Путь от начала последнего поиска до клетки end, в порядке от начала.
inline void tracePath(const SearchSpace& space, Cell end, std::vector<Cell>& out) {
    out.clear();
    Cell cell = end;
    for (;;) {
        out.push_back(cell);
        const std::uint8_t d = space.direction(cell.x, cell.y);
        if (d == kNoDirection) {
            break;
        }
        cell = {cell.x - kNavDx[d], cell.y - kNavDy[d]};
    }
    std::reverse(out.begin(), out.end());
}

} // namespace lw::nav
