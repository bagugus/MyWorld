#pragma once

#include "sim/Map.h"
#include "sim/nav/NavGrid.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lw {

// Граф навигации — верхний уровень поиска дальних путей: дороги и местность.
//
// Карта делится на секторы kSectorSize × kSectorSize клеток. Связный по сторонам
// кусок клеток одного вида (дорога, суша, река) внутри сектора — узел графа.
// Клетка узла — клетка куска, ближайшая к его центру; стоимость узла — средняя
// стоимость его клеток. Ребро связывает куски, которые соприкасаются: соседние
// клетки по стороне (внутри сектора и через его границу) или диагональный шаг
// через общий угол четырёх секторов. Без диагоналей путь по местности наискосок
// шёл бы лесенкой из секторов и оценивался бы на 41 % дороже, чем есть.
//
// Стоимость ребра — оценка пути «клетка узла → точка касания → клетка соседа»:
// каждая половина — по прямой с ценой своего куска, шаг через границу — точно.
// Из всех точек касания берётся самая дешёвая. Оценка не меньше октильного
// расстояния по дороге, поэтому A* по графу с такой оценкой остатка корректен.
//
// Дороги — отдельные узлы с ценой дороги, а не часть кусков суши: это и есть
// граф дорог, по которому дальние пути идут, если так дешевле. Остальные узлы
// нужны, чтобы граф видел и путь напрямик по местности, и обход озёр и моря.
// Куски связны внутри своих секторов, поэтому вдоль любого маршрута по графу в
// секторах его узлов есть путь по сетке.
//
// Производные данные, как и NavGrid: строятся из сетки и в хеш мира не входят.
class NavGraph {
public:
    static constexpr int kSectorShift = 4;
    static constexpr int kSectorSize = 1 << kSectorShift; // 16 клеток, 64 м
    static constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;

    struct Node {
        Cell cell;
        NavClass kind = NavClass::Land;
        std::uint8_t cost = 0; // средняя стоимость клеток куска
        std::uint32_t firstEdge = 0;
        std::uint32_t edgeCount = 0;
    };
    struct Edge {
        std::uint32_t to = 0;
        std::uint32_t cost = 0;
    };

    void build(const NavGrid& nav);

    // Узлы — по секторам в порядке строк, внутри сектора — по первой клетке куска.
    const std::vector<Node>& nodes() const { return nodes_; }
    std::size_t nodeCount() const { return nodes_.size(); }
    // Рёбра хранятся в обе стороны; у узла — по возрастанию номера соседа.
    std::size_t edgeCount() const { return edges_.size(); }
    std::span<const Edge> edges(std::uint32_t node) const {
        const Node& n = nodes_[node];
        return {edges_.data() + n.firstEdge, n.edgeCount};
    }

    // Узел куска, в котором лежит клетка; kNoNode, если она непроходима.
    // nav — та же сетка, по которой граф построен.
    std::uint32_t nodeOf(const NavGrid& nav, Cell cell) const;

    int sectorsX() const { return sectorsX_; }
    int sectorsY() const { return sectorsY_; }

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    // Узлы сектора s — [sectorFirst_[s], sectorFirst_[s + 1]).
    std::vector<std::uint32_t> sectorFirst_;
    int sectorsX_ = 0;
    int sectorsY_ = 0;
};

} // namespace lw
