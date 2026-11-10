#pragma once

#include <cstddef>

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

// Замер приложения (этап 2а.1): старт и время кадра — для панели «Статистика».
// Шаги создания мира — в World::profiler().startup().
struct AppStats {
    double worldSeconds = 0.0;      // создание мира
    double map2dSeconds = 0.0;      // текстуры карты (вид сверху и земля в 3D)
    double renderer3dSeconds = 0.0; // рельеф и раскладка облика 3D
    // Память процесса после каждого шага старта, байты.
    std::size_t worldMemory = 0;
    std::size_t map2dMemory = 0;
    std::size_t renderer3dMemory = 0;
    double simMs = 0.0;             // симуляция в последнем кадре
    double frameMs = 0.0;           // весь кадр на процессоре, без ожидания синхронизации
    double frameMsAverage = 0.0;    // то же, сглаженное (около секунды)
    double frameMsWorst = 0.0;      // худший кадр за последнюю секунду
    int fps = 0;                    // кадров за последнюю секунду (вместе с ожиданием синхронизации)
};

// Там же — выбор вида (сверху, 3D, полёт, из-за плеча), настройки камеры и тени в 3D,
// замер систем тика, старта и памяти.
void drawStatsPanel(const World& world, const Environment& environment, const SimDriver& driver,
                    CameraController& camera, const WorldRenderer& renderer,
                    WorldRenderer3D& renderer3d, const AppStats& stats);
// Сценарий и ивенты: фаза мира, когда наступит следующая, кнопка «начать
// вспышку» / «следующая фаза» (этап 7.4); идущие ивенты (ссылки в инспектор) и
// кнопки «начать ивент» — где выберет мир или у центра экрана (этап 7.1).
// Начатый ивент открывается в инспекторе.
void drawEventsPanel(const World& world, SimDriver& driver, Inspector& inspector,
                     const CameraController& camera);

} // namespace ui
} // namespace lw
