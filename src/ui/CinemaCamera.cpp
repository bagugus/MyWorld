#include "ui/CinemaCamera.h"

#include "render/CameraController.h"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <cmath>

namespace lw::ui {

namespace {

// Крупный план: точка взгляда — грудь героя; камера в стольких метрах, чуть
// выше глаз; сбоку от направления взгляда на kCloseAngle градусов (спереди —
// видно лицо и куда он идёт). Отвернулся дальше kCloseTurn — камера
// переставляется.
constexpr float kCloseHeight = 1.35f;
constexpr float kCloseDistance = 6.5f;
constexpr float kClosePitch = 9.0f;
constexpr float kCloseFov = 34.0f;
constexpr float kCloseAngle = 55.0f;
constexpr float kCloseTurn = 65.0f;
constexpr float kCloseSmoothing = 0.9f;
// Средний план: все точки в кадре, не ближе и не дальше этого.
constexpr float kMediumHeight = 1.2f;
constexpr float kMediumPitch = 16.0f;
constexpr float kMediumFov = 40.0f;
constexpr float kMediumMin = 9.0f;
constexpr float kMediumMax = 120.0f;
constexpr float kMediumSmoothing = 1.0f;
// Двое — сбоку от линии между ними; камера переставляется, если ушла от этой
// стороны дальше kAxisTurn градусов.
constexpr float kAxisTurn = 25.0f;
// Подсказка «откуда снимать» (ShotPlan::viewFrom): чуть сбоку от неё.
constexpr float kViewFromAngle = 20.0f;
constexpr float kViewFromTurn = 35.0f;
// Общий план: сверху, облёт со скоростью kWideDrift градусов в секунду.
constexpr float kWidePitch = 30.0f;
constexpr float kWideFov = 45.0f;
constexpr float kWideMin = 60.0f;
constexpr float kWideMax = 1500.0f;
constexpr float kWideDrift = 1.2f;
constexpr float kWideSmoothing = 1.5f;
// Всё не влезает (точки разбросаны шире kMediumSpread / kWideSpread метров от
// середины) — передний план и даль: камера позади героя смотрит туда, куда он
// идёт; герой крупнее впереди, цель — вдали за ним.
constexpr float kMediumSpread = 45.0f;
constexpr float kWideSpread = 180.0f;
constexpr float kDepthMediumDistance = 14.0f;
constexpr float kDepthWideDistance = 45.0f;
constexpr float kDepthPitch = 12.0f;
constexpr float kDepthAngle = 18.0f; // чуть сбоку от линии
constexpr float kDepthLead = 0.25f;  // точка взгляда — на такую долю пути вперёд, не дальше…
constexpr float kDepthLeadMax = 12.0f; // …стольких метров
// Кадр по высоте — около трёх четвертей экрана (полосы широкого кадра).
constexpr float kFrameShare = 0.75f;
constexpr float kFitMargin = 1.2f;
// Склейка — если новый кадр дальше стольких метров (и не ближе доли его
// расстояния); проявка из чёрного — за kFadeIn секунд.
constexpr float kCutDistance = 30.0f;
constexpr float kCutShare = 0.75f;
constexpr float kFadeIn = 0.45f;
// Близкая смена кадра — плавный переход не быстрее kSettleSmoothing, kSettleTime секунд.
constexpr float kSettleTime = 2.0f;
constexpr float kSettleSmoothing = 1.3f;
// Наезд в кульминации: объектив уже в kPushIn раз за kPushTime секунд, обратно — за kPushBack.
constexpr float kPushIn = 1.3f;
constexpr float kPushTime = 12.0f;
constexpr float kPushBack = 3.0f;

float wrapDegrees(float angle) {
    return std::remainder(angle, 360.0f);
}

// Курс камеры, стоящей от точки в сторону direction (как у орбиты).
float yawOf(Vector2 direction) {
    return std::atan2(direction.x, direction.y) * RAD2DEG;
}

// Расстояние, с которого круг на земле радиуса radius виден целиком под
// наклоном pitch объективом fov: по ширине — весь диаметр, по высоте —
// сокращённая наклоном глубина.
float fitDistance(float radius, float fov, float pitch) {
    const float vertical = std::tan(fov * 0.5f * DEG2RAD);
    const float aspect = static_cast<float>(GetScreenWidth()) / std::max(1.0f, static_cast<float>(GetScreenHeight()));
    const float across = radius / (vertical * aspect);
    const float deep = radius * std::max(std::sin(pitch * DEG2RAD), 0.25f) / (vertical * kFrameShare);
    return std::max(across, deep) * kFitMargin;
}

} // namespace

void CinemaCamera::reset() {
    pendingCut_ = true;
    started_ = false;
    pushIn_ = 1.0f;
    blackout_ = 0.0f;
    settle_ = 0.0f;
}

void CinemaCamera::update(CameraController& camera, const ShotPlan& plan, std::optional<float> heading,
                          bool climax, float dt) {
    blackout_ = std::max(0.0f, blackout_ - dt / kFadeIn);
    settle_ = std::max(0.0f, settle_ - dt);
    if (!plan.valid) {
        return; // кадр остаётся прежним
    }
    const float push = climax ? kPushIn : 1.0f;
    pushIn_ += (push - pushIn_) * std::min(1.0f, dt / (climax ? kPushTime : kPushBack));

    // Что в кадре: середина и радиус всех точек (крупно — только герой) и
    // самая дальняя от героя.
    Vector2 center = plan.subject;
    if (plan.size != ShotSize::Close && !plan.extra.empty()) {
        Vector2 lo = plan.subject;
        Vector2 hi = plan.subject;
        for (const Vector2 point : plan.extra) {
            lo = Vector2Min(lo, point);
            hi = Vector2Max(hi, point);
        }
        center = Vector2Scale(Vector2Add(lo, hi), 0.5f);
    }
    float radius = Vector2Distance(center, plan.subject);
    Vector2 farthest = plan.subject;
    for (const Vector2 point : plan.extra) {
        radius = std::max(radius, Vector2Distance(center, point));
        if (Vector2Distance(plan.subject, point) > Vector2Distance(plan.subject, farthest)) {
            farthest = point;
        }
    }
    const float reach = Vector2Distance(plan.subject, farthest);

    const bool cutting = pendingCut_ || !started_;
    pendingCut_ = false;
    if (cutting && plan.subjectId != kInvalidId) {
        side_ = plan.subjectId % 2 == 0 ? 1.0f : -1.0f;
    }
    const std::optional<float> facing =
        heading ? std::optional<float>(*heading * RAD2DEG) : std::nullopt;
    // Повернуть камеру к желаемому курсу, если она ушла от него дальше turn.
    const auto aim = [&](float wanted, float turn) {
        if (cutting || std::abs(wrapDegrees(wanted - yaw_)) > turn) {
            yaw_ = wanted;
        }
    };

    CinemaShot shot;
    shot.anchorId = plan.subjectId;
    shot.anchor = plan.subject;
    const float spread = plan.size == ShotSize::Wide ? kWideSpread : kMediumSpread;
    if (plan.size == ShotSize::Close) {
        shot.height = kCloseHeight;
        shot.distance = kCloseDistance;
        shot.pitch = kClosePitch;
        shot.fovY = kCloseFov;
        shot.smoothing = kCloseSmoothing;
        if (facing) {
            aim(*facing + side_ * kCloseAngle, kCloseTurn);
        }
    } else if (radius > spread && reach > 1.0f && !plan.viewFrom) {
        // Передний план и даль: позади героя, взгляд — туда, куда он идёт.
        const Vector2 ahead = Vector2Scale(Vector2Subtract(farthest, plan.subject), 1.0f / reach);
        center = Vector2Add(plan.subject, Vector2Scale(ahead, std::min(reach * kDepthLead, kDepthLeadMax)));
        shot.height = kMediumHeight;
        shot.pitch = kDepthPitch;
        shot.fovY = plan.size == ShotSize::Wide ? kWideFov : kMediumFov;
        shot.distance = plan.size == ShotSize::Wide ? kDepthWideDistance : kDepthMediumDistance;
        shot.smoothing = kMediumSmoothing;
        aim(yawOf(Vector2Negate(ahead)) + side_ * kDepthAngle, kAxisTurn);
    } else if (plan.size == ShotSize::Medium) {
        shot.height = kMediumHeight;
        shot.pitch = kMediumPitch;
        shot.fovY = kMediumFov;
        shot.distance = std::clamp(fitDistance(radius, shot.fovY, shot.pitch), kMediumMin, kMediumMax);
        shot.smoothing = kMediumSmoothing;
        if (plan.viewFrom) {
            aim(*plan.viewFrom + side_ * kViewFromAngle, kViewFromTurn);
        } else if (reach > 1.0f) {
            // Двое и больше — сбоку от линии между героем и самым дальним: из
            // двух сторон та, что ближе к нынешней камере (при склейке — своя).
            const float along = yawOf(Vector2Subtract(farthest, plan.subject));
            const float a = along + 90.0f;
            const float b = along - 90.0f;
            const bool first = cutting ? side_ > 0.0f
                                       : std::abs(wrapDegrees(a - yaw_)) <= std::abs(wrapDegrees(b - yaw_));
            aim(first ? a : b, kAxisTurn);
        } else if (facing) {
            aim(*facing + side_ * kCloseAngle, kCloseTurn);
        }
    } else {
        shot.height = 0.0f;
        shot.pitch = kWidePitch;
        shot.fovY = kWideFov;
        shot.distance = std::clamp(fitDistance(radius, shot.fovY, shot.pitch), kWideMin, kWideMax);
        shot.smoothing = kWideSmoothing;
        yaw_ += kWideDrift * dt; // медленный облёт
    }
    if (plan.lookHeight > 0.0f) {
        shot.height = plan.lookHeight;
    }
    shot.offset = Vector2Subtract(center, plan.subject);
    yaw_ = wrapDegrees(yaw_);
    shot.yaw = yaw_;
    shot.fovY /= pushIn_;

    if (cutting) {
        // Далеко или впервые — склейка с проявкой из чёрного, близко — плавно.
        const float away = Vector2Distance(camera.focus(), center);
        if (!started_ || away > std::max(kCutDistance, kCutShare * shot.distance)) {
            shot.cut = true;
            blackout_ = 1.0f;
            settle_ = 0.0f;
        } else {
            settle_ = kSettleTime;
        }
    }
    if (settle_ > 0.0f) {
        shot.smoothing = std::max(shot.smoothing, kSettleSmoothing);
    }
    started_ = true;
    camera.setCinemaShot(shot);
}

} // namespace lw::ui
