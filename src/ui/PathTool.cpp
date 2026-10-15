#include "ui/PathTool.h"

#include "render/CameraController.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>

namespace lw::ui {

namespace {

constexpr int kPickRadius = 12; // клеток: клик по воде или дому ищет проходимую рядом
// Скорость пешехода по дороге для оценки времени в пути; своя у движения (этап 4).
constexpr float kWalkSpeed = 1.4f;

constexpr Color kRoadColor = {80, 220, 255, 220};
constexpr Color kLandColor = {235, 235, 210, 110};
constexpr Color kWaterColor = {70, 120, 255, 200};
constexpr Color kCellsColor = {255, 255, 255, 110};
constexpr Color kPathColor = {255, 70, 200, 255};
constexpr Color kStartColor = {60, 230, 90, 255};
constexpr Color kGoalColor = {240, 60, 60, 255};

Vector2 cellCenter(Cell cell) {
    return {Map::cellCenter(cell.x), Map::cellCenter(cell.y)};
}

Color nodeColor(NavClass kind) {
    return kind == NavClass::Road ? kRoadColor : kind == NavClass::Water ? kWaterColor : kLandColor;
}

const char* methodName(PathMethod method) {
    switch (method) {
    case PathMethod::Direct:
        return "напрямую (A* по сетке)";
    case PathMethod::Graph:
        return "по графу навигации";
    case PathMethod::Full:
        return "A* по всей карте";
    case PathMethod::None:
        break;
    }
    return "—";
}

} // namespace

PathTool::PathTool(const World& world) : world_(world), pathfinder_(world.nav(), world.navGraph()) {}

std::optional<Cell> PathTool::pick(float x, float y) const {
    return world_.nav().nearestPassable(world_.map().clampedCellAt(x, y), kPickRadius);
}

void PathTool::setEndpoints(float startX, float startY, float goalX, float goalY) {
    start_ = pick(startX, startY);
    goal_ = pick(goalX, goalY);
    recompute();
}

void PathTool::handleInput(const CameraController& camera, bool mouseFree) {
    if (!enabled_ || !mouseFree || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return;
    }
    const Vector2 point = GetScreenToWorld2D(GetMousePosition(), camera.camera());
    const std::optional<Cell> cell = pick(point.x, point.y);
    if (!cell) {
        return;
    }
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
        goal_ = cell;
    } else {
        start_ = cell;
    }
    recompute();
}

void PathTool::recompute() {
    path_.reset();
    if (!start_ || !goal_) {
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    path_ = pathfinder_.find(*start_, *goal_);
    searchMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void PathTool::drawOverlay(const Camera2D& camera) const {
    const float px = 1.0f / camera.zoom; // метров в пикселе экрана
    if (showGraph_) {
        const Vector2 topLeft = GetScreenToWorld2D({0.0f, 0.0f}, camera);
        const Vector2 bottomRight = GetScreenToWorld2D(
            {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())}, camera);
        const float margin = NavGraph::kSectorSize * 2 * Map::kCellSize;
        const auto visible = [&](Vector2 p) {
            return p.x > topLeft.x - margin && p.x < bottomRight.x + margin &&
                   p.y > topLeft.y - margin && p.y < bottomRight.y + margin;
        };
        // Рёбра между дорогами — цветом дороги и толще: это граф дорог.
        const NavGraph& graph = world_.navGraph();
        const std::vector<NavGraph::Node>& nodes = graph.nodes();
        for (std::uint32_t i = 0; i < nodes.size(); ++i) {
            const Vector2 from = cellCenter(nodes[i].cell);
            if (!visible(from)) {
                continue;
            }
            for (const NavGraph::Edge& edge : graph.edges(i)) {
                if (edge.to < i) {
                    continue;
                }
                const bool road = nodes[i].kind == NavClass::Road && nodes[edge.to].kind == NavClass::Road;
                DrawLineEx(from, cellCenter(nodes[edge.to].cell), (road ? 2.5f : 1.0f) * px,
                           road ? kRoadColor : kLandColor);
            }
        }
        for (const NavGraph::Node& node : nodes) {
            const Vector2 center = cellCenter(node.cell);
            if (visible(center)) {
                DrawCircleV(center, (node.kind == NavClass::Land ? 2.0f : 3.0f) * px, nodeColor(node.kind));
            }
        }
    }

    if (path_ && path_->found()) {
        const std::vector<Cell>& cells = path_->cells;
        for (std::size_t i = 1; i < cells.size(); ++i) {
            DrawLineEx(cellCenter(cells[i - 1]), cellCenter(cells[i]), 1.5f * px, kCellsColor);
        }
        const std::vector<Cell>& waypoints = path_->waypoints;
        for (std::size_t i = 1; i < waypoints.size(); ++i) {
            DrawLineEx(cellCenter(waypoints[i - 1]), cellCenter(waypoints[i]), 3.0f * px, kPathColor);
        }
        for (const Cell waypoint : waypoints) {
            DrawCircleV(cellCenter(waypoint), 3.0f * px, kPathColor);
        }
    }
    const float marker = std::max(Map::kCellSize * 0.6f, 7.0f * px);
    if (start_) {
        DrawCircleV(cellCenter(*start_), marker, kStartColor);
    }
    if (goal_) {
        DrawCircleV(cellCenter(*goal_), marker, kGoalColor);
    }
}

void PathTool::drawPanel() {
    // Справа под статистикой. Привязка к верхнему краю: панель растёт вниз, когда
    // после поиска в ней появляются строки.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 12.0f, 420.0f}, ImGuiCond_FirstUseEver, {1.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({320.0f, 0.0f}, {FLT_MAX, FLT_MAX});
    if (ImGui::Begin("Поиск пути###path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("ЛКМ — начало, Shift+ЛКМ — цель", &enabled_);
        const NavGraph& graph = world_.navGraph();
        ImGui::Checkbox("Граф навигации", &showGraph_);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu узлов, %zu рёбер", graph.nodeCount(), graph.edgeCount() / 2);
        ImGui::TextDisabled("Голубой — дороги, белый — суша, синий — реки");

        ImGui::Separator();
        if (!path_) {
            ImGui::TextDisabled(start_ ? "Выберите цель" : "Выберите начало и цель");
        } else {
            switch (path_->status) {
            case PathStatus::Found: {
                const Path& path = *path_;
                const float roadMeters =
                    static_cast<float>(path.cost) / static_cast<float>(NavGrid::kCostPerRoadMeter);
                const int gameMinutes = static_cast<int>(roadMeters / kWalkSpeed *
                                                         static_cast<float>(kGameTimeScale) / 60.0f);
                ImGui::Text("Найден: %s", methodName(path.method));
                ImGui::Text("Длина %.0f м, точек поворота %zu", path.length, path.waypoints.size());
                ImGui::Text("Стоимость — как %.0f м по дороге", roadMeters);
                ImGui::Text("Пешком около %d ч %02d мин игрового времени", gameMinutes / 60,
                            gameMinutes % 60);
                ImGui::Text("Раскрыто клеток %u, узлов графа %u", path.expanded, path.graphExpanded);
                if (path.method == PathMethod::Graph) {
                    ImGui::Text("Узлов маршрута по дорогам: %u", path.roadNodes);
                }
                break;
            }
            case PathStatus::Blocked:
                ImGui::Text("Начало или цель непроходимы");
                break;
            case PathStatus::Unreachable:
                ImGui::Text("Пути нет: точки разделены водой");
                break;
            case PathStatus::NotFound:
                ImGui::Text("Путь не найден");
                break;
            }
            ImGui::Text("Поиск: %.2f мс", searchMs_);
        }
        if (ImGui::Button("Сбросить")) {
            start_.reset();
            goal_.reset();
            path_.reset();
        }
    }
    ImGui::End();
}

} // namespace lw::ui
