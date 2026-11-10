#include "ui/Camerawork.h"

#include "render/CameraController.h"

#include <raymath.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace lw::ui {

namespace {

// Зум плана, пикселей на метр: от (если не влезает всё, что хотелось бы) до
// (если в кадре один герой).
struct ZoomRange {
    float lo;
    float hi;
};
constexpr ZoomRange kRanges[] = {
    {0.12f, 1.2f}, // общий: несколько километров
    {0.7f, 3.0f},  // средний: сотни метров
    {2.5f, 6.0f},  // крупный: герой и его двор
};
static_assert(std::size(kRanges) == static_cast<std::size_t>(ShotSize::Count));
// Доля экрана, в которую укладывается план: по бокам окна интерфейса.
constexpr float kFrameWidth = 0.35f;
constexpr float kFrameHeight = 0.55f;
// Медленный наезд в кульминации: до kPushIn за kPushTime секунд, обратно — за kPushBack.
constexpr float kPushIn = 1.35f;
constexpr float kPushTime = 12.0f;
constexpr float kPushBack = 3.0f;
// Пролёт — если новый кадр дальше kFlightScreens экранов; длится от
// kFlightMin до kFlightMax секунд (дальше — дольше).
constexpr float kFlightScreens = 1.0f;
constexpr float kFlightMin = 1.0f;
constexpr float kFlightMax = 4.0f;
// В середине пролёта видны оба конца: отъезд до такой доли экрана на путь.
constexpr float kFlightOverview = 0.7f;
// Множитель зума от колеса пользователя.
constexpr float kUserScaleMin = 0.25f;
constexpr float kUserScaleMax = 4.0f;

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

const char* shotSizeTitle(ShotSize size) {
    constexpr const char* kNames[] = {"общий", "средний", "крупный"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ShotSize::Count));
    const auto index = static_cast<std::size_t>(size);
    return index < std::size(kNames) ? kNames[index] : "?";
}

void Camerawork::reset() {
    pendingCut_ = true; // влететь в первый кадр
    flying_ = false;
    pushIn_ = 1.0f;
    userScale_ = 1.0f;
    lastZoom_ = 0.0f;
}

Camerawork::Framing Camerawork::compose(const ShotPlan& plan) const {
    const ZoomRange range = kRanges[static_cast<std::size_t>(plan.size)];
    Vector2 lo = plan.subject;
    Vector2 hi = plan.subject;
    for (const Vector2 point : plan.extra) {
        lo = Vector2Min(lo, point);
        hi = Vector2Max(hi, point);
    }
    const float width = static_cast<float>(GetScreenWidth()) * kFrameWidth;
    const float height = static_cast<float>(GetScreenHeight()) * kFrameHeight;
    const float fit = std::min(width / std::max(hi.x - lo.x, 1.0f), height / std::max(hi.y - lo.y, 1.0f));
    Framing framing;
    if (fit >= range.lo) {
        framing.center = Vector2Scale(Vector2Add(lo, hi), 0.5f);
        framing.zoom = std::min(fit, range.hi);
    } else {
        // Всё не влезает в план: главное в центре, остальное — за кадром.
        framing.center = plan.subject;
        framing.zoom = range.lo;
    }
    framing.zoom *= pushIn_ * userScale_;
    return framing;
}

void Camerawork::update(CameraController& camera, const ShotPlan& plan, bool climax, float dt) {
    if (camera.is3d() || !plan.valid) {
        idle();
        return;
    }
    // Колесо пользователя сдвинуло цель зума — это его множитель.
    if (lastZoom_ > 0.0f) {
        const float goal = camera.goalZoom();
        if (std::abs(std::log(goal / lastZoom_)) > 1e-3f) {
            userScale_ = std::clamp(userScale_ * goal / lastZoom_, kUserScaleMin, kUserScaleMax);
        }
    }
    const float push = climax ? kPushIn : 1.0f;
    pushIn_ += (push - pushIn_) * std::min(1.0f, dt / (climax ? kPushTime : kPushBack));

    const Framing goal = compose(plan);
    if (pendingCut_) {
        pendingCut_ = false;
        from_ = {camera.focus(), camera.zoom()};
        const float distance = Vector2Distance(from_.center, goal.center);
        const float screens =
            distance * std::min(from_.zoom, goal.zoom) / static_cast<float>(GetScreenWidth());
        if (screens > kFlightScreens || std::abs(std::log(goal.zoom / from_.zoom)) > 1.5f) {
            flying_ = true;
            flightTime_ = 0.0f;
            flightLength_ = std::clamp(kFlightMin + 0.7f * std::log2(std::max(screens, 1.0f)),
                                       kFlightMin, kFlightMax);
        }
    }
    if (flying_) {
        // Пролёт: центр — по плавной кривой, зум — через отъезд, чтобы в
        // середине пути были видны оба конца (цель всё время пересчитывается:
        // герой идёт).
        flightTime_ += dt;
        const float s = smoothstep(flightTime_ / flightLength_);
        const Vector2 center = Vector2Lerp(from_.center, goal.center, s);
        const float lz0 = std::log(from_.zoom);
        const float lz1 = std::log(goal.zoom);
        const float distance = std::max(Vector2Distance(from_.center, goal.center), 1.0f);
        const float overview =
            std::log(static_cast<float>(GetScreenWidth()) * kFlightOverview / distance);
        const float bump = std::max(0.0f, std::min(lz0, lz1) - overview);
        const float lz = lz0 + (lz1 - lz0) * s - bump * 4.0f * s * (1.0f - s);
        camera.frame(center.x, center.y, std::exp(lz));
        if (flightTime_ >= flightLength_) {
            flying_ = false;
        }
    } else {
        camera.frame(goal.center.x, goal.center.y, goal.zoom);
    }
    lastZoom_ = camera.goalZoom();
}

} // namespace lw::ui
