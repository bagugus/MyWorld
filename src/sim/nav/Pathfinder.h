#pragma once

#include "sim/Map.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/SearchSpace.h"

#include <cstdint>
#include <vector>

namespace lw {

class NavGraph;

enum class PathStatus : std::uint8_t {
    Found,
    Blocked,     // начало или цель непроходимы: вода, здание, край карты
    Unreachable, // клетки в разных областях: пути нет
    NotFound,    // поиск сдался; не случается, пока связность областей известна
};

// Как найден путь.
enum class PathMethod : std::uint8_t {
    None,
    Direct, // A* по сетке напрямую: путь оптимален
    Graph,  // маршрут по графу навигации, затем A* по сетке вдоль него
    Full,   // запасной вариант: A* по всей карте без ограничений
};

struct Path {
    PathStatus status = PathStatus::NotFound;
    PathMethod method = PathMethod::None;
    // Клетки по порядку от начала до цели; соседние смежны стороной или углом.
    std::vector<Cell> cells;
    // Сглаженный путь: начало, точки поворота, цель. Между соседними точками —
    // прямая, все клетки под ней проходимы.
    std::vector<Cell> waypoints;
    // Стоимость пути по cells (единицы NavGrid). Сглаженный путь, по оценке, не дороже.
    std::uint32_t cost = 0;
    // Длина пути по waypoints, метры.
    float length = 0.0f;
    // Цена поиска — для отладки и замеров.
    std::uint32_t expanded = 0;      // раскрыто клеток во всех поисках по сетке
    std::uint32_t graphExpanded = 0; // раскрыто узлов графа навигации
    std::uint32_t roadNodes = 0;     // узлов-дорог в маршруте по графу

    bool found() const { return status == PathStatus::Found; }
};

// Настройки поиска. Значения по умолчанию подобраны под карту по умолчанию
// (lw_headless --paths); тесты меняют их, чтобы проверить все ветки на маленьких картах.
struct PathfinderSettings {
    // Если цель ближе этого (клетки по каждой оси), сначала пробуется прямой A*.
    int directRange = 160;
    // Сколько клеток может раскрыть прямой A*, прежде чем уступить графу.
    std::uint32_t directBudget = 60'000;
    // Насколько коридор вокруг маршрута по графу шире секторов маршрута, клетки.
    int corridorMargin = 8;
    // Промежуточные цели A* вдоль маршрута — примерно через столько клеток.
    int refineSpan = 48;
    // Самый длинный отрезок сглаженного пути, клетки исходного пути.
    int smoothSpan = 32;
};

// Поиск пути пешком по NavGrid.
//
// Близкие цели — оптимальный A* по сетке с ограничением числа раскрытых клеток.
// Дальние (и близкие, если прямой поиск не уложился) — через граф навигации:
// A* по графу выбирает маршрут из кусков дорог, суши и рек, затем A* по сетке
// уточняет путь участками внутри коридора из секторов маршрута. Последний
// запасной вариант — A* по всей карте.
//
// Ответ зависит только от сетки, графа, настроек и запроса — не от предыдущих
// запросов. Все стоимости целые, порядок раскрытия задан ключом записи.
// Держит рабочие буферы, поэтому один Pathfinder нельзя звать из нескольких
// потоков сразу; сетку и граф несколько Pathfinder'ов могут делить.
class Pathfinder {
public:
    Pathfinder(const NavGrid& nav, const NavGraph& graph, const PathfinderSettings& settings = {});

    Path find(Cell start, Cell goal);

    const PathfinderSettings& settings() const { return settings_; }

private:
    // Узел графа и оценка пути между ним и началом (или целью).
    struct Access {
        std::uint32_t node;
        std::uint32_t cost;
    };

    void prepare();
    // Оптимальный A* по всей сетке; не больше budget раскрытых клеток.
    bool searchDirect(Cell start, Cell goal, std::uint32_t budget, Path& path);
    bool searchGraph(Cell start, Cell goal, Path& path);
    // Узел куска, где лежит клетка, и его соседи — с оценкой пути от клетки.
    void collectAccess(Cell cell, std::vector<Access>& out) const;
    void markCorridor(Cell start, Cell goal);
    void markSector(Cell cell);
    // Точный путь вдоль маршрута по графу: A* в коридоре от одной промежуточной
    // цели до другой.
    bool refineRoute(Cell start, Cell goal, Path& path);
    void smooth(Path& path);
    bool straightIsNoWorse(Cell a, Cell b, std::uint8_t worst, std::uint32_t pathCost) const;

    const NavGrid& nav_;
    const NavGraph& graph_;
    PathfinderSettings settings_;

    int preparedWidth_ = -1;
    int preparedHeight_ = -1;
    nav::SearchSpace space_;
    nav::OpenList open_;
    std::vector<Access> starts_;
    std::vector<Access> exits_;
    std::vector<std::uint32_t> graphCost_;
    std::vector<std::uint32_t> graphParent_;
    std::vector<std::uint32_t> route_; // узлы графа от начала к цели
    // Коридор — блоки 8 × 8 клеток, по которым идёт уточнение маршрута.
    int blocksX_ = 0;
    int blocksY_ = 0;
    std::vector<std::uint8_t> corridor_;
    std::vector<std::uint32_t> corridorUsed_;
    std::vector<Cell> piece_;
    std::vector<std::uint32_t> prefixCost_;
};

} // namespace lw
