#include "render/CameraController.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

constexpr float kMinZoom = 0.02f; // пикселей на метр
constexpr float kMaxZoom = 40.0f;
constexpr float kWheelZoomStep = 0.15f;
constexpr float kKeyPanSpeed = 700.0f; // пикселей экрана в секунду

constexpr float kFovY = 45.0f; // градусы, по вертикали
constexpr float kMinDistance = 4.0f; // метры
constexpr float kMinPitch = 8.0f;    // градусы над горизонтом
constexpr float kMaxPitch = 88.0f;   // не 90: у отвесного взгляда нет «верха»
constexpr float kDefaultPitch = 55.0f;
constexpr float kMouseRotate = 0.25f; // градусов на пиксель
constexpr float kKeyRotate = 90.0f;   // градусов в секунду
constexpr float kKeyTilt = 45.0f;
constexpr float kKeyPanRate = 0.8f; // расстояний до точки в секунду

Vector2 screenCenter() {
    return {static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.5f};
}

} // namespace

CameraController::CameraController(float worldWidth, float worldHeight)
    : worldWidth_(worldWidth), worldHeight_(worldHeight) {
    reset();
    resetOrbit();
}

void CameraController::reset() {
    if (mode_ == ViewMode::Orbit) {
        resetOrbit();
        return;
    }
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    const float fit = std::min(screenWidth / worldWidth_, screenHeight / worldHeight_) * 0.9f;

    camera_.offset = screenCenter();
    camera_.target = {worldWidth_ * 0.5f, worldHeight_ * 0.5f};
    camera_.rotation = 0.0f;
    camera_.zoom = std::clamp(fit, kMinZoom, kMaxZoom);
}

void CameraController::resetOrbit() {
    orbitFocus_ = {worldWidth_ * 0.5f, worldHeight_ * 0.5f};
    yaw_ = 0.0f;
    pitch_ = kDefaultPitch;
    // Весь мир по вертикали поля зрения.
    distance_ = std::max(worldWidth_, worldHeight_) / (2.0f * std::tan(kFovY * 0.5f * DEG2RAD));
    refreshOrbit();
}

void CameraController::lookAt(float x, float y, float zoom) {
    if (mode_ == ViewMode::Orbit) {
        orbitFocus_ = {x, y};
        distance_ = distanceForZoom(zoom);
        refreshOrbit();
        return;
    }
    camera_.offset = screenCenter();
    camera_.target = {x, y};
    camera_.zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
}

void CameraController::moveTo(float x, float y) {
    if (mode_ == ViewMode::Orbit) {
        orbitFocus_ = {x, y};
        refreshOrbit();
        return;
    }
    lookAt(x, y, camera_.zoom);
}

void CameraController::setMode(ViewMode mode) {
    if (mode == mode_) {
        return;
    }
    const Vector2 center = focus();
    const float scale = zoom();
    mode_ = mode;
    lookAt(center.x, center.y, scale);
}

void CameraController::setOrbitAngles(float yawDegrees, float pitchDegrees) {
    yaw_ = yawDegrees;
    pitch_ = std::clamp(pitchDegrees, kMinPitch, kMaxPitch);
    refreshOrbit();
}

Vector2 CameraController::focus() const {
    if (mode_ == ViewMode::Orbit) {
        return orbitFocus_;
    }
    return GetScreenToWorld2D(screenCenter(), camera_);
}

float CameraController::zoom() const {
    return mode_ == ViewMode::Orbit ? zoomForDistance(distance_) : camera_.zoom;
}

float CameraController::zoomAt(Vector2 point) const {
    if (mode_ == ViewMode::TopDown) {
        return camera_.zoom;
    }
    return zoomForDistance(Vector3Distance(camera3d_.position, {point.x, 0.0f, point.y}));
}

std::optional<Vector2> CameraController::screenToWorld(Vector2 screen) const {
    if (mode_ == ViewMode::TopDown) {
        return GetScreenToWorld2D(screen, camera_);
    }
    // Луч из камеры до плоскости земли.
    const Ray ray = GetScreenToWorldRay(screen, camera3d_);
    if (ray.direction.y > -1e-4f) {
        return std::nullopt;
    }
    const float t = -ray.position.y / ray.direction.y;
    return Vector2{ray.position.x + ray.direction.x * t, ray.position.z + ray.direction.z * t};
}

std::optional<Vector2> CameraController::worldToScreen(Vector2 point, float height) const {
    if (mode_ == ViewMode::TopDown) {
        return GetWorldToScreen2D(point, camera_);
    }
    const Vector3 at = {point.x, height, point.y};
    const Vector3 forward = Vector3Normalize(Vector3Subtract(camera3d_.target, camera3d_.position));
    if (Vector3DotProduct(Vector3Subtract(at, camera3d_.position), forward) <= rlGetCullDistanceNear()) {
        return std::nullopt;
    }
    return GetWorldToScreen(at, camera3d_);
}

void CameraController::beginOverlay() const {
    if (mode_ == ViewMode::TopDown) {
        BeginMode2D(camera_);
        return;
    }
    BeginMode3D(camera3d_);
    // 2D-функции рисуют в плоскости (x, y) с z около −1: поворот вокруг оси X
    // кладёт её на землю, (x, y, z) → (x, −z, y), то есть на метр над землёй.
    rlPushMatrix();
    rlRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    // Слой — подсказка, а не часть мира: виден сквозь дома и с обеих сторон.
    rlDisableDepthTest();
    rlDisableBackfaceCulling();
}

void CameraController::endOverlay() const {
    if (mode_ == ViewMode::TopDown) {
        EndMode2D();
        return;
    }
    rlPopMatrix();
    rlDrawRenderBatchActive(); // дорисовать слой, пока отсечение граней выключено
    rlEnableBackfaceCulling();
    EndMode3D();
}

void CameraController::update(float dt, bool mouseFree, bool keyboardFree) {
    if (keyboardFree && IsKeyPressed(KEY_V)) {
        setMode(mode_ == ViewMode::Orbit ? ViewMode::TopDown : ViewMode::Orbit);
    }
    if (mode_ == ViewMode::Orbit) {
        updateOrbit(dt, mouseFree, keyboardFree);
    } else {
        updateTopDown(dt, mouseFree, keyboardFree);
    }
}

void CameraController::updateTopDown(float dt, bool mouseFree, bool keyboardFree) {
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

void CameraController::updateOrbit(float dt, bool mouseFree, bool keyboardFree) {
    const float maxDistance = 1.6f * std::max(worldWidth_, worldHeight_);
    if (mouseFree) {
        const Vector2 mouse = GetMousePosition();
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Ближе — к точке под курсором: она почти не сдвигается на экране.
            const float distance = std::clamp(distance_ * std::exp(-wheel * kWheelZoomStep),
                                              kMinDistance, maxDistance);
            if (const auto under = screenToWorld(mouse)) {
                const float pull = 1.0f - distance / distance_;
                orbitFocus_.x += (under->x - orbitFocus_.x) * pull;
                orbitFocus_.y += (under->y - orbitFocus_.y) * pull;
            }
            distance_ = distance;
        }

        const Vector2 delta = GetMouseDelta();
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            yaw_ -= delta.x * kMouseRotate;
            pitch_ += delta.y * kMouseRotate;
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            // Земля под курсором тянется за ним.
            const auto from = screenToWorld(Vector2Subtract(mouse, delta));
            const auto to = screenToWorld(mouse);
            if (from && to) {
                orbitFocus_.x += from->x - to->x;
                orbitFocus_.y += from->y - to->y;
            }
        }
    }

    if (keyboardFree) {
        float forward = 0.0f;
        float right = 0.0f;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) right -= 1.0f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) right += 1.0f;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) forward += 1.0f;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) forward -= 1.0f;
        // Вперёд — от камеры к точке по земле; вправо — поперёк.
        const float yaw = yaw_ * DEG2RAD;
        const float step = kKeyPanRate * distance_ * dt;
        orbitFocus_.x += (-std::sin(yaw) * forward + std::cos(yaw) * right) * step;
        orbitFocus_.y += (-std::cos(yaw) * forward - std::sin(yaw) * right) * step;

        if (IsKeyDown(KEY_Q)) yaw_ += kKeyRotate * dt;
        if (IsKeyDown(KEY_E)) yaw_ -= kKeyRotate * dt;
        if (IsKeyDown(KEY_R)) pitch_ += kKeyTilt * dt;
        if (IsKeyDown(KEY_F)) pitch_ -= kKeyTilt * dt;

        if (IsKeyPressed(KEY_HOME)) {
            resetOrbit();
        }
    }
    refreshOrbit();
}

void CameraController::refreshOrbit() {
    const float maxDistance = 1.6f * std::max(worldWidth_, worldHeight_);
    distance_ = std::clamp(distance_, kMinDistance, maxDistance);
    pitch_ = std::clamp(pitch_, kMinPitch, kMaxPitch);
    yaw_ = std::fmod(yaw_, 360.0f);
    orbitFocus_.x = std::clamp(orbitFocus_.x, 0.0f, worldWidth_);
    orbitFocus_.y = std::clamp(orbitFocus_.y, 0.0f, worldHeight_);

    const float yaw = yaw_ * DEG2RAD;
    const float pitch = pitch_ * DEG2RAD;
    const Vector3 target = {orbitFocus_.x, 0.0f, orbitFocus_.y};
    const Vector3 offset = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    camera3d_.target = target;
    camera3d_.position = Vector3Add(target, Vector3Scale(offset, distance_));
    camera3d_.up = {0.0f, 1.0f, 0.0f};
    camera3d_.fovy = kFovY;
    camera3d_.projection = CAMERA_PERSPECTIVE;

    if (mode_ == ViewMode::Orbit) {
        // Ближняя плоскость отодвигается с расстоянием: так хватает точности
        // глубины и вблизи, и на весь мир; дальняя — за краем мира.
        const double diagonal = std::hypot(worldWidth_, worldHeight_);
        rlSetClipPlanes(std::max(0.1, 0.01 * distance_), distance_ + 1.5 * diagonal);

        camera_.offset = screenCenter();
        camera_.target = orbitFocus_;
        camera_.rotation = 0.0f;
        camera_.zoom = zoomForDistance(distance_);
    }
}

float CameraController::distanceForZoom(float zoom) const {
    const float height = static_cast<float>(GetScreenHeight());
    return height / (2.0f * std::max(zoom, 1e-4f) * std::tan(kFovY * 0.5f * DEG2RAD));
}

float CameraController::zoomForDistance(float distance) const {
    const float height = static_cast<float>(GetScreenHeight());
    return height / (2.0f * std::max(distance, 1e-3f) * std::tan(kFovY * 0.5f * DEG2RAD));
}

} // namespace lw
