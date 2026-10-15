// Living World — приложение с окном.
//   living_world [--seed N] [--hours H] [--view X Y ZOOM] [--screenshot FILE.png]
//
// --hours       перед открытием окна прогнать мир на H игровых часов (посмотреть ночь, погоду)
// --view        начальное положение камеры: центр в метрах и зум в пикселях на метр
// --screenshot  нарисовать несколько кадров, сохранить снимок окна и выйти

#include "render/CameraController.h"
#include "render/WeatherRenderer.h"
#include "render/WorldRenderer.h"
#include "sim/Climate.h"
#include "sim/SimDriver.h"
#include "sim/World.h"
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

        constexpr int kScreenshotFrame = 3; // первые кадры уходят на загрузку шрифта
        for (int frame = 0; !WindowShouldClose(); ++frame) {
            const float dt = GetFrameTime();

            lw::ui::handleHotkeys(driver);
            camera.update(dt, !lw::ui::wantsMouse(), !lw::ui::wantsKeyboard());
            driver.advance(dt);
            weatherRenderer.update(dt, !driver.paused());
            const lw::Environment environment = lw::environmentAt(world);

            BeginDrawing();
            ClearBackground({16, 18, 22, 255});

            BeginMode2D(camera.camera());
            renderer.draw(world, camera.camera());
            EndMode2D();
            weatherRenderer.draw(environment, world.weather());

            lw::ui::beginFrame();
            lw::ui::drawSettlementLabels(world, camera);
            lw::ui::drawTimePanel(world, environment, driver);
            lw::ui::drawStatsPanel(world, environment, driver, camera, renderer);
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
