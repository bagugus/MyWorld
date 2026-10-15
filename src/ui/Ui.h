#pragma once

namespace lw {

class CameraController;
class SimDriver;
class World;
class WorldRenderer;

namespace ui {

// Вызывать после InitWindow / перед CloseWindow.
void init();
void shutdown();

// Между beginFrame и endFrame рисуются панели (внутри BeginDrawing/EndDrawing).
void beginFrame();
void endFrame();

// Интерфейс забрал мышь / клавиатуру — мир на этот ввод реагировать не должен.
bool wantsMouse();
bool wantsKeyboard();

// Space — пауза, 1/2/3 — скорость x1/x10/x100.
void handleHotkeys(SimDriver& driver);

// Названия поселений поверх карты, под панелями.
void drawSettlementLabels(const World& world, const CameraController& camera);

void drawTimePanel(const World& world, SimDriver& driver);
void drawStatsPanel(const World& world, const SimDriver& driver, const CameraController& camera,
                    const WorldRenderer& renderer);

} // namespace ui
} // namespace lw
