#pragma once

#include "sim/Map.h"
#include "sim/nav/Pathfinder.h"

#include <raylib.h>

#include <optional>

namespace lw {

class CameraController;
class World;

namespace ui {

// Проверка поиска пути: ЛКМ — начало, Shift+ЛКМ — цель. Путь и граф дорог
// рисуются поверх карты, в панели — длина, стоимость и цена поиска.
// У инструмента свой Pathfinder: мир он только читает.
class PathTool {
public:
    explicit PathTool(const World& world);

    // Начало и цель в метрах (для --path); непроходимая точка сдвигается к ближайшей проходимой.
    void setEndpoints(float startX, float startY, float goalX, float goalY);
    void setShowGraph(bool show) { showGraph_ = show; }

    void handleInput(const CameraController& camera, bool mouseFree);
    // Поверх всего мира, между BeginMode2D и EndMode2D.
    void drawOverlay(const Camera2D& camera) const;
    void drawPanel();

private:
    // Проходимая клетка под точкой мира или ближайшая к ней.
    std::optional<Cell> pick(float x, float y) const;
    void recompute();

    const World& world_;
    Pathfinder pathfinder_;
    bool enabled_ = true;
    bool showGraph_ = false;
    std::optional<Cell> start_;
    std::optional<Cell> goal_;
    std::optional<Path> path_;
    double searchMs_ = 0.0;
};

} // namespace ui
} // namespace lw
