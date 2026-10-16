// Living World — приложение с окном.
//   living_world [--seed N] [--event NAME] [--hours H] [--view X Y ZOOM] [--path X1 Y1 X2 Y2]
//                [--nav-graph] [--select ID] [--3d] [--orbit YAW PITCH] [--screenshot FILE.png]
//
// --event       начать ивент на старте (drop — сброс груза) и открыть его в инспекторе;
//               --hours прокручивает мир уже после него
//
// --hours       перед открытием окна прогнать мир на H игровых часов (посмотреть ночь, погоду)
// --view        начальное положение камеры: центр в метрах и зум в пикселях на метр
// --path        проверить поиск пути из точки (X1, Y1) в (X2, Y2), метры
// --nav-graph   показать граф навигации (дороги и местность)
// --select      открыть объект в инспекторе и следить за ним камерой
// --3d          начать с 3D-вида (V переключает вид); --view задаёт точку и зум и в 3D
// --orbit       3D-вид под углом: YAW — с какой стороны (0 — с юга, 90 — с востока),
//               PITCH — наклон над горизонтом, градусы
// --screenshot  нарисовать несколько кадров, сохранить снимок окна и выйти

#include "render/CameraController.h"
#include "render/Palette.h"
#include "render/WeatherRenderer.h"
#include "render/WorldRenderer.h"
#include "render/WorldRenderer3D.h"
#include "sim/Climate.h"
#include "sim/SimDriver.h"
#include "sim/Events.h"
#include "sim/World.h"
#include "sim/systems/EventSystem.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"
#include "ui/Inspector.h"
#include "ui/PathTool.h"
#include "ui/Ui.h"

#include <raylib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    double hours = 0.0;
    const char* screenshot = nullptr;
    bool hasView = false;
    float viewX = 0.0f;
    float viewY = 0.0f;
    float viewZoom = 1.0f;
    bool hasPath = false;
    float path[4] = {};
    bool navGraph = false;
    std::uint64_t selectId = 0;
    bool view3d = false;
    std::optional<std::pair<float, float>> orbit;
    std::optional<lw::EventKind> event;
    lw::WorldConfig config;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--hours") == 0 && i + 1 < argc) {
            hours = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--view") == 0 && i + 3 < argc) {
            viewX = std::strtof(argv[++i], nullptr);
            viewY = std::strtof(argv[++i], nullptr);
            viewZoom = std::strtof(argv[++i], nullptr);
            hasView = true;
        } else if (std::strcmp(argv[i], "--path") == 0 && i + 4 < argc) {
            for (float& value : path) {
                value = std::strtof(argv[++i], nullptr);
            }
            hasPath = true;
        } else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            selectId = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--event") == 0 && i + 1 < argc) {
            event = lw::eventKindByName(argv[++i]);
        } else if (std::strcmp(argv[i], "--3d") == 0) {
            view3d = true;
        } else if (std::strcmp(argv[i], "--orbit") == 0 && i + 2 < argc) {
            const float yaw = std::strtof(argv[++i], nullptr);
            orbit = {{yaw, std::strtof(argv[++i], nullptr)}};
            view3d = true;
        } else if (std::strcmp(argv[i], "--nav-graph") == 0) {
            navGraph = true;
        } else if (std::strcmp(argv[i], "--zombies") == 0 && i + 1 < argc) {
            config.zombies = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-peace") == 0) {
            config.peacetime = false;
        }
    }

    // Мир создаётся до окна: генерация карты занимает пару секунд, и пустое
    // окно в это время выглядело бы зависшим.
    lw::World world(seed, config);
    if (event) {
        const lw::EntityId id = lw::startEvent(world, *event);
        if (selectId == 0) {
            selectId = id;
        }
    }
    const auto skipTicks = static_cast<std::uint64_t>(std::max(0.0, hours) * lw::kTicksPerGameHour);
    for (std::uint64_t i = 0; i < skipTicks; ++i) {
        world.step();
    }
    lw::SimDriver driver(world);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(1600, 900, "Living World");
    SetExitKey(KEY_NULL); // Esc не закрывает окно

    {
        // Текстуры карты должны быть освобождены до CloseWindow.
        lw::WorldRenderer renderer(world);
        lw::WorldRenderer3D renderer3d(renderer.map(), world.map());
        lw::WeatherRenderer weatherRenderer;
        lw::CameraController camera(world.width(), world.height());
        camera.setTerrain(&renderer3d.terrain());
        if (orbit) {
            camera.setOrbitAngles(orbit->first, orbit->second);
        }
        if (view3d) {
            camera.setMode(lw::ViewMode::Orbit);
        }
        if (hasView) {
            camera.lookAt(viewX, viewY, viewZoom);
        }
        lw::ui::init();
        lw::ui::PathTool pathTool(world);
        pathTool.setShowGraph(navGraph);
        if (hasPath) {
            pathTool.setEndpoints(path[0], path[1], path[2], path[3]);
        }
        lw::ui::Inspector inspector(world);
        if (selectId != 0) {
            inspector.select(selectId);
            inspector.setFollow(true);
        }

        constexpr int kScreenshotFrame = 3; // первые кадры уходят на загрузку шрифта
        for (int frame = 0; !WindowShouldClose(); ++frame) {
            const float dt = GetFrameTime();

            lw::ui::handleHotkeys(driver);
            camera.update(dt, !lw::ui::wantsMouse(), !lw::ui::wantsKeyboard());
            pathTool.handleInput(camera, !lw::ui::wantsMouse());
            inspector.handleInput(camera, !lw::ui::wantsMouse());
            driver.advance(dt);
            const float alpha = driver.interpolationAlpha();
            inspector.updateCamera(camera, alpha);
            weatherRenderer.update(dt, !driver.paused());
            const lw::Environment environment = lw::environmentAt(world);

            BeginDrawing();
            if (camera.orbit()) {
                ClearBackground(lw::kSkyColor);
                renderer3d.draw(world, camera, alpha);
            } else {
                ClearBackground({16, 18, 22, 255});
                BeginMode2D(camera.camera());
                renderer.draw(world, camera.camera(), alpha);
                EndMode2D();
            }
            weatherRenderer.draw(environment, world.weather());
            // Отладочные слои — поверх погоды, чтобы их было видно и ночью.
            camera.beginOverlay();
            pathTool.drawOverlay(camera.camera());
            inspector.drawOverlay(camera.camera(), alpha);
            camera.endOverlay();

            lw::ui::beginFrame();
            lw::ui::setNameSource(&world);
            lw::ui::drawSettlementLabels(world, camera);
            lw::ui::drawTimePanel(world, environment, driver);
            lw::ui::drawStatsPanel(world, environment, driver, camera, renderer, renderer3d);
            pathTool.drawPanel();
            inspector.drawPanel();
            lw::ui::drawEventsPanel(world, driver, inspector, camera);
            lw::ui::drawChroniclePanel(world, inspector);
            lw::ui::endFrame();

            EndDrawing();

            if (screenshot != nullptr && frame == kScreenshotFrame) {
                const Image image = LoadImageFromScreen();
                ExportImage(image, screenshot);
                UnloadImage(image);
                break;
            }
        }

        lw::ui::shutdown();
    }
    CloseWindow();
    return 0;
}
