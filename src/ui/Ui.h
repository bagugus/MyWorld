#pragma once

namespace lw {

class CameraController;
struct Environment;
class SimDriver;
class World;
class WorldRenderer;
class WorldRenderer3D;

namespace ui {

class Inspector;

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

// Время, дата, солнце, погода.
void drawTimePanel(const World& world, const Environment& environment, SimDriver& driver);
// Там же — выбор вида (сверху или 3D).
void drawStatsPanel(const World& world, const Environment& environment, const SimDriver& driver,
                    CameraController& camera, const WorldRenderer& renderer,
                    const WorldRenderer3D& renderer3d);
// Ивенты (этап 7.1): идущие ивенты (ссылки в инспектор) и кнопки «начать
// ивент» — где выберет мир или у центра экрана. Начатый ивент открывается в
// инспекторе.
void drawEventsPanel(const World& world, SimDriver& driver, Inspector& inspector,
                     const CameraController& camera);

} // namespace ui
} // namespace lw
