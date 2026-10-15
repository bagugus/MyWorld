// Living World — приложение с окном.
//   living_world [--seed N] [--hours H] [--view X Y ZOOM] [--path X1 Y1 X2 Y2] [--nav-graph]
//                [--screenshot FILE.png]
//
// --hours       перед открытием окна прогнать мир на H игровых часов (посмотреть ночь, погоду)
// --view        начальное положение камеры: центр в метрах и зум в пикселях на метр
// --path        проверить поиск пути из точки (X1, Y1) в (X2, Y2), метры
// --nav-graph   показать граф навигации (дороги и местность)
// --screenshot  нарисовать несколько кадров, сохранить снимок окна и выйти

#include "render/CameraController.h"
#include "render/WeatherRenderer.h"
#include "render/WorldRenderer.h"
#include "sim/Climate.h"
#include "sim/SimDriver.h"
#include "sim/World.h"
#include "ui/PathTool.h"
#include "ui/Ui.h"

#include <raylib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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
        } else if (std::strcmp(argv[i], "--nav-graph") == 0) {
            navGraph = true;
        }
    }

    // Мир создаётся до окна: генерация карты занимает пару секунд, и пустое
    // окно в это время выглядело бы зависшим.
    lw::World world(seed);
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
        lw::WeatherRenderer weatherRenderer;
        lw::CameraController camera(world.width(), world.height());
        if (hasView) {
            camera.lookAt(viewX, viewY, viewZoom);
        }
        lw::ui::init();
        lw::ui::PathTool pathTool(world);
        pathTool.setShowGraph(navGraph);
        if (hasPath) {
            pathTool.setEndpoints(path[0], path[1], path[2], path[3]);
        }

        constexpr int kScreenshotFrame = 3; // первые кадры уходят на загрузку шрифта
        for (int frame = 0; !WindowShouldClose(); ++frame) {
            const float dt = GetFrameTime();

            lw::ui::handleHotkeys(driver);
            camera.update(dt, !lw::ui::wantsMouse(), !lw::ui::wantsKeyboard());
            pathTool.handleInput(camera, !lw::ui::wantsMouse());
            driver.advance(dt);
            weatherRenderer.update(dt, !driver.paused());
            const lw::Environment environment = lw::environmentAt(world);

            BeginDrawing();
            ClearBackground({16, 18, 22, 255});

            BeginMode2D(camera.camera());
            renderer.draw(world, camera.camera());
            EndMode2D();
            weatherRenderer.draw(environment, world.weather());
            // Отладочные слои — поверх погоды, чтобы их было видно и ночью.
            BeginMode2D(camera.camera());
            pathTool.drawOverlay(camera.camera());
            EndMode2D();

            lw::ui::beginFrame();
            lw::ui::drawSettlementLabels(world, camera);
            lw::ui::drawTimePanel(world, environment, driver);
            lw::ui::drawStatsPanel(world, environment, driver, camera, renderer);
            pathTool.drawPanel();
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
