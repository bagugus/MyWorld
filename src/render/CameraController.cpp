#include "render/CameraController.h"

#include "render/Terrain3D.h"
#include "render/TreeLayout.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lw {

namespace {

constexpr float kMinZoom = 0.02f; // пикселей на метр
constexpr float kMaxZoom = 40.0f;
constexpr float kWheelZoomStep = 0.15f;
constexpr float kKeyPanSpeed = 700.0f; // пикселей экрана в секунду
constexpr float kAccelTime = 0.1f;     // секунд: разгон панорамы и полёта клавишами

constexpr float kFovY = 45.0f; // градусы, по вертикали
constexpr float kMinDistance = 4.0f; // метры
constexpr float kMinPitch = 8.0f;    // градусы над горизонтом
constexpr float kMaxPitch = 88.0f;   // не 90: у отвесного взгляда нет «верха»
// Почти отвесно: так 3D смотрит при переходе из вида сверху и обратно.
constexpr float kTopPitch = 89.9f;
constexpr float kDefaultPitch = 55.0f;
constexpr float kMouseRotate = 0.25f; // градусов на пиксель
constexpr float kKeyRotate = 90.0f;   // градусов в секунду
constexpr float kKeyTilt = 45.0f;
constexpr float kKeyPanRate = 0.8f; // расстояний до точки в секунду
// Ближняя плоскость отсечения не ближе стольких метров (см. setClipPlanes).
constexpr double kMinNearPlane = 0.1;
constexpr float kMinEyeHeight = 1.7f; // метров над поверхностью: ниже глаз не опускается

// Полёт: скорость — столько высот над рельефом в секунду (от 4 до 2000 м высоты).
constexpr float kFlyRate = 1.5f;
constexpr float kFlyBoost = 4.0f; // Shift
constexpr float kMaxFlyPitch = 89.0f;

// Из-за плеча.
constexpr float kShoulderSide = 0.45f; // метров вправо от головы
constexpr float kShoulderAhead = 2.0f; // взгляд — на столько метров впереди
constexpr float kShoulderMinDistance = 1.5f;
constexpr float kShoulderMaxDistance = 30.0f;
constexpr float kShoulderTurn = 4.0f; // курс существа догоняется вчетверо медленнее обычного

// Кино (12а.7).
constexpr float kCinemaClearance = 0.6f;   // метров: луч взгляда не ближе этого к земле
constexpr float kCinemaWallGap = 0.4f;     // метров: глаз не ближе этого к стене
constexpr float kCinemaMinDistance = 1.2f; // метров от точки взгляда
constexpr float kCinemaMinPitch = -30.0f;  // нижний ракурс
constexpr float kCinemaMaxPitch = 82.0f;
// В комнате глаз не ближе этого к стенам и полу, метры (12а.14).
constexpr float kCinemaRoomGap = 0.2f;
// Препятствие между глазом и точкой взгляда: сперва камера поднимается над
// ним на столько градусов наклона (по очереди), если не помогло — подъезжает ближе.
constexpr float kCinemaLifts[] = {0.0f, 8.0f, 16.0f, 26.0f, 38.0f, 52.0f};
constexpr float kCinemaLiftUp = 0.25f;  // секунд: подъём над препятствием
constexpr float kCinemaLiftDown = 1.5f; // секунд: обратно
constexpr float kCinemaRelease = 1.2f;  // секунд: отъезд обратно после подъезда
// Вид заслонён: подъём выше этого (градусы) или камера ближе этой доли расстояния.
constexpr float kCinemaBlockedLift = 6.0f;
constexpr float kCinemaBlockedReach = 0.6f;
// Деревья: крона непрозрачна — сквозь листву луч проходит не больше
// kFoliageSeeThrough метров; выше kTreeTop над землёй деревьев нет; луч
// проверяется шагами kFoliageStep.
constexpr float kFoliageSeeThrough = 1.0f;
constexpr float kTreeTop = 32.0f;
constexpr float kFoliageStep = 0.8f;
// Перед объективом на kLensClear метров веток быть не должно — это обязательно.
// Листва дальше закрывает героя (в ельнике кроны окружают его со всех сторон):
// это лишь оценка ракурса, ближе kFoliageSkip к точке взгляда — не считается.
constexpr float kLensClear = 8.0f;
constexpr float kFoliageSkip = 2.0f;

// Радиус кроны дерева на высоте y над его корнем (0 — там кроны нет): ель —
// конус почти от земли, сосна — верхняя треть, лиственные и берёза — верхняя
// половина (как модели render/Trees3D). Кусты камере не мешают.
float crownRadiusAt(const TreeSpec& tree, float y) {
    const float h = tree.height;
    if (y > h || y < 0.0f) {
        return 0.0f;
    }
    switch (tree.kind) {
    case TreeKind::Spruce:
        return y < 0.05f * h ? 0.0f : tree.crown * (1.0f - (y - 0.05f * h) / (0.95f * h));
    case TreeKind::Pine:
        return y < 0.58f * h ? 0.0f : tree.crown * (1.0f - 0.5f * (y - 0.58f * h) / (0.42f * h));
    case TreeKind::Broadleaf:
        return y < 0.45f * h ? 0.0f : tree.crown * 1.1f;
    case TreeKind::Birch:
        return y < 0.45f * h ? 0.0f : tree.crown * 0.9f;
    case TreeKind::Bush:
        return 0.0f;
    }
    return 0.0f;
}

Vector2 screenCenter() {
    return {static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.5f};
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Зум и расстояние сглаживаются в логарифме: шаг колеса одинаков на любом масштабе.
float lerpLog(float a, float b, float t) {
    return std::exp(lerp(std::log(a), std::log(b), t));
}

bool shiftDown() {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

} // namespace

CameraController::CameraController(float worldWidth, float worldHeight)
    : worldWidth_(worldWidth), worldHeight_(worldHeight) {
    reset();
    orbit_ = orbitGoal_ = wholeWorldPose();
    snap();
}

void CameraController::setTerrain(const Terrain3D* terrain) {
    terrain_ = terrain;
    refresh();
}

float CameraController::surfaceAt(Vector2 point) const {
    return terrain_ != nullptr ? terrain_->surfaceAt(point.x, point.y) : 0.0f;
}

float CameraController::maxDistance() const {
    return 1.6f * std::max(worldWidth_, worldHeight_);
}

std::optional<Vector2> CameraController::groundHit(const Ray& ray) const {
    if (terrain_ != nullptr) {
        return terrain_->raycast(ray);
    }
    if (ray.direction.y > -1e-4f) {
        return std::nullopt;
    }
    const float t = -ray.position.y / ray.direction.y;
    return Vector2{ray.position.x + ray.direction.x * t, ray.position.z + ray.direction.z * t};
}

float CameraController::approach(float dt, float scale) const {
    const float time = settings_.smoothing * scale;
    return time <= 1e-4f ? 1.0f : 1.0f - std::exp(-dt / time);
}

Vector3 CameraController::pannedVelocity(Vector3 velocity, Vector3 wanted, bool pressed, float dt) const {
    if (pressed) {
        const float accel = std::min(kAccelTime, settings_.smoothing);
        const float t = accel <= 1e-4f ? 1.0f : 1.0f - std::exp(-dt / accel);
        return Vector3Lerp(velocity, wanted, t);
    }
    if (settings_.inertia <= 1e-4f) {
        return {};
    }
    return Vector3Scale(velocity, std::exp(-dt / settings_.inertia));
}

CameraController::OrbitPose CameraController::wholeWorldPose() const {
    OrbitPose pose;
    pose.focus = {worldWidth_ * 0.5f, worldHeight_ * 0.5f};
    pose.yaw = 0.0f;
    pose.pitch = kDefaultPitch;
    // Весь мир по вертикали поля зрения.
    pose.distance = std::max(worldWidth_, worldHeight_) / (2.0f * std::tan(kFovY * 0.5f * DEG2RAD));
    return pose;
}

void CameraController::reset() {
    if (mode_ == ViewMode::Cinema) {
        return; // кадр ставит режиссёр
    }
    if (is3d()) {
        if (mode_ != ViewMode::Orbit) {
            enterOrbitFromCamera();
        }
        toTopDown_ = false;
        orbitGoal_ = wholeWorldPose();
        orbitGoal_.yaw += 360.0f * std::round((orbit_.yaw - orbitGoal_.yaw) / 360.0f);
        return;
    }
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    const float fit = std::min(screenWidth / worldWidth_, screenHeight / worldHeight_) * 0.9f;
    topGoalCenter_ = {worldWidth_ * 0.5f, worldHeight_ * 0.5f};
    topGoalZoom_ = std::clamp(fit, kMinZoom, kMaxZoom);
    zoomAnchored_ = false;
    panVelocity_ = {};
}

void CameraController::lookAt(float x, float y, float zoom) {
    if (mode_ == ViewMode::Fly || mode_ == ViewMode::Shoulder) {
        enterOrbitFromCamera();
    }
    panVelocity_ = {};
    if (mode_ == ViewMode::Orbit) {
        orbit_.focus = orbitGoal_.focus = {x, y};
        orbit_.distance = orbitGoal_.distance = std::clamp(distanceForZoom(zoom), kMinDistance, maxDistance());
    } else {
        topCenter_ = topGoalCenter_ = {x, y};
        topZoom_ = topGoalZoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
        zoomAnchored_ = false;
    }
    refresh();
}

void CameraController::moveTo(float x, float y) {
    if (mode_ == ViewMode::Orbit) {
        orbitGoal_.focus = {x, y};
    } else if (mode_ == ViewMode::TopDown) {
        topGoalCenter_ = {x, y};
        zoomAnchored_ = false;
    }
}

void CameraController::frame(float x, float y, float zoom) {
    if (mode_ != ViewMode::TopDown) {
        moveTo(x, y);
        return;
    }
    topGoalCenter_ = {x, y};
    topGoalZoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    zoomAnchored_ = false;
}

float CameraController::goalZoom() const {
    return mode_ == ViewMode::TopDown ? topGoalZoom_ : zoom();
}

void CameraController::snap() {
    topCenter_ = topGoalCenter_;
    topZoom_ = topGoalZoom_;
    zoomAnchored_ = false;
    orbit_ = orbitGoal_;
    if (toTopDown_) {
        toTopDown_ = false;
        mode_ = ViewMode::TopDown;
        topCenter_ = topGoalCenter_ = orbit_.focus;
        topZoom_ = topGoalZoom_ = std::clamp(zoomForDistance(orbit_.distance), kMinZoom, kMaxZoom);
    }
    flyYaw_ = flyGoalYaw_;
    flyPitch_ = flyGoalPitch_;
    if (subject_) {
        shoulderHeading_ = subject_->heading;
        shoulderGoal(shoulderEye_, shoulderTarget_);
    }
    if (mode_ == ViewMode::Cinema) {
        cinemaSnap_ = true;
        updateCinema(0.0f);
    }
    refresh();
}

CameraController::OrbitPose CameraController::poseFromCamera() const {
    const Vector3 eye = camera3d_.position;
    const Vector3 forward = Vector3Normalize(Vector3Subtract(camera3d_.target, eye));
    std::optional<Vector2> focus = groundHit({eye, forward});
    if (focus && Vector2Distance(*focus, {eye.x, eye.z}) > maxDistance()) {
        focus.reset();
    }
    if (!focus) {
        // Взгляд в небо: точка на земле впереди.
        const float ahead = std::max(50.0f, 2.0f * (eye.y - surfaceAt({eye.x, eye.z})));
        const Vector2 flat = {forward.x, forward.z};
        const float length = Vector2Length(flat);
        focus = length > 1e-3f ? Vector2Add({eye.x, eye.z}, Vector2Scale(flat, ahead / length))
                               : Vector2{eye.x, eye.z};
    }
    OrbitPose pose;
    pose.focus = {std::clamp(focus->x, 0.0f, worldWidth_), std::clamp(focus->y, 0.0f, worldHeight_)};
    const Vector3 offset = Vector3Subtract(eye, {pose.focus.x, surfaceAt(pose.focus), pose.focus.y});
    pose.distance = std::clamp(Vector3Length(offset), kMinDistance, maxDistance());
    pose.yaw = std::atan2(offset.x, offset.z) * RAD2DEG;
    pose.pitch = std::clamp(std::asin(std::clamp(offset.y / std::max(Vector3Length(offset), 1e-3f), -1.0f, 1.0f)) *
                                RAD2DEG,
                            0.5f, kTopPitch);
    return pose;
}

void CameraController::enterOrbitFromCamera() {
    orbit_ = poseFromCamera();
    orbitGoal_ = orbit_;
    // Наклон вне пределов орбиты выправляется плавно.
    orbitGoal_.pitch = std::clamp(orbitGoal_.pitch, kMinPitch, kMaxPitch);
    mode_ = ViewMode::Orbit;
    toTopDown_ = false;
}

void CameraController::setMode(ViewMode mode) {
    if (mode == mode_ && !toTopDown_) {
        return;
    }
    // В 3D из вида сверху — сначала отвесная орбита над той же точкой.
    const auto leaveTopDown = [this](float yaw, float pitch) {
        orbit_ = {topCenter_, 0.0f, kTopPitch, std::clamp(distanceForZoom(topZoom_), kMinDistance, maxDistance())};
        orbitGoal_ = {topGoalCenter_, yaw, pitch,
                      std::clamp(distanceForZoom(topGoalZoom_), kMinDistance, maxDistance())};
        mode_ = ViewMode::Orbit;
        toTopDown_ = false;
        refreshOrbit();
    };

    switch (mode) {
    case ViewMode::TopDown:
        if (mode_ != ViewMode::Orbit) {
            enterOrbitFromCamera();
        }
        if (!toTopDown_) {
            savedYaw_ = orbitGoal_.yaw;
            savedPitch_ = std::clamp(orbitGoal_.pitch, kMinPitch, kMaxPitch);
        }
        // Выпрямиться над точкой и повернуться севером вверх; вид сверху
        // включится, когда орбита дойдёт (updateOrbit).
        toTopDown_ = true;
        orbitGoal_.yaw = 360.0f * std::round(orbit_.yaw / 360.0f);
        orbitGoal_.pitch = kTopPitch;
        break;
    case ViewMode::Orbit:
        if (mode_ == ViewMode::TopDown) {
            leaveTopDown(std::remainder(savedYaw_, 360.0f), savedPitch_);
        } else if (mode_ == ViewMode::Orbit) {
            // Передумали уходить в вид сверху.
            toTopDown_ = false;
            orbitGoal_.yaw = savedYaw_ + 360.0f * std::round((orbit_.yaw - savedYaw_) / 360.0f);
            orbitGoal_.pitch = savedPitch_;
        } else {
            enterOrbitFromCamera();
        }
        break;
    case ViewMode::Fly: {
        if (mode_ == ViewMode::TopDown) {
            leaveTopDown(0.0f, kTopPitch);
        }
        const Vector3 eye = camera3d_.position;
        const Vector3 forward = Vector3Normalize(Vector3Subtract(camera3d_.target, eye));
        flyEye_ = eye;
        flyYaw_ = flyGoalYaw_ = std::atan2(-forward.x, -forward.z) * RAD2DEG;
        flyPitch_ = flyGoalPitch_ =
            std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)) * RAD2DEG, -kMaxFlyPitch, kMaxFlyPitch);
        flyAltitude_ = eye.y - surfaceAt({eye.x, eye.z});
        flyVelocity_ = {};
        mode_ = ViewMode::Fly;
        toTopDown_ = false;
        break;
    }
    case ViewMode::Shoulder: {
        if (!subject_) {
            return;
        }
        if (mode_ == ViewMode::TopDown) {
            leaveTopDown(0.0f, kTopPitch);
        }
        // Камера подлетает с того места, где была.
        const Vector3 pivot = shoulderPivot();
        shoulderEye_ = Vector3Subtract(camera3d_.position, pivot);
        shoulderTarget_ = Vector3Subtract(camera3d_.target, pivot);
        shoulderHeading_ = subject_->heading;
        shoulderYaw_ = 0.0f;
        mode_ = ViewMode::Shoulder;
        toTopDown_ = false;
        break;
    }
    case ViewMode::Cinema:
        if (mode_ == ViewMode::TopDown) {
            leaveTopDown(0.0f, kTopPitch);
        }
        enterCinemaFromCamera();
        break;
    }
    panVelocity_ = {};
    refresh();
}

void CameraController::setOrbitAngles(float yawDegrees, float pitchDegrees) {
    savedYaw_ = yawDegrees;
    savedPitch_ = std::clamp(pitchDegrees, kMinPitch, kMaxPitch);
    orbit_.yaw = orbitGoal_.yaw = savedYaw_;
    orbit_.pitch = orbitGoal_.pitch = savedPitch_;
    refresh();
}

void CameraController::setSubject(std::optional<CameraSubject> subject) {
    const bool switched = subject && subject_ && subject->id != subject_->id;
    subject_ = subject;
    if (switched && mode_ == ViewMode::Shoulder) {
        // Глаз и взгляд остаются на месте и догоняют нового.
        const Vector3 pivot = shoulderPivot();
        shoulderEye_ = Vector3Subtract(camera3d_.position, pivot);
        shoulderTarget_ = Vector3Subtract(camera3d_.target, pivot);
        shoulderHeading_ = subject_->heading;
    }
}

float CameraController::eyeHeight() const {
    return camera3d_.position.y - surfaceAt({camera3d_.position.x, camera3d_.position.z});
}

Vector2 CameraController::focus() const {
    switch (mode_) {
    case ViewMode::TopDown:
        return topCenter_;
    case ViewMode::Orbit:
        return orbit_.focus;
    case ViewMode::Fly:
        return flyFocus_;
    case ViewMode::Shoulder:
        return subject_ ? subject_->at : camera_.target;
    case ViewMode::Cinema:
        return Vector2Add(cinema_.anchor, cinema_.offset);
    }
    return topCenter_;
}

float CameraController::zoom() const {
    return mode_ == ViewMode::TopDown ? topZoom_ : camera_.zoom;
}

float CameraController::zoomAt(Vector2 point) const {
    if (mode_ == ViewMode::TopDown) {
        return camera_.zoom;
    }
    return zoomForDistance(Vector3Distance(camera3d_.position, {point.x, surfaceAt(point), point.y}));
}

std::optional<Vector2> CameraController::screenToWorld(Vector2 screen) const {
    if (mode_ == ViewMode::TopDown) {
        return GetScreenToWorld2D(screen, camera_);
    }
    // Луч из камеры до рельефа; без него — до плоскости земли.
    return groundHit(GetScreenToWorldRay(screen, camera3d_));
}

std::optional<Vector2> CameraController::worldToScreen(Vector2 point, float height) const {
    if (mode_ == ViewMode::TopDown) {
        return GetWorldToScreen2D(point, camera_);
    }
    const Vector3 at = {point.x, surfaceAt(point) + height, point.y};
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
    // кладёт её на землю, (x, y, z) → (x, −z, y), то есть на метр над землёй;
    // с рельефом высоту вершинам затем даёт его шейдер (Terrain3D::beginDrape).
    rlPushMatrix();
    rlRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    // Слой — подсказка, а не часть мира: виден сквозь дома и с обеих сторон.
    rlDisableDepthTest();
    rlDisableBackfaceCulling();
    // Высоту вершинам слоя даёт рельеф.
    if (terrain_ != nullptr) {
        terrain_->beginDrape({camera3d_.target.x, camera3d_.target.z});
    }
}

void CameraController::endOverlay() const {
    if (mode_ == ViewMode::TopDown) {
        EndMode2D();
        return;
    }
    rlPopMatrix();
    rlDrawRenderBatchActive(); // дорисовать слой, пока отсечение граней выключено
    if (terrain_ != nullptr) {
        terrain_->endDrape();
    }
    rlEnableBackfaceCulling();
    EndMode3D();
}

void CameraController::update(float dt, bool mouseFree, bool keyboardFree) {
    if (keyboardFree && mode_ != ViewMode::Cinema) {
        if (IsKeyPressed(KEY_V)) {
            setMode(is3d() && !toTopDown_ ? ViewMode::TopDown : ViewMode::Orbit);
        }
        if (IsKeyPressed(KEY_G)) {
            setMode(mode_ == ViewMode::Fly ? ViewMode::Orbit : ViewMode::Fly);
        }
        if (IsKeyPressed(KEY_T)) {
            setMode(mode_ == ViewMode::Shoulder ? ViewMode::Orbit : ViewMode::Shoulder);
        }
    }
    if (mode_ == ViewMode::Shoulder && !subject_) {
        enterOrbitFromCamera();
    }
    switch (mode_) {
    case ViewMode::TopDown:
        updateTopDown(dt, mouseFree, keyboardFree);
        break;
    case ViewMode::Orbit:
        updateOrbit(dt, mouseFree, keyboardFree);
        break;
    case ViewMode::Fly:
        updateFly(dt, mouseFree, keyboardFree);
        break;
    case ViewMode::Shoulder:
        updateShoulder(dt, mouseFree);
        break;
    case ViewMode::Cinema:
        updateCinema(dt);
        break;
    }
    refresh();
}

void CameraController::updateTopDown(float dt, bool mouseFree, bool keyboardFree) {
    bool dragging = false;
    Vector2 dragMove{};
    if (mouseFree) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Точка мира под курсором остаётся под курсором, пока зум догоняет цель.
            const Vector2 mouse = GetMousePosition();
            zoomAnchor_ = GetScreenToWorld2D(mouse, camera_);
            zoomAnchorScreen_ = mouse;
            zoomAnchored_ = true;
            topGoalZoom_ = std::clamp(topGoalZoom_ * std::exp(wheel * kWheelZoomStep), kMinZoom, kMaxZoom);
        }

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            // Земля тянется за курсором без сглаживания.
            const Vector2 delta = GetMouseDelta();
            dragging = true;
            dragMove = Vector2Scale(delta, -1.0f / topZoom_);
            if (zoomAnchored_) {
                zoomAnchorScreen_ = Vector2Add(zoomAnchorScreen_, delta);
            } else {
                topCenter_ = Vector2Add(topCenter_, dragMove);
                topGoalCenter_ = Vector2Add(topGoalCenter_, dragMove);
            }
        }
    }

    Vector3 wanted{};
    bool pressed = false;
    if (keyboardFree) {
        float dx = 0.0f;
        float dy = 0.0f;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) dx -= 1.0f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) dx += 1.0f;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) dy -= 1.0f;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) dy += 1.0f;
        pressed = dx != 0.0f || dy != 0.0f;
        // Скорость постоянна в пикселях экрана, поэтому в метрах зависит от зума.
        const float speed = kKeyPanSpeed * settings_.keyboardSpeed / topZoom_;
        wanted = {dx * speed, dy * speed, 0.0f};

        if (IsKeyPressed(KEY_HOME)) {
            reset();
        }
    }

    if (dragging) {
        // Скорость броска — сглаженная скорость перетаскивания.
        if (dt > 0.0f) {
            panVelocity_ = Vector3Lerp(panVelocity_, {dragMove.x / dt, dragMove.y / dt, 0.0f}, 0.5f);
        }
    } else {
        panVelocity_ = pannedVelocity(panVelocity_, wanted, pressed, dt);
        const Vector2 step = {panVelocity_.x * dt, panVelocity_.y * dt};
        if (zoomAnchored_) {
            zoomAnchor_ = Vector2Add(zoomAnchor_, step);
        } else {
            topCenter_ = Vector2Add(topCenter_, step);
            topGoalCenter_ = Vector2Add(topGoalCenter_, step);
        }
    }

    const float t = approach(dt);
    topZoom_ = lerpLog(topZoom_, topGoalZoom_, t);
    if (zoomAnchored_) {
        const Vector2 offset = Vector2Subtract(zoomAnchorScreen_, screenCenter());
        topCenter_ = Vector2Subtract(zoomAnchor_, Vector2Scale(offset, 1.0f / topZoom_));
        topGoalCenter_ = Vector2Subtract(zoomAnchor_, Vector2Scale(offset, 1.0f / topGoalZoom_));
        if (std::abs(std::log(topZoom_ / topGoalZoom_)) < 1e-3f) {
            topZoom_ = topGoalZoom_;
            topCenter_ = topGoalCenter_;
            zoomAnchored_ = false;
        }
    } else {
        topCenter_ = Vector2Lerp(topCenter_, topGoalCenter_, t);
    }
}

void CameraController::updateOrbit(float dt, bool mouseFree, bool keyboardFree) {
    const float sensitivity = kMouseRotate * settings_.mouseSensitivity;
    const float invert = settings_.invertY ? -1.0f : 1.0f;
    bool dragging = false;
    Vector2 dragMove{};
    if (mouseFree) {
        const Vector2 mouse = GetMousePosition();
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Ближе — к точке под курсором: она почти не сдвигается на экране.
            const float distance =
                std::clamp(orbitGoal_.distance * std::exp(-wheel * kWheelZoomStep), kMinDistance, maxDistance());
            if (const auto under = screenToWorld(mouse)) {
                const float pull = 1.0f - distance / orbitGoal_.distance;
                orbitGoal_.focus = Vector2Add(orbitGoal_.focus,
                                              Vector2Scale(Vector2Subtract(*under, orbitGoal_.focus), pull));
            }
            orbitGoal_.distance = distance;
        }

        const Vector2 delta = GetMouseDelta();
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && !toTopDown_) {
            orbitGoal_.yaw -= delta.x * sensitivity;
            orbitGoal_.pitch += delta.y * sensitivity * invert;
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            // Земля под курсором тянется за ним без сглаживания.
            dragging = true;
            const auto from = screenToWorld(Vector2Subtract(mouse, delta));
            const auto to = screenToWorld(mouse);
            if (from && to) {
                dragMove = Vector2Subtract(*from, *to);
                orbit_.focus = Vector2Add(orbit_.focus, dragMove);
                orbitGoal_.focus = Vector2Add(orbitGoal_.focus, dragMove);
            }
        }
    }

    Vector3 wanted{};
    bool pressed = false;
    if (keyboardFree) {
        float forward = 0.0f;
        float right = 0.0f;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) right -= 1.0f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) right += 1.0f;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) forward += 1.0f;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) forward -= 1.0f;
        pressed = forward != 0.0f || right != 0.0f;
        // Вперёд — от камеры к точке по земле; вправо — поперёк.
        const float yaw = orbit_.yaw * DEG2RAD;
        const float speed = kKeyPanRate * settings_.keyboardSpeed * orbit_.distance;
        wanted = {(-std::sin(yaw) * forward + std::cos(yaw) * right) * speed,
                  (-std::cos(yaw) * forward - std::sin(yaw) * right) * speed, 0.0f};

        if (!toTopDown_) {
            const float rotate = kKeyRotate * settings_.keyboardSpeed * dt;
            const float tilt = kKeyTilt * settings_.keyboardSpeed * dt;
            if (IsKeyDown(KEY_Q)) orbitGoal_.yaw += rotate;
            if (IsKeyDown(KEY_E)) orbitGoal_.yaw -= rotate;
            if (IsKeyDown(KEY_R)) orbitGoal_.pitch += tilt;
            if (IsKeyDown(KEY_F)) orbitGoal_.pitch -= tilt;
        }

        if (IsKeyPressed(KEY_HOME)) {
            reset();
        }
    }

    if (dragging) {
        if (dt > 0.0f) {
            panVelocity_ = Vector3Lerp(panVelocity_, {dragMove.x / dt, dragMove.y / dt, 0.0f}, 0.5f);
        }
    } else {
        panVelocity_ = pannedVelocity(panVelocity_, wanted, pressed, dt);
        const Vector2 step = {panVelocity_.x * dt, panVelocity_.y * dt};
        orbit_.focus = Vector2Add(orbit_.focus, step);
        orbitGoal_.focus = Vector2Add(orbitGoal_.focus, step);
    }

    // Цель — в пределах орбиты и мира; угол поворота держится около нуля
    // одинаковым сдвигом текущего и цели, чтобы не было рывка на полный оборот.
    orbitGoal_.pitch = std::clamp(orbitGoal_.pitch, kMinPitch, toTopDown_ ? kTopPitch : kMaxPitch);
    orbitGoal_.distance = std::clamp(orbitGoal_.distance, kMinDistance, maxDistance());
    orbitGoal_.focus = {std::clamp(orbitGoal_.focus.x, 0.0f, worldWidth_),
                        std::clamp(orbitGoal_.focus.y, 0.0f, worldHeight_)};
    const float turns = 360.0f * std::floor(orbit_.yaw / 360.0f);
    orbit_.yaw -= turns;
    orbitGoal_.yaw -= turns;

    const float t = approach(dt);
    orbit_.focus = Vector2Lerp(orbit_.focus, orbitGoal_.focus, t);
    orbit_.yaw = lerp(orbit_.yaw, orbitGoal_.yaw, t);
    orbit_.pitch = lerp(orbit_.pitch, orbitGoal_.pitch, t);
    orbit_.distance = lerpLog(orbit_.distance, orbitGoal_.distance, t);

    if (toTopDown_ && std::abs(orbit_.pitch - orbitGoal_.pitch) < 0.2f &&
        std::abs(orbit_.yaw - orbitGoal_.yaw) < 0.2f) {
        // Выпрямилась: дальше — вид сверху над той же точкой с тем же зумом.
        toTopDown_ = false;
        mode_ = ViewMode::TopDown;
        topCenter_ = topGoalCenter_ = orbit_.focus;
        topZoom_ = topGoalZoom_ = std::clamp(zoomForDistance(orbit_.distance), kMinZoom, kMaxZoom);
        zoomAnchored_ = false;
    }
}

void CameraController::updateFly(float dt, bool mouseFree, bool keyboardFree) {
    if (mouseFree) {
        const float sensitivity = kMouseRotate * settings_.mouseSensitivity;
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            const Vector2 delta = GetMouseDelta();
            flyGoalYaw_ -= delta.x * sensitivity;
            flyGoalPitch_ -= delta.y * sensitivity * (settings_.invertY ? -1.0f : 1.0f);
            flyGoalPitch_ = std::clamp(flyGoalPitch_, -kMaxFlyPitch, kMaxFlyPitch);
        }
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            settings_.flySpeed = std::clamp(settings_.flySpeed * std::exp(wheel * kWheelZoomStep), 0.1f, 10.0f);
        }
    }
    // Взгляд догоняет мышь вдвое быстрее обычного: полёт отзывчивее.
    const float turn = approach(dt, 0.5f);
    const float turns = 360.0f * std::floor(flyYaw_ / 360.0f);
    flyYaw_ = lerp(flyYaw_ - turns, flyGoalYaw_ - turns, turn);
    flyGoalYaw_ -= turns;
    flyPitch_ = lerp(flyPitch_, flyGoalPitch_, turn);

    const float altitude = flyEye_.y - surfaceAt({flyEye_.x, flyEye_.z});
    Vector3 wanted{};
    bool pressed = false;
    if (keyboardFree) {
        float forward = 0.0f;
        float right = 0.0f;
        float up = 0.0f;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) right -= 1.0f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) right += 1.0f;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) forward += 1.0f;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) forward -= 1.0f;
        if (IsKeyDown(KEY_E)) up += 1.0f;
        if (IsKeyDown(KEY_Q)) up -= 1.0f;
        pressed = forward != 0.0f || right != 0.0f || up != 0.0f;

        const float yaw = flyYaw_ * DEG2RAD;
        const float pitch = flyPitch_ * DEG2RAD;
        // С удержанием высоты вперёд — по горизонтали, иначе — куда смотрим.
        const Vector3 ahead = settings_.holdAltitude
                                  ? Vector3{-std::sin(yaw), 0.0f, -std::cos(yaw)}
                                  : Vector3{-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                            -std::cos(yaw) * std::cos(pitch)};
        const Vector3 side = {std::cos(yaw), 0.0f, -std::sin(yaw)};
        Vector3 direction = Vector3Add(Vector3Add(Vector3Scale(ahead, forward), Vector3Scale(side, right)),
                                       {0.0f, up, 0.0f});
        if (Vector3Length(direction) > 1e-3f) {
            direction = Vector3Normalize(direction);
        }
        // Чем выше, тем быстрее: у земли — бегом, над миром — через всю карту за секунды.
        const float speed = kFlyRate * settings_.flySpeed * std::clamp(altitude, 4.0f, 2000.0f) *
                            (shiftDown() ? kFlyBoost : 1.0f);
        wanted = Vector3Scale(direction, speed);

        if (IsKeyPressed(KEY_HOME)) {
            reset();
            return;
        }
    }
    flyVelocity_ = pannedVelocity(flyVelocity_, wanted, pressed, dt);

    const Vector3 move = Vector3Scale(flyVelocity_, dt);
    flyEye_.x = std::clamp(flyEye_.x + move.x, 0.0f, worldWidth_);
    flyEye_.z = std::clamp(flyEye_.z + move.z, 0.0f, worldHeight_);
    const float ground = surfaceAt({flyEye_.x, flyEye_.z});
    if (settings_.holdAltitude) {
        // Высота над рельефом постоянна; камера сглаженно идёт за холмами.
        flyAltitude_ = std::clamp(flyAltitude_ + move.y, kMinEyeHeight, maxDistance());
        flyEye_.y = lerp(flyEye_.y, ground + flyAltitude_, approach(dt, 2.0f));
    } else {
        flyEye_.y += move.y;
    }
    flyEye_.y = std::clamp(flyEye_.y, ground + kMinEyeHeight, maxDistance());
    if (!settings_.holdAltitude) {
        flyAltitude_ = flyEye_.y - ground;
    }
}

Vector3 CameraController::shoulderPivot() const {
    if (!subject_) {
        return camera3d_.target;
    }
    return {subject_->at.x, surfaceAt(subject_->at) + subject_->height, subject_->at.y};
}

void CameraController::shoulderGoal(Vector3& eye, Vector3& target) const {
    // Камера — позади существа по сглаженному курсу, чуть правее головы.
    const float yaw = shoulderHeading_ + PI + shoulderYaw_ * DEG2RAD;
    const float pitch = shoulderPitch_ * DEG2RAD;
    const Vector3 side = Vector3Scale({std::cos(yaw), 0.0f, -std::sin(yaw)},
                                      kShoulderSide * std::min(1.0f, shoulderDistance_ / 4.0f));
    const Vector3 back = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vector3 ahead = {-std::sin(yaw), 0.0f, -std::cos(yaw)};
    eye = Vector3Add(Vector3Scale(back, shoulderDistance_), side);
    target = Vector3Add(Vector3Scale(ahead, kShoulderAhead), side);
}

void CameraController::updateShoulder(float dt, bool mouseFree) {
    if (mouseFree) {
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            const float sensitivity = kMouseRotate * settings_.mouseSensitivity;
            const Vector2 delta = GetMouseDelta();
            shoulderYaw_ = std::remainder(shoulderYaw_ - delta.x * sensitivity, 360.0f);
            shoulderPitch_ += delta.y * sensitivity * (settings_.invertY ? -1.0f : 1.0f);
            shoulderPitch_ = std::clamp(shoulderPitch_, -10.0f, 75.0f);
        }
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            shoulderDistance_ = std::clamp(shoulderDistance_ * std::exp(-wheel * kWheelZoomStep),
                                           kShoulderMinDistance, kShoulderMaxDistance);
        }
    }
    // Курс существа догоняется медленно: шаги и повороты не дёргают камеру.
    const float turn = std::remainder(subject_->heading - shoulderHeading_, 2.0f * PI);
    shoulderHeading_ = std::remainder(shoulderHeading_ + turn * approach(dt, kShoulderTurn), 2.0f * PI);

    Vector3 eye;
    Vector3 target;
    shoulderGoal(eye, target);
    const float t = approach(dt);
    shoulderEye_ = Vector3Lerp(shoulderEye_, eye, t);
    shoulderTarget_ = Vector3Lerp(shoulderTarget_, target, t);
}

void CameraController::refresh() {
    camera3d_.up = {0.0f, 1.0f, 0.0f}; // кинокамера может наклонить кадр (голландский угол)
    switch (mode_) {
    case ViewMode::TopDown:
        refreshTopDown();
        break;
    case ViewMode::Orbit:
        refreshOrbit();
        break;
    case ViewMode::Fly:
        refreshFly();
        break;
    case ViewMode::Shoulder:
        refreshShoulder();
        break;
    case ViewMode::Cinema:
        refreshCinema();
        break;
    }
    camera3d_.fovy = fovY();
    camera3d_.projection = CAMERA_PERSPECTIVE;
}

void CameraController::refreshTopDown() {
    camera_.offset = screenCenter();
    camera_.target = topCenter_;
    camera_.rotation = 0.0f;
    camera_.zoom = topZoom_;
}

void CameraController::setClipPlanes(double nearPlane) const {
    // Дальняя плоскость — за краем мира.
    const double diagonal = std::hypot(worldWidth_, worldHeight_);
    const double height = std::max(0.0f, eyeHeight());
    // Точность буфера глубины (24 бита) падает с расстоянием как d² / near:
    // при ближней плоскости 0,1 м уже в 300 м окна в 4 см от стены, настил
    // моста над водой и берег начинают «драться» и мерцать. Поэтому ближняя
    // плоскость отодвигается настолько, насколько позволяет вид, но не дальше
    // половины высоты глаза над землёй — землю у ног она не срезает.
    nearPlane = std::clamp(nearPlane, kMinNearPlane, std::max(kMinNearPlane, 0.5 * height));
    rlSetClipPlanes(nearPlane, height + 1.5 * diagonal + Vector3Distance(camera3d_.position, camera3d_.target));
}

void CameraController::refreshOrbit() {
    orbit_.distance = std::clamp(orbit_.distance, kMinDistance, maxDistance());
    orbit_.pitch = std::clamp(orbit_.pitch, 0.5f, kTopPitch);
    orbit_.focus = {std::clamp(orbit_.focus.x, 0.0f, worldWidth_), std::clamp(orbit_.focus.y, 0.0f, worldHeight_)};

    const float yaw = orbit_.yaw * DEG2RAD;
    const float pitch = orbit_.pitch * DEG2RAD;
    const Vector3 target = {orbit_.focus.x, surfaceAt(orbit_.focus), orbit_.focus.y};
    const Vector3 offset = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    camera3d_.target = target;
    camera3d_.position = Vector3Add(target, Vector3Scale(offset, orbit_.distance));
    // Склон между камерой и точкой не пускает глаз под землю: камера поднимается
    // над ним, взгляд по-прежнему на точку.
    const float lowest = surfaceAt({camera3d_.position.x, camera3d_.position.z}) + kMinEyeHeight;
    camera3d_.position.y = std::max(camera3d_.position.y, lowest);

    // Ближняя плоскость отодвигается с расстоянием: так хватает точности
    // глубины и вблизи, и на весь мир.
    setClipPlanes(0.02 * orbit_.distance);
    camera_.offset = screenCenter();
    camera_.target = orbit_.focus;
    camera_.rotation = 0.0f;
    camera_.zoom = zoomForDistance(orbit_.distance);
}

void CameraController::refreshFly() {
    const float yaw = flyYaw_ * DEG2RAD;
    const float pitch = flyPitch_ * DEG2RAD;
    const Vector3 forward = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    const float altitude = std::max(kMinEyeHeight, flyEye_.y - surfaceAt({flyEye_.x, flyEye_.z}));
    // Точка взгляда — на земле, если она близко, иначе — не дальше нескольких
    // высот: по ней выбирается квадрат теней и зум.
    float distance = std::numeric_limits<float>::max();
    const std::optional<Vector2> hit = groundHit({flyEye_, forward});
    if (hit) {
        distance = Vector3Distance(flyEye_, {hit->x, surfaceAt(*hit), hit->y});
    }
    const float look = std::clamp(distance, 10.0f, std::max(150.0f, 3.0f * altitude));
    camera3d_.position = flyEye_;
    camera3d_.target = Vector3Add(flyEye_, Vector3Scale(forward, look));
    flyFocus_ = hit && distance <= look ? *hit : Vector2{camera3d_.target.x, camera3d_.target.z};
    flyFocus_ = {std::clamp(flyFocus_.x, 0.0f, worldWidth_), std::clamp(flyFocus_.y, 0.0f, worldHeight_)};

    setClipPlanes(std::min(0.05 * altitude, 50.0));
    camera_.offset = screenCenter();
    camera_.target = flyFocus_;
    camera_.rotation = 0.0f;
    camera_.zoom = zoomForDistance(std::min(distance, look));
}

void CameraController::refreshShoulder() {
    const Vector3 pivot = shoulderPivot();
    camera3d_.position = Vector3Add(pivot, shoulderEye_);
    camera3d_.target = Vector3Add(pivot, shoulderTarget_);
    const float lowest = surfaceAt({camera3d_.position.x, camera3d_.position.z}) + 0.4f;
    camera3d_.position.y = std::max(camera3d_.position.y, lowest);

    setClipPlanes(0.2);
    camera_.offset = screenCenter();
    camera_.target = subject_ ? subject_->at : Vector2{pivot.x, pivot.z};
    camera_.rotation = 0.0f;
    camera_.zoom = zoomForDistance(Vector3Distance(camera3d_.position, pivot));
}

float CameraController::distanceForZoom(float zoom) const {
    const float height = static_cast<float>(GetScreenHeight());
    return height / (2.0f * std::max(zoom, 1e-4f) * std::tan(fovY() * 0.5f * DEG2RAD));
}

float CameraController::zoomForDistance(float distance) const {
    const float height = static_cast<float>(GetScreenHeight());
    return height / (2.0f * std::max(distance, 1e-3f) * std::tan(fovY() * 0.5f * DEG2RAD));
}

float CameraController::fovY() const {
    return mode_ == ViewMode::Cinema ? cinema_.fovY : kFovY;
}

Vector3 CameraController::orbitDirection(float yawDegrees, float pitchDegrees) {
    const float yaw = yawDegrees * DEG2RAD;
    const float pitch = pitchDegrees * DEG2RAD;
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

void CameraController::enterCinemaFromCamera() {
    const OrbitPose pose = poseFromCamera();
    CinemaShot shot;
    shot.anchor = pose.focus;
    shot.height = 0.0f;
    shot.yaw = pose.yaw;
    shot.pitch = std::clamp(pose.pitch, kCinemaMinPitch, kCinemaMaxPitch);
    shot.distance = pose.distance;
    shot.fovY = kFovY;
    cinema_ = cinemaGoal_ = shot;
    cinemaLift_ = 0.0f;
    cinemaWantedLift_ = 0.0f;
    cinemaReach_ = pose.distance;
    cinemaSnap_ = false;
    mode_ = ViewMode::Cinema;
    toTopDown_ = false;
}

void CameraController::leaveCinema(ViewMode mode, float zoom) {
    if (mode_ != ViewMode::Cinema) {
        setMode(mode);
        return;
    }
    // Дальше — орбита с того же места: отъехать к прежнему зуму и подняться,
    // чтобы не остаться вплотную к герою у самой земли.
    enterOrbitFromCamera();
    orbitGoal_.distance = std::clamp(distanceForZoom(zoom), kMinDistance, maxDistance());
    orbitGoal_.pitch = std::clamp(std::max(orbitGoal_.pitch, 35.0f), kMinPitch, kMaxPitch);
    if (mode != ViewMode::Orbit && mode != ViewMode::Cinema) {
        setMode(mode);
    }
    refresh();
}

void CameraController::setCinemaShot(const CinemaShot& shot) {
    const bool cut = shot.cut || cinemaGoal_.cut; // склейку, которую ещё не сделали, не теряем
    if (mode_ == ViewMode::Cinema && !cut && shot.anchorId != cinemaGoal_.anchorId) {
        // Якорь сменился без склейки: точка взгляда остаётся на месте и плавно
        // идёт к новой.
        const Vector2 focusNow = Vector2Add(cinema_.anchor, cinema_.offset);
        cinema_.anchor = shot.anchor;
        cinema_.offset = Vector2Subtract(focusNow, shot.anchor);
    }
    cinemaGoal_ = shot;
    cinemaGoal_.cut = cut;
}

CameraController::Clearance CameraController::clearance(Vector3 target, Vector3 dir, float distance) const {
    Clearance result{distance, 0.0f};
    // Рельеф — шагами вдоль луча: холм между глазом и точкой.
    if (terrain_ != nullptr) {
        const int steps = std::clamp(static_cast<int>(distance / 1.5f), 6, 64);
        for (int i = 1; i <= steps; ++i) {
            const float t = distance * static_cast<float>(i) / static_cast<float>(steps);
            const Vector3 point = Vector3Add(target, Vector3Scale(dir, t));
            if (point.y < surfaceAt({point.x, point.z}) + kCinemaClearance) {
                result.clear = distance * static_cast<float>(i - 1) / static_cast<float>(steps);
                break;
            }
        }
    }
    // Здания: луч против коробок, раздутых на зазор до стены.
    const float origin[3] = {target.x, target.y, target.z};
    const float direction[3] = {dir.x, dir.y, dir.z};
    static const std::vector<BoundingBox> kNoBoxes;
    for (const BoundingBox& box : obstacles_ != nullptr ? *obstacles_ : kNoBoxes) {
        const Vector3 center = Vector3Scale(Vector3Add(box.min, box.max), 0.5f);
        const float radius = Vector3Distance(center, box.max) + kCinemaWallGap;
        if (Vector3Distance(center, target) > distance + radius) {
            continue; // луч до неё не дотянется
        }
        const float lo[3] = {box.min.x - kCinemaWallGap, box.min.y - kCinemaWallGap, box.min.z - kCinemaWallGap};
        const float hi[3] = {box.max.x + kCinemaWallGap, box.max.y + kCinemaWallGap, box.max.z + kCinemaWallGap};
        float enter = -std::numeric_limits<float>::infinity();
        float leave = std::numeric_limits<float>::infinity();
        bool miss = false;
        for (int axis = 0; axis < 3 && !miss; ++axis) {
            if (std::abs(direction[axis]) < 1e-6f) {
                miss = origin[axis] < lo[axis] || origin[axis] > hi[axis];
                continue;
            }
            float t1 = (lo[axis] - origin[axis]) / direction[axis];
            float t2 = (hi[axis] - origin[axis]) / direction[axis];
            if (t1 > t2) {
                std::swap(t1, t2);
            }
            enter = std::max(enter, t1);
            leave = std::min(leave, t2);
        }
        if (miss || leave < std::max(enter, 0.0f)) {
            continue;
        }
        if (enter <= 0.0f) {
            result.minimum = std::max(result.minimum, leave); // точка внутри: глаз — снаружи
        } else {
            result.clear = std::min(result.clear, enter);
        }
    }
    // Деревья — последними: где поставить глаз, зависит от итогового расстояния.
    foliage(target, dir, result);
    return result;
}

void CameraController::foliage(Vector3 target, Vector3 dir, Clearance& result) const {
    const float clear = result.clear;
    if (terrain_ == nullptr) {
        return;
    }
    // Профиль листвы вдоль луча от точки взгляда: сколько метров кроны набралось
    // к каждому шагу. Луч проверяется, пока он ниже верхушек; дальше листвы нет.
    constexpr int kMaxSamples = 400;
    float foliageBefore[kMaxSamples + 1]; // метров листвы до шага i (не включая его)
    const Map& map = terrain_->map();
    struct Near {
        TreeSpec tree;
        float root = 0.0f; // высота земли у корня
    };
    Near near[9 * kMaxTreesPerCell];
    int count = 0;
    Cell gathered{-1, -1};
    const int samples = std::min(kMaxSamples, static_cast<int>(clear / kFoliageStep));
    foliageBefore[0] = 0.0f;
    int measured = 0;
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i + 1) * kFoliageStep;
        const Vector3 point = Vector3Add(target, Vector3Scale(dir, t));
        bool foliage = false;
        if (point.y <= surfaceAt({point.x, point.z}) + kTreeTop) {
            const Cell cell = Map::cellAt(point.x, point.z);
            if (!(cell == gathered)) {
                // Деревья этой клетки и соседних: крона шире клетки.
                gathered = cell;
                count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const Cell around{cell.x + dx, cell.y + dy};
                        if (!map.contains(around) || map.cover.at(around.x, around.y) != Cover::Forest) {
                            continue;
                        }
                        TreeSpec trees[kMaxTreesPerCell];
                        const int n = treesInCell(map, around, trees);
                        for (int k = 0; k < n; ++k) {
                            if (trees[k].kind != TreeKind::Bush) {
                                near[count++] = {trees[k], terrain_->groundAt(trees[k].x, trees[k].z)};
                            }
                        }
                    }
                }
            }
            for (int k = 0; k < count && !foliage; ++k) {
                const TreeSpec& tree = near[k].tree;
                const float y = point.y - near[k].root;
                const float radius = crownRadiusAt(tree, y);
                if (radius > 0.0f) {
                    const float dx = point.x - (tree.x + tree.tiltX * y);
                    const float dz = point.z - (tree.z + tree.tiltZ * y);
                    foliage = dx * dx + dz * dz < radius * radius;
                }
            }
        } else if (dir.y >= 0.0f) {
            break; // дальше луч только выше верхушек
        }
        foliageBefore[i + 1] = foliageBefore[i] + (foliage ? kFoliageStep : 0.0f);
        measured = i + 1;
    }
    // Листва от точки взгляда до шага e.
    const auto upTo = [&](float t) {
        return foliageBefore[std::clamp(static_cast<int>(t / kFoliageStep), 0, measured)];
    };
    // Ветки перед объективом, если глаз на расстоянии eye.
    const auto blocked = [&](float eye) {
        return upTo(eye) - upTo(std::max(kFoliageSkip, eye - kLensClear)) > kFoliageSeeThrough;
    };
    float eye = clear;
    if (blocked(eye)) {
        // Самое дальнее положение глаза, перед которым веток нет.
        eye = 0.0f;
        for (int e = std::min(samples, static_cast<int>(clear / kFoliageStep)); e > 0; --e) {
            if (!blocked(static_cast<float>(e) * kFoliageStep)) {
                eye = static_cast<float>(e) * kFoliageStep;
                break;
            }
        }
    }
    result.clear = eye;
    result.hidden = std::max(0.0f, upTo(eye) - upTo(kFoliageSkip));
}

void CameraController::updateCinema(float dt) {
    const bool snap = cinemaSnap_ || cinemaGoal_.cut;
    if (snap) {
        cinema_ = cinemaGoal_;
        cinema_.cut = false;
        cinemaGoal_.cut = false;
    } else {
        const float t =
            cinemaGoal_.smoothing <= 1e-4f ? 1.0f : 1.0f - std::exp(-dt / cinemaGoal_.smoothing);
        cinema_.anchorId = cinemaGoal_.anchorId;
        cinema_.anchor = cinemaGoal_.anchor; // за якорем — жёстко
        cinema_.offset = Vector2Lerp(cinema_.offset, cinemaGoal_.offset, t);
        cinema_.height = lerp(cinema_.height, cinemaGoal_.height, t);
        cinema_.yaw =
            std::remainder(cinema_.yaw + std::remainder(cinemaGoal_.yaw - cinema_.yaw, 360.0f) * t, 360.0f);
        cinema_.pitch = lerp(cinema_.pitch, cinemaGoal_.pitch, t);
        cinema_.distance = lerpLog(std::max(cinema_.distance, 0.1f), std::max(cinemaGoal_.distance, 0.1f), t);
        cinema_.fovY = lerp(cinema_.fovY, cinemaGoal_.fovY, t);
        // Место на экране и наклон — своей плавностью: кадр не дёргается, когда
        // герой оглянулся, но и не отстаёт от склейки.
        const float tf = cinemaGoal_.frameSmoothing <= 1e-4f ? 1.0f
                                                             : 1.0f - std::exp(-dt / cinemaGoal_.frameSmoothing);
        cinema_.frameX = lerp(cinema_.frameX, cinemaGoal_.frameX, tf);
        cinema_.frameY = lerp(cinema_.frameY, cinemaGoal_.frameY, tf);
        cinema_.roll = lerp(cinema_.roll, cinemaGoal_.roll, tf);
        cinema_.frameSmoothing = cinemaGoal_.frameSmoothing;
        cinema_.smoothing = cinemaGoal_.smoothing;
    }
    cinema_.pitch = std::clamp(cinema_.pitch, kCinemaMinPitch, kCinemaMaxPitch);
    cinema_.fovY = std::clamp(cinema_.fovY, 8.0f, 90.0f);
    cinema_.indoor = cinemaGoal_.indoor;
    cinema_.floor = cinemaGoal_.floor;
    cinema_.ceiling = cinemaGoal_.ceiling;
    cinema_.bounds = cinemaGoal_.bounds;

    if (cinema_.indoor) {
        // В комнате: глаз — внутри коробки этажа, стены между ним и героем
        // прячет отрисовка (WorldRenderer3D), поэтому только упор в коробку.
        const Vector2 focus = Vector2Add(cinema_.anchor, cinema_.offset);
        const Vector3 target = {focus.x, cinema_.floor + cinema_.height, focus.y};
        const Vector3 dir = orbitDirection(cinema_.yaw, cinema_.pitch);
        const float origin[3] = {target.x, target.y, target.z};
        const float direction[3] = {dir.x, dir.y, dir.z};
        const float lo[3] = {cinema_.bounds.x + kCinemaRoomGap, cinema_.floor + 0.3f, cinema_.bounds.y + kCinemaRoomGap};
        const float hi[3] = {cinema_.bounds.z - kCinemaRoomGap, cinema_.ceiling - 0.15f, cinema_.bounds.w - kCinemaRoomGap};
        float reach = cinema_.distance;
        for (int axis = 0; axis < 3; ++axis) {
            if (direction[axis] > 1e-5f) {
                reach = std::min(reach, (hi[axis] - origin[axis]) / direction[axis]);
            } else if (direction[axis] < -1e-5f) {
                reach = std::min(reach, (lo[axis] - origin[axis]) / direction[axis]);
            }
        }
        cinemaWantedLift_ = 0.0f;
        cinemaLift_ = 0.0f;
        cinemaReach_ = std::max(reach, 0.4f);
        cinemaSnap_ = false;
        return;
    }

    // Препятствие между глазом и точкой: на сколько подняться, чтобы его не было.
    const Vector2 focus = Vector2Add(cinema_.anchor, cinema_.offset);
    const Vector3 target = {focus.x, surfaceAt(focus) + cinema_.height, focus.y};
    // Самый низкий ракурс, откуда герой виден целиком; иначе — откуда объектив
    // чист на полном расстоянии и листвы меньше всего (над кронами); иначе —
    // откуда свободно дальше всего.
    float wantedLift = 0.0f;
    int bestRank = -1;
    float bestScore = 0.0f;
    for (const float lift : kCinemaLifts) {
        const float pitch = std::min(cinema_.pitch + lift, kCinemaMaxPitch);
        const Clearance free = clearance(target, orbitDirection(cinema_.yaw, pitch), cinema_.distance);
        const bool full = free.clear >= cinema_.distance * 0.97f;
        if (full && free.hidden <= kFoliageSeeThrough) {
            wantedLift = lift;
            break;
        }
        const int rank = full ? 1 : 0;
        const float score = full ? -free.hidden : free.clear;
        if (rank > bestRank || (rank == bestRank && score > bestScore + 0.5f)) {
            bestRank = rank;
            bestScore = score;
            wantedLift = lift;
        }
    }
    cinemaWantedLift_ = wantedLift;
    if (snap) {
        cinemaLift_ = wantedLift;
    } else {
        const float time = wantedLift > cinemaLift_ ? kCinemaLiftUp : kCinemaLiftDown;
        cinemaLift_ = lerp(cinemaLift_, wantedLift, 1.0f - std::exp(-dt / time));
    }
    // С нынешним подъёмом: ближе препятствия — сразу, отъезд обратно — плавно.
    const float pitch = std::min(cinema_.pitch + cinemaLift_, kCinemaMaxPitch);
    const Clearance free = clearance(target, orbitDirection(cinema_.yaw, pitch), cinema_.distance);
    if (snap || free.clear < cinemaReach_) {
        cinemaReach_ = free.clear;
    } else {
        cinemaReach_ = lerp(cinemaReach_, free.clear, 1.0f - std::exp(-dt / kCinemaRelease));
    }
    cinemaReach_ = std::max(cinemaReach_, free.minimum);
    cinemaSnap_ = false;
}

bool CameraController::cinemaBlocked() const {
    if (cinema_.indoor) {
        return false;
    }
    return cinemaWantedLift_ > kCinemaBlockedLift || cinemaReach_ < cinema_.distance * kCinemaBlockedReach;
}

bool CameraController::cinemaClearFrom(float yaw) const {
    if (cinema_.indoor) {
        return true;
    }
    const Vector2 focus = Vector2Add(cinema_.anchor, cinema_.offset);
    const Vector3 target = {focus.x, surfaceAt(focus) + cinema_.height, focus.y};
    const Clearance free = clearance(target, orbitDirection(yaw, cinema_.pitch), cinema_.distance);
    return free.clear >= cinema_.distance * 0.97f && free.hidden <= kFoliageSeeThrough;
}

void CameraController::refreshCinema() {
    const Vector2 focus = Vector2Add(cinema_.anchor, cinema_.offset);
    const float base = cinema_.indoor ? cinema_.floor : surfaceAt(focus);
    const Vector3 target = {focus.x, base + cinema_.height, focus.y};
    const float pitch = std::min(cinema_.pitch + cinemaLift_, kCinemaMaxPitch);
    const float distance = cinema_.indoor ? std::max(std::min(cinema_.distance, cinemaReach_), 0.4f)
                                          : std::max(std::min(cinema_.distance, cinemaReach_), kCinemaMinDistance);
    Vector3 eye = Vector3Add(target, Vector3Scale(orbitDirection(cinema_.yaw, pitch), distance));
    if (!cinema_.indoor) {
        eye.y = std::max(eye.y, surfaceAt({eye.x, eye.z}) + kCinemaClearance);
    }
    camera3d_.position = eye;
    camera3d_.target = target;
    // Композиция (этап 12а.12): точка взгляда — не в середине экрана, а на
    // frameX, frameY (доли ширины и высоты от середины) — камера чуть
    // поворачивается от неё; наклон кадра — поворот «верха» вокруг оси взгляда.
    const Vector3 toTarget = Vector3Subtract(target, eye);
    const float reachTarget = Vector3Length(toTarget);
    if (reachTarget > 1e-3f) {
        const Vector3 forward = Vector3Scale(toTarget, 1.0f / reachTarget);
        Vector3 right = Vector3CrossProduct(forward, {0.0f, 1.0f, 0.0f});
        right = Vector3Length(right) < 1e-4f ? Vector3{1.0f, 0.0f, 0.0f} : Vector3Normalize(right);
        const Vector3 up = Vector3CrossProduct(right, forward);
        if (std::abs(cinema_.frameX) > 1e-4f || std::abs(cinema_.frameY) > 1e-4f) {
            const float tanV = std::tan(std::clamp(cinema_.fovY, 8.0f, 90.0f) * 0.5f * DEG2RAD);
            const float aspect =
                static_cast<float>(GetScreenWidth()) / std::max(1.0f, static_cast<float>(GetScreenHeight()));
            const float sx = 2.0f * cinema_.frameX * tanV * aspect;
            const float sy = 2.0f * cinema_.frameY * tanV;
            const Vector3 look = Vector3Normalize(
                Vector3Subtract(forward, Vector3Add(Vector3Scale(right, sx), Vector3Scale(up, sy))));
            camera3d_.target = Vector3Add(eye, Vector3Scale(look, reachTarget));
        }
        if (std::abs(cinema_.roll) > 1e-3f) {
            const float roll = cinema_.roll * DEG2RAD;
            camera3d_.up = Vector3Add(Vector3Scale(up, std::cos(roll)), Vector3Scale(right, std::sin(roll)));
        }
    }

    setClipPlanes(cinema_.indoor ? 0.05 : std::min(0.04 * distance, 4.0));
    camera_.offset = screenCenter();
    camera_.target = {std::clamp(focus.x, 0.0f, worldWidth_), std::clamp(focus.y, 0.0f, worldHeight_)};
    camera_.rotation = 0.0f;
    camera_.zoom = zoomForDistance(distance);
}

} // namespace lw
