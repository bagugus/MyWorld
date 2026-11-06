#pragma once

struct ImFont;

namespace lw {

class CameraController;
struct CameraSettings;
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

// Шрифты титров режима «Сериал» (этап 12а.7): заголовки (Georgia), надписи
// монтажа (Georgia курсив) и текст (Segoe UI Light); если их нет в системе —
// шрифт интерфейса. Размер — любой (ImDrawList::AddText с размером).
ImFont* titleFont();
ImFont* italicFont();
ImFont* captionFont();

// Между beginFrame и endFrame рисуются панели (внутри BeginDrawing/EndDrawing).
void beginFrame();
void endFrame();

// Интерфейс забрал мышь / клавиатуру — мир на этот ввод реагировать не должен.
bool wantsMouse();
bool wantsKeyboard();

// Space — пауза, 1/2/3 — скорость x1/x10/x100, F1 — окно «Клавиши».
void handleHotkeys(SimDriver& driver);

// Настройки камеры из camera.ini (если файла нет — остаются как есть) и
// запись туда, когда они изменились.
void loadCameraSettings(CameraSettings& settings);
void saveCameraSettings(const CameraSettings& settings);
// Окно «Клавиши» (F1): управление камерой в текущем виде и общие клавиши.
void drawKeyHelp(const CameraController& camera);

// Названия поселений поверх карты, под панелями.
void drawSettlementLabels(const World& world, const CameraController& camera);

// Время, дата, солнце, погода.
void drawTimePanel(const World& world, const Environment& environment, SimDriver& driver);
// Там же — выбор вида (сверху, 3D, полёт, из-за плеча), настройки камеры и тени в 3D.
void drawStatsPanel(const World& world, const Environment& environment, const SimDriver& driver,
                    CameraController& camera, const WorldRenderer& renderer,
                    WorldRenderer3D& renderer3d);
// Сценарий и ивенты: фаза мира, когда наступит следующая, кнопка «начать
// вспышку» / «следующая фаза» (этап 7.4); идущие ивенты (ссылки в инспектор) и
// кнопки «начать ивент» — где выберет мир или у центра экрана (этап 7.1).
// Начатый ивент открывается в инспекторе.
void drawEventsPanel(const World& world, SimDriver& driver, Inspector& inspector,
                     const CameraController& camera);

} // namespace ui
} // namespace lw
