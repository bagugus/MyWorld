#include "render/CameraController.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

constexpr float kMinZoom = 0.02f; // пикселей на метр
constexpr float kMaxZoom = 40.0f;
constexpr float kWheelZoomStep = 0.15f;
constexpr float kKeyPanSpeed = 700.0f; // пикселей экрана в секунду

} // namespace

CameraController::CameraController(float worldWidth, float worldHeight)
    : worldWidth_(worldWidth), worldHeight_(worldHeight) {
    reset();
}

void CameraController::reset() {
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    const float fit = std::min(screenWidth / worldWidth_, screenHeight / worldHeight_) * 0.9f;

    camera_.offset = {screenWidth * 0.5f, screenHeight * 0.5f};
    camera_.target = {worldWidth_ * 0.5f, worldHeight_ * 0.5f};
    camera_.rotation = 0.0f;
    camera_.zoom = std::clamp(fit, kMinZoom, kMaxZoom);
}

void CameraController::lookAt(float x, float y, float zoom) {
    camera_.offset = {static_cast<float>(GetScreenWidth()) * 0.5f,
                      static_cast<float>(GetScreenHeight()) * 0.5f};
    camera_.target = {x, y};
    camera_.zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
}

void CameraController::update(float dt, bool mouseFree, bool keyboardFree) {
    if (mouseFree) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Точка мира под курсором остаётся под курсором.
            const Vector2 mouse = GetMousePosition();
            camera_.target = GetScreenToWorld2D(mouse, camera_);
            camera_.offset = mouse;
            camera_.zoom =
                std::clamp(camera_.zoom * std::exp(wheel * kWheelZoomStep), kMinZoom, kMaxZoom);
        }

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            const Vector2 delta = GetMouseDelta();
            camera_.target.x -= delta.x / camera_.zoom;
            camera_.target.y -= delta.y / camera_.zoom;
        }
    }

    if (keyboardFree) {
        float dx = 0.0f;
        float dy = 0.0f;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) dx -= 1.0f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) dx += 1.0f;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) dy -= 1.0f;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) dy += 1.0f;

        // Скорость постоянна в пикселях экрана, поэтому в метрах зависит от зума.
        const float step = kKeyPanSpeed * dt / camera_.zoom;
        camera_.target.x += dx * step;
        camera_.target.y += dy * step;

        if (IsKeyPressed(KEY_HOME)) {
            reset();
        }
    }
}

} // namespace lw
