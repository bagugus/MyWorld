#include "render/CameraController.h"

#include "render/Terrain3D.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

constexpr float kMinZoom = 0.02f; // Ð¿Ð¸ÐºÑÐµÐ»ÐµÐ¹ Ð½Ð° Ð¼ÐµÑ‚Ñ€
constexpr float kMaxZoom = 40.0f;
constexpr float kWheelZoomStep = 0.15f;
constexpr float kKeyPanSpeed = 700.0f; // Ð¿Ð¸ÐºÑÐµÐ»ÐµÐ¹ ÑÐºÑ€Ð°Ð½Ð° Ð² ÑÐµÐºÑƒÐ½Ð´Ñƒ
constexpr float kAccelTime = 0.1f;     // ÑÐµÐºÑƒÐ½Ð´: Ñ€Ð°Ð·Ð³Ð¾Ð½ Ð¿Ð°Ð½Ð¾Ñ€Ð°Ð¼Ñ‹ Ð¸ Ð¿Ð¾Ð»Ñ‘Ñ‚Ð° ÐºÐ»Ð°Ð²Ð¸ÑˆÐ°Ð¼Ð¸

constexpr float kFovY = 45.0f; // Ð³Ñ€Ð°Ð´ÑƒÑÑ‹, Ð¿Ð¾ Ð²ÐµÑ€Ñ‚Ð¸ÐºÐ°Ð»Ð¸
constexpr float kMinDistance = 4.0f; // Ð¼ÐµÑ‚Ñ€Ñ‹
constexpr float kMinPitch = 8.0f;    // Ð³Ñ€Ð°Ð´ÑƒÑÑ‹ Ð½Ð°Ð´ Ð³Ð¾Ñ€Ð¸Ð·Ð¾Ð½Ñ‚Ð¾Ð¼
constexpr float kMaxPitch = 88.0f;   // Ð½Ðµ 90: Ñƒ Ð¾Ñ‚Ð²ÐµÑÐ½Ð¾Ð³Ð¾ Ð²Ð·Ð³Ð»ÑÐ´Ð° Ð½ÐµÑ‚ Â«Ð²ÐµÑ€Ñ…Ð°Â»
// ÐŸÐ¾Ñ‡Ñ‚Ð¸ Ð¾Ñ‚Ð²ÐµÑÐ½Ð¾: Ñ‚Ð°Ðº 3D ÑÐ¼Ð¾Ñ‚Ñ€Ð¸Ñ‚ Ð¿Ñ€Ð¸ Ð¿ÐµÑ€ÐµÑ…Ð¾Ð´Ðµ Ð¸Ð· Ð²Ð¸Ð´Ð° ÑÐ²ÐµÑ€Ñ…Ñƒ Ð¸ Ð¾Ð±Ñ€Ð°Ñ‚Ð½Ð¾.
constexpr float kTopPitch = 89.9f;
constexpr float kDefaultPitch = 55.0f;
constexpr float kMouseRotate = 0.25f; // Ð³Ñ€Ð°Ð´ÑƒÑÐ¾Ð² Ð½Ð° Ð¿Ð¸ÐºÑÐµÐ»ÑŒ
constexpr float kKeyRotate = 90.0f;   // Ð³Ñ€Ð°Ð´ÑƒÑÐ¾Ð² Ð² ÑÐµÐºÑƒÐ½Ð´Ñƒ
constexpr float kKeyTilt = 45.0f;
constexpr float kKeyPanRate = 0.8f; // Ñ€Ð°ÑÑÑ‚Ð¾ÑÐ½Ð¸Ð¹ Ð´Ð¾ Ñ‚Ð¾Ñ‡ÐºÐ¸ Ð² ÑÐµÐºÑƒÐ½Ð´Ñƒ
constexpr float kMinEyeHeight = 1.7f; // Ð¼ÐµÑ‚Ñ€Ð¾Ð² Ð½Ð°Ð´ Ð¿Ð¾Ð²ÐµÑ€Ñ…Ð½Ð¾ÑÑ‚ÑŒÑŽ: Ð½Ð¸Ð¶Ðµ Ð³Ð»Ð°Ð· Ð½Ðµ Ð¾Ð¿ÑƒÑÐºÐ°ÐµÑ‚ÑÑ

// ÐŸÐ¾Ð»Ñ‘Ñ‚: ÑÐºÐ¾Ñ€Ð¾ÑÑ‚ÑŒ â€” ÑÑ‚Ð¾Ð»ÑŒÐºÐ¾ Ð²Ñ‹ÑÐ¾Ñ‚ Ð½Ð°Ð´ Ñ€ÐµÐ»ÑŒÐµÑ„Ð¾Ð¼ Ð² ÑÐµÐºÑƒÐ½Ð´Ñƒ (Ð¾Ñ‚ 4 Ð´Ð¾ 2000 Ð¼ Ð²Ñ‹ÑÐ¾Ñ‚Ñ‹).
constexpr float kFlyRate = 1.5f;
constexpr float kFlyBoost = 4.0f; // Shift
constexpr float kMaxFlyPitch = 89.0f;

// Ð˜Ð·-Ð·Ð° Ð¿Ð»ÐµÑ‡Ð°.
constexpr float kShoulderSide = 0.45f; // Ð¼ÐµÑ‚Ñ€Ð¾Ð² Ð²Ð¿Ñ€Ð°Ð²Ð¾ Ð¾Ñ‚ Ð³Ð¾Ð»Ð¾Ð²Ñ‹
constexpr float kShoulderAhead = 2.0f; // Ð²Ð·Ð³Ð»ÑÐ´ â€” Ð½Ð° ÑÑ‚Ð¾Ð»ÑŒÐºÐ¾ Ð¼ÐµÑ‚Ñ€Ð¾Ð² Ð²Ð¿ÐµÑ€ÐµÐ´Ð¸
constexpr float kShoulderMinDistance = 1.5f;
constexpr float kShoulderMaxDistance = 30.0f;
constexpr float kShoulderTurn = 4.0f; // ÐºÑƒÑ€Ñ ÑÑƒÑ‰ÐµÑÑ‚Ð²Ð° Ð´Ð¾Ð³Ð¾Ð½ÑÐµÑ‚ÑÑ Ð²Ñ‡ÐµÑ‚Ð²ÐµÑ€Ð¾ Ð¼ÐµÐ´Ð»ÐµÐ½Ð½ÐµÐµ Ð¾Ð±Ñ‹Ñ‡Ð½Ð¾Ð³Ð¾

Vector2 screenCenter() {
    return {static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.5f};
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Ð—ÑƒÐ¼ Ð¸ Ñ€Ð°ÑÑÑ‚Ð¾ÑÐ½Ð¸Ðµ ÑÐ³Ð»Ð°Ð¶Ð¸Ð²Ð°ÑŽÑ‚ÑÑ Ð² Ð»Ð¾Ð³Ð°Ñ€Ð¸Ñ„Ð¼Ðµ: ÑˆÐ°Ð³ ÐºÐ¾Ð»ÐµÑÐ° Ð¾Ð´Ð¸Ð½Ð°ÐºÐ¾Ð² Ð½Ð° Ð»ÑŽÐ±Ð¾Ð¼ Ð¼Ð°ÑÑˆÑ‚Ð°Ð±Ðµ.
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
    // Ð’ÐµÑÑŒ Ð¼Ð¸Ñ€ Ð¿Ð¾ Ð²ÐµÑ€Ñ‚Ð¸ÐºÐ°Ð»Ð¸ Ð¿Ð¾Ð»Ñ Ð·Ñ€ÐµÐ½Ð¸Ñ.
    pose.distance = std::max(worldWidth_, worldHeight_) / (2.0f * std::tan(kFovY * 0.5f * DEG2RAD));
    return pose;
}

void CameraController::reset() {
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
        // Ð’Ð·Ð³Ð»ÑÐ´ Ð² Ð½ÐµÐ±Ð¾: Ñ‚Ð¾Ñ‡ÐºÐ° Ð½Ð° Ð·ÐµÐ¼Ð»Ðµ Ð²Ð¿ÐµÑ€ÐµÐ´Ð¸.
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
    // ÐÐ°ÐºÐ»Ð¾Ð½ Ð²Ð½Ðµ Ð¿Ñ€ÐµÐ´ÐµÐ»Ð¾Ð² Ð¾Ñ€Ð±Ð¸Ñ‚Ñ‹ Ð²Ñ‹Ð¿Ñ€Ð°Ð²Ð»ÑÐµÑ‚ÑÑ Ð¿Ð»Ð°Ð²Ð½Ð¾.
    orbitGoal_.pitch = std::clamp(orbitGoal_.pitch, kMinPitch, kMaxPitch);
    mode_ = ViewMode::Orbit;
    toTopDown_ = false;
}

void CameraController::setMode(ViewMode mode) {
    if (mode == mode_ && !toTopDown_) {
        return;
    }
    // Ð’ 3D Ð¸Ð· Ð²Ð¸Ð´Ð° ÑÐ²ÐµÑ€Ñ…Ñƒ â€” ÑÐ½Ð°Ñ‡Ð°Ð»Ð° Ð¾Ñ‚Ð²ÐµÑÐ½Ð°Ñ Ð¾Ñ€Ð±Ð¸Ñ‚Ð° Ð½Ð°Ð´ Ñ‚Ð¾Ð¹ Ð¶Ðµ Ñ‚Ð¾Ñ‡ÐºÐ¾Ð¹.
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
        // Ð’Ñ‹Ð¿Ñ€ÑÐ¼Ð¸Ñ‚ÑŒÑÑ Ð½Ð°Ð´ Ñ‚Ð¾Ñ‡ÐºÐ¾Ð¹ Ð¸ Ð¿Ð¾Ð²ÐµÑ€Ð½ÑƒÑ‚ÑŒÑÑ ÑÐµÐ²ÐµÑ€Ð¾Ð¼ Ð²Ð²ÐµÑ€Ñ…; Ð²Ð¸Ð´ ÑÐ²ÐµÑ€Ñ…Ñƒ
        // Ð²ÐºÐ»ÑŽÑ‡Ð¸Ñ‚ÑÑ, ÐºÐ¾Ð³Ð´Ð° Ð¾Ñ€Ð±Ð¸Ñ‚Ð° Ð´Ð¾Ð¹Ð´Ñ‘Ñ‚ (updateOrbit).
        toTopDown_ = true;
        orbitGoal_.yaw = 360.0f * std::round(orbit_.yaw / 360.0f);
        orbitGoal_.pitch = kTopPitch;
        break;
    case ViewMode::Orbit:
        if (mode_ == ViewMode::TopDown) {
            leaveTopDown(std::remainder(savedYaw_, 360.0f), savedPitch_);
        } else if (mode_ == ViewMode::Orbit) {
            // ÐŸÐµÑ€ÐµÐ´ÑƒÐ¼Ð°Ð»Ð¸ ÑƒÑ…Ð¾Ð´Ð¸Ñ‚ÑŒ Ð² Ð²Ð¸Ð´ ÑÐ²ÐµÑ€Ñ…Ñƒ.
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
        // ÐšÐ°Ð¼ÐµÑ€Ð° Ð¿Ð¾Ð´Ð»ÐµÑ‚Ð°ÐµÑ‚ Ñ Ñ‚Ð¾Ð³Ð¾ Ð¼ÐµÑÑ‚Ð°, Ð³Ð´Ðµ Ð±Ñ‹Ð»Ð°.
        const Vector3 pivot = shoulderPivot();
        shoulderEye_ = Vector3Subtract(camera3d_.position, pivot);
        shoulderTarget_ = Vector3Subtract(camera3d_.target, pivot);
        shoulderHeading_ = subject_->heading;
        shoulderYaw_ = 0.0f;
        mode_ = ViewMode::Shoulder;
        toTopDown_ = false;
        break;
    }
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
        // Ð“Ð»Ð°Ð· Ð¸ Ð²Ð·Ð³Ð»ÑÐ´ Ð¾ÑÑ‚Ð°ÑŽÑ‚ÑÑ Ð½Ð° Ð¼ÐµÑÑ‚Ðµ Ð¸ Ð´Ð¾Ð³Ð¾Ð½ÑÑŽÑ‚ Ð½Ð¾Ð²Ð¾Ð³Ð¾.
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
    // Ð›ÑƒÑ‡ Ð¸Ð· ÐºÐ°Ð¼ÐµÑ€Ñ‹ Ð´Ð¾ Ñ€ÐµÐ»ÑŒÐµÑ„Ð°; Ð±ÐµÐ· Ð½ÐµÐ³Ð¾ â€” Ð´Ð¾ Ð¿Ð»Ð¾ÑÐºÐ¾ÑÑ‚Ð¸ Ð·ÐµÐ¼Ð»Ð¸.
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
    // 2D-Ñ„ÑƒÐ½ÐºÑ†Ð¸Ð¸ Ñ€Ð¸ÑÑƒÑŽÑ‚ Ð² Ð¿Ð»Ð¾ÑÐºÐ¾ÑÑ‚Ð¸ (x, y) Ñ z Ð¾ÐºÐ¾Ð»Ð¾ âˆ’1: Ð¿Ð¾Ð²Ð¾Ñ€Ð¾Ñ‚ Ð²Ð¾ÐºÑ€ÑƒÐ³ Ð¾ÑÐ¸ X
    // ÐºÐ»Ð°Ð´Ñ‘Ñ‚ ÐµÑ‘ Ð½Ð° Ð·ÐµÐ¼Ð»ÑŽ, (x, y, z) â†’ (x, âˆ’z, y), Ñ‚Ð¾ ÐµÑÑ‚ÑŒ Ð½Ð° Ð¼ÐµÑ‚Ñ€ Ð½Ð°Ð´ Ð·ÐµÐ¼Ð»Ñ‘Ð¹;
    // Ñ Ñ€ÐµÐ»ÑŒÐµÑ„Ð¾Ð¼ Ð²Ñ‹ÑÐ¾Ñ‚Ñƒ Ð²ÐµÑ€ÑˆÐ¸Ð½Ð°Ð¼ Ð·Ð°Ñ‚ÐµÐ¼ Ð´Ð°Ñ‘Ñ‚ ÐµÐ³Ð¾ ÑˆÐµÐ¹Ð´ÐµÑ€ (Terrain3D::beginDrape).
    rlPushMatrix();
    rlRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    // Ð¡Ð»Ð¾Ð¹ â€” Ð¿Ð¾Ð´ÑÐºÐ°Ð·ÐºÐ°, Ð° Ð½Ðµ Ñ‡Ð°ÑÑ‚ÑŒ Ð¼Ð¸Ñ€Ð°: Ð²Ð¸Ð´ÐµÐ½ ÑÐºÐ²Ð¾Ð·ÑŒ Ð´Ð¾Ð¼Ð° Ð¸ Ñ Ð¾Ð±ÐµÐ¸Ñ… ÑÑ‚Ð¾Ñ€Ð¾Ð½.
    rlDisableDepthTest();
    rlDisableBackfaceCulling();
    // Ð’Ñ‹ÑÐ¾Ñ‚Ñƒ Ð²ÐµÑ€ÑˆÐ¸Ð½Ð°Ð¼ ÑÐ»Ð¾Ñ Ð´Ð°Ñ‘Ñ‚ Ñ€ÐµÐ»ÑŒÐµÑ„.
    if (terrain_ != nullptr) {
        terrain_->beginDrape();
    }
}

void CameraController::endOverlay() const {
    if (mode_ == ViewMode::TopDown) {
        EndMode2D();
        return;
    }
    rlPopMatrix();
    rlDrawRenderBatchActive(); // Ð´Ð¾Ñ€Ð¸ÑÐ¾Ð²Ð°Ñ‚ÑŒ ÑÐ»Ð¾Ð¹, Ð¿Ð¾ÐºÐ° Ð¾Ñ‚ÑÐµÑ‡ÐµÐ½Ð¸Ðµ Ð³Ñ€Ð°Ð½ÐµÐ¹ Ð²Ñ‹ÐºÐ»ÑŽÑ‡ÐµÐ½Ð¾
    if (terrain_ != nullptr) {
        terrain_->endDrape();
    }
    rlEnableBackfaceCulling();
    EndMode3D();
}

void CameraController::update(float dt, bool mouseFree, bool keyboardFree) {
    if (keyboardFree) {
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
    }
    refresh();
}

void CameraController::updateTopDown(float dt, bool mouseFree, bool keyboardFree) {
    bool dragging = false;
    Vector2 dragMove{};
    if (mouseFree) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Ð¢Ð¾Ñ‡ÐºÐ° Ð¼Ð¸Ñ€Ð° Ð¿Ð¾Ð´ ÐºÑƒÑ€ÑÐ¾Ñ€Ð¾Ð¼ Ð¾ÑÑ‚Ð°Ñ‘Ñ‚ÑÑ Ð¿Ð¾Ð´ ÐºÑƒÑ€ÑÐ¾Ñ€Ð¾Ð¼, Ð¿Ð¾ÐºÐ° Ð·ÑƒÐ¼ Ð´Ð¾Ð³Ð¾Ð½ÑÐµÑ‚ Ñ†ÐµÐ»ÑŒ.
            const Vector2 mouse = GetMousePosition();
            zoomAnchor_ = GetScreenToWorld2D(mouse, camera_);
            zoomAnchorScreen_ = mouse;
            zoomAnchored_ = true;
            topGoalZoom_ = std::clamp(topGoalZoom_ * std::exp(wheel * kWheelZoomStep), kMinZoom, kMaxZoom);
        }

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            // Ð—ÐµÐ¼Ð»Ñ Ñ‚ÑÐ½ÐµÑ‚ÑÑ Ð·Ð° ÐºÑƒÑ€ÑÐ¾Ñ€Ð¾Ð¼ Ð±ÐµÐ· ÑÐ³Ð»Ð°Ð¶Ð¸Ð²Ð°Ð½Ð¸Ñ.
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
        // Ð¡ÐºÐ¾Ñ€Ð¾ÑÑ‚ÑŒ Ð¿Ð¾ÑÑ‚Ð¾ÑÐ½Ð½Ð° Ð² Ð¿Ð¸ÐºÑÐµÐ»ÑÑ… ÑÐºÑ€Ð°Ð½Ð°, Ð¿Ð¾ÑÑ‚Ð¾Ð¼Ñƒ Ð² Ð¼ÐµÑ‚Ñ€Ð°Ñ… Ð·Ð°Ð²Ð¸ÑÐ¸Ñ‚ Ð¾Ñ‚ Ð·ÑƒÐ¼Ð°.
        const float speed = kKeyPanSpeed * settings_.keyboardSpeed / topZoom_;
        wanted = {dx * speed, dy * speed, 0.0f};

        if (IsKeyPressed(KEY_HOME)) {
            reset();
        }
    }

    if (dragging) {
        // Ð¡ÐºÐ¾Ñ€Ð¾ÑÑ‚ÑŒ Ð±Ñ€Ð¾ÑÐºÐ° â€” ÑÐ³Ð»Ð°Ð¶ÐµÐ½Ð½Ð°Ñ ÑÐºÐ¾Ñ€Ð¾ÑÑ‚ÑŒ Ð¿ÐµÑ€ÐµÑ‚Ð°ÑÐºÐ¸Ð²Ð°Ð½Ð¸Ñ.
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
            // Ð‘Ð»Ð¸Ð¶Ðµ â€” Ðº Ñ‚Ð¾Ñ‡ÐºÐµ Ð¿Ð¾Ð´ ÐºÑƒÑ€ÑÐ¾Ñ€Ð¾Ð¼: Ð¾Ð½Ð° Ð¿Ð¾Ñ‡Ñ‚Ð¸ Ð½Ðµ ÑÐ´Ð²Ð¸Ð³Ð°ÐµÑ‚ÑÑ Ð½Ð° ÑÐºÑ€Ð°Ð½Ðµ.
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
            // Ð—ÐµÐ¼Ð»Ñ Ð¿Ð¾Ð´ ÐºÑƒÑ€ÑÐ¾Ñ€Ð¾Ð¼ Ñ‚ÑÐ½ÐµÑ‚ÑÑ Ð·Ð° Ð½Ð¸Ð¼ Ð±ÐµÐ· ÑÐ³Ð»Ð°Ð¶Ð¸Ð²Ð°Ð½Ð¸Ñ.
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
        // Ð’Ð¿ÐµÑ€Ñ‘Ð´ â€” Ð¾Ñ‚ ÐºÐ°Ð¼ÐµÑ€Ñ‹ Ðº Ñ‚Ð¾Ñ‡ÐºÐµ Ð¿Ð¾ Ð·ÐµÐ¼Ð»Ðµ; Ð²Ð¿Ñ€Ð°Ð²Ð¾ â€” Ð¿Ð¾Ð¿ÐµÑ€Ñ‘Ðº.
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

    // Ð¦ÐµÐ»ÑŒ â€” Ð² Ð¿Ñ€ÐµÐ´ÐµÐ»Ð°Ñ… Ð¾Ñ€Ð±Ð¸Ñ‚Ñ‹ Ð¸ Ð¼Ð¸Ñ€Ð°; ÑƒÐ³Ð¾Ð» Ð¿Ð¾Ð²Ð¾Ñ€Ð¾Ñ‚Ð° Ð´ÐµÑ€Ð¶Ð¸Ñ‚ÑÑ Ð¾ÐºÐ¾Ð»Ð¾ Ð½ÑƒÐ»Ñ
    // Ð¾Ð´Ð¸Ð½Ð°ÐºÐ¾Ð²Ñ‹Ð¼ ÑÐ´Ð²Ð¸Ð³Ð¾Ð¼ Ñ‚ÐµÐºÑƒÑ‰ÐµÐ³Ð¾ Ð¸ Ñ†ÐµÐ»Ð¸, Ñ‡Ñ‚Ð¾Ð±Ñ‹ Ð½Ðµ Ð±Ñ‹Ð»Ð¾ Ñ€Ñ‹Ð²ÐºÐ° Ð½Ð° Ð¿Ð¾Ð»Ð½Ñ‹Ð¹ Ð¾Ð±Ð¾Ñ€Ð¾Ñ‚.
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
        // Ð’Ñ‹Ð¿Ñ€ÑÐ¼Ð¸Ð»Ð°ÑÑŒ: Ð´Ð°Ð»ÑŒÑˆÐµ â€” Ð²Ð¸Ð´ ÑÐ²ÐµÑ€Ñ…Ñƒ Ð½Ð°Ð´ Ñ‚Ð¾Ð¹ Ð¶Ðµ Ñ‚Ð¾Ñ‡ÐºÐ¾Ð¹ Ñ Ñ‚ÐµÐ¼ Ð¶Ðµ Ð·ÑƒÐ¼Ð¾Ð¼.
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
    // Ð’Ð·Ð³Ð»ÑÐ´ Ð´Ð¾Ð³Ð¾Ð½ÑÐµÑ‚ Ð¼Ñ‹ÑˆÑŒ Ð²Ð´Ð²Ð¾Ðµ Ð±Ñ‹ÑÑ‚Ñ€ÐµÐµ Ð¾Ð±Ñ‹Ñ‡Ð½Ð¾Ð³Ð¾: Ð¿Ð¾Ð»Ñ‘Ñ‚ Ð¾Ñ‚Ð·Ñ‹Ð²Ñ‡Ð¸Ð²ÐµÐµ.
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
        // Ð¡ ÑƒÐ´ÐµÑ€Ð¶Ð°Ð½Ð¸ÐµÐ¼ Ð²Ñ‹ÑÐ¾Ñ‚Ñ‹ Ð²Ð¿ÐµÑ€Ñ‘Ð´ â€” Ð¿Ð¾ Ð³Ð¾Ñ€Ð¸Ð·Ð¾Ð½Ñ‚Ð°Ð»Ð¸, Ð¸Ð½Ð°Ñ‡Ðµ â€” ÐºÑƒÐ´Ð° ÑÐ¼Ð¾Ñ‚Ñ€Ð¸Ð¼.
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
        // Ð§ÐµÐ¼ Ð²Ñ‹ÑˆÐµ, Ñ‚ÐµÐ¼ Ð±Ñ‹ÑÑ‚Ñ€ÐµÐµ: Ñƒ Ð·ÐµÐ¼Ð»Ð¸ â€” Ð±ÐµÐ³Ð¾Ð¼, Ð½Ð°Ð´ Ð¼Ð¸Ñ€Ð¾Ð¼ â€” Ñ‡ÐµÑ€ÐµÐ· Ð²ÑÑŽ ÐºÐ°Ñ€Ñ‚Ñƒ Ð·Ð° ÑÐµÐºÑƒÐ½Ð´Ñ‹.
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
        // Ð’Ñ‹ÑÐ¾Ñ‚Ð° Ð½Ð°Ð´ Ñ€ÐµÐ»ÑŒÐµÑ„Ð¾Ð¼ Ð¿Ð¾ÑÑ‚Ð¾ÑÐ½Ð½Ð°; ÐºÐ°Ð¼ÐµÑ€Ð° ÑÐ³Ð»Ð°Ð¶ÐµÐ½Ð½Ð¾ Ð¸Ð´Ñ‘Ñ‚ Ð·Ð° Ñ…Ð¾Ð»Ð¼Ð°Ð¼Ð¸.
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
    // ÐšÐ°Ð¼ÐµÑ€Ð° â€” Ð¿Ð¾Ð·Ð°Ð´Ð¸ ÑÑƒÑ‰ÐµÑÑ‚Ð²Ð° Ð¿Ð¾ ÑÐ³Ð»Ð°Ð¶ÐµÐ½Ð½Ð¾Ð¼Ñƒ ÐºÑƒÑ€ÑÑƒ, Ñ‡ÑƒÑ‚ÑŒ Ð¿Ñ€Ð°Ð²ÐµÐµ Ð³Ð¾Ð»Ð¾Ð²Ñ‹.
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
    // ÐšÑƒÑ€Ñ ÑÑƒÑ‰ÐµÑÑ‚Ð²Ð° Ð´Ð¾Ð³Ð¾Ð½ÑÐµÑ‚ÑÑ Ð¼ÐµÐ´Ð»ÐµÐ½Ð½Ð¾: ÑˆÐ°Ð³Ð¸ Ð¸ Ð¿Ð¾Ð²Ð¾Ñ€Ð¾Ñ‚Ñ‹ Ð½Ðµ Ð´Ñ‘Ñ€Ð³Ð°ÑŽÑ‚ ÐºÐ°Ð¼ÐµÑ€Ñƒ.
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
    }
    camera3d_.up = {0.0f, 1.0f, 0.0f};
    camera3d_.fovy = kFovY;
    camera3d_.projection = CAMERA_PERSPECTIVE;
}

void CameraController::refreshTopDown() {
    camera_.offset = screenCenter();
    camera_.target = topCenter_;
    camera_.rotation = 0.0f;
    camera_.zoom = topZoom_;
}

void CameraController::setClipPlanes(double nearPlane) const {
    // Ð”Ð°Ð»ÑŒÐ½ÑÑ Ð¿Ð»Ð¾ÑÐºÐ¾ÑÑ‚ÑŒ â€” Ð·Ð° ÐºÑ€Ð°ÐµÐ¼ Ð¼Ð¸Ñ€Ð°.
    const double diagonal = std::hypot(worldWidth_, worldHeight_);
    const double height = std::max(0.0f, eyeHeight());
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
    // Ð¡ÐºÐ»Ð¾Ð½ Ð¼ÐµÐ¶Ð´Ñƒ ÐºÐ°Ð¼ÐµÑ€Ð¾Ð¹ Ð¸ Ñ‚Ð¾Ñ‡ÐºÐ¾Ð¹ Ð½Ðµ Ð¿ÑƒÑÐºÐ°ÐµÑ‚ Ð³Ð»Ð°Ð· Ð¿Ð¾Ð´ Ð·ÐµÐ¼Ð»ÑŽ: ÐºÐ°Ð¼ÐµÑ€Ð° Ð¿Ð¾Ð´Ð½Ð¸Ð¼Ð°ÐµÑ‚ÑÑ
    // Ð½Ð°Ð´ Ð½Ð¸Ð¼, Ð²Ð·Ð³Ð»ÑÐ´ Ð¿Ð¾-Ð¿Ñ€ÐµÐ¶Ð½ÐµÐ¼Ñƒ Ð½Ð° Ñ‚Ð¾Ñ‡ÐºÑƒ.
    const float lowest = surfaceAt({camera3d_.position.x, camera3d_.position.z}) + kMinEyeHeight;
    camera3d_.position.y = std::max(camera3d_.position.y, lowest);

    // Ð‘Ð»Ð¸Ð¶Ð½ÑÑ Ð¿Ð»Ð¾ÑÐºÐ¾ÑÑ‚ÑŒ Ð¾Ñ‚Ð¾Ð´Ð²Ð¸Ð³Ð°ÐµÑ‚ÑÑ Ñ Ñ€Ð°ÑÑÑ‚Ð¾ÑÐ½Ð¸ÐµÐ¼: Ñ‚Ð°Ðº Ñ…Ð²Ð°Ñ‚Ð°ÐµÑ‚ Ñ‚Ð¾Ñ‡Ð½Ð¾ÑÑ‚Ð¸
    // Ð³Ð»ÑƒÐ±Ð¸Ð½Ñ‹ Ð¸ Ð²Ð±Ð»Ð¸Ð·Ð¸, Ð¸ Ð½Ð° Ð²ÐµÑÑŒ Ð¼Ð¸Ñ€.
    setClipPlanes(std::max(0.1, 0.01 * orbit_.distance));
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
    // Ð¢Ð¾Ñ‡ÐºÐ° Ð²Ð·Ð³Ð»ÑÐ´Ð° â€” Ð½Ð° Ð·ÐµÐ¼Ð»Ðµ, ÐµÑÐ»Ð¸ Ð¾Ð½Ð° Ð±Ð»Ð¸Ð·ÐºÐ¾, Ð¸Ð½Ð°Ñ‡Ðµ â€” Ð½Ðµ Ð´Ð°Ð»ÑŒÑˆÐµ Ð½ÐµÑÐºÐ¾Ð»ÑŒÐºÐ¸Ñ…
    // Ð²Ñ‹ÑÐ¾Ñ‚: Ð¿Ð¾ Ð½ÐµÐ¹ Ð²Ñ‹Ð±Ð¸Ñ€Ð°ÐµÑ‚ÑÑ ÐºÐ²Ð°Ð´Ñ€Ð°Ñ‚ Ñ‚ÐµÐ½ÐµÐ¹ Ð¸ Ð·ÑƒÐ¼.
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

    setClipPlanes(std::clamp(0.01 * altitude, 0.1, 50.0));
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

    setClipPlanes(0.1);
    camera_.offset = screenCenter();
    camera_.target = subject_ ? subject_->at : Vector2{pivot.x, pivot.z};
    camera_.rotation = 0.0f;
    camera_.zoom = zoomForDistance(Vector3Distance(camera3d_.position, pivot));
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
