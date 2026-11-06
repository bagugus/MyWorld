#include "ui/CinemaCamera.h"

#include "render/CameraController.h"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <cmath>

namespace lw::ui {

namespace {

// Крупный план: точка взгляда — у глаз героя, глаза — на верхней трети кадра
// (kEyeLine); камера в стольких метрах, чуть выше глаз; сбоку от направления
// взгляда на kCloseAngle градусов (спереди — видно лицо и куда он идёт).
constexpr float kCloseHeight = 1.5f;
constexpr float kEyeLine = 0.1f;
constexpr float kCloseDistance = 6.5f;
constexpr float kClosePitch = 7.0f;
constexpr float kCloseFov = 34.0f;
constexpr float kCloseAngle = 55.0f;
// Камера не ходит вокруг героя, который просто переминается или бродит:
// переставляется, только когда видна уже его спина.
constexpr float kCloseTurn = 115.0f;
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
constexpr float kAxisTurn = 50.0f;
// Подсказка «откуда снимать» (ShotPlan::viewFrom): чуть сбоку от неё.
constexpr float kViewFromAngle = 20.0f;
constexpr float kViewFromTurn = 35.0f;
// Общий план: сверху, неподвижно.
constexpr float kWidePitch = 30.0f;
constexpr float kWideFov = 45.0f;
constexpr float kWideMin = 60.0f;
constexpr float kWideMax = 1500.0f;
constexpr float kWideSmoothing = 1.5f;
// Кадр-пауза (ShotPlan::vista): низко над точкой взгляда, почти у горизонта,
// широким объективом, неподвижно; горизонт — на нижней трети.
constexpr float kVistaHeight = 8.0f;
constexpr float kVistaPitch = 5.0f;
constexpr float kVistaFov = 50.0f;
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
// Смена крупности (расстояние меняется больше чем в kSizeCut раз: общий план
// места → герой) — тоже склейка, но без затемнения: камера не пикирует (12а.10).
constexpr float kSizeCut = 2.5f;
constexpr float kFadeIn = 0.45f;
// Близкая смена кадра — плавный переход не быстрее kSettleSmoothing, kSettleTime секунд.
constexpr float kSettleTime = 2.0f;
constexpr float kSettleSmoothing = 1.3f;
// Наезд в кульминации: объектив уже в kPushIn раз за kPushTime секунд, обратно — за kPushBack.
constexpr float kPushIn = 1.3f;
constexpr float kPushTime = 12.0f;
constexpr float kPushBack = 3.0f;
// Горе и беспомощность — свой медленный наезд (12а.12).
constexpr float kGriefPush = 1.22f;
constexpr float kGriefPushTime = 16.0f;
// Отъезд в финале: камера в kPullOut раз дальше за kPullTime секунд (медленно,
// без рывка); после финала — сразу, со склейкой нового кадра.
constexpr float kPullOut = 1.7f;
constexpr float kPullTime = 6.0f;
// Вид заслонён (камера поднялась над препятствием или упёрлась в него)
// дольше kBlockedTime секунд — переставить камеру на ближайший чистый курс
// (шагами по kBlockedStep градусов в обе стороны): движение с причиной.
constexpr float kBlockedTime = 0.6f;
// После обхода камера kDodgeRest секунд не переставляется снова: доезжает и
// успокаивается (иначе — то же кружение).
constexpr float kDodgeRest = 3.0f;
constexpr float kBlockedStep = 30.0f;
// В комнате (12а.14): крупно — с пары-тройки метров широким объективом, средний
// план — все в комнате; курс — где хватает места, ближе к трём четвертям спереди.
constexpr float kRoomCloseDistance = 2.8f;
constexpr float kRoomClosePitch = 8.0f;
constexpr float kRoomCloseFov = 50.0f;
constexpr float kRoomMediumPitch = 14.0f;
constexpr float kRoomMediumFov = 60.0f;
constexpr float kRoomMin = 2.4f;
constexpr float kRoomMax = 6.5f;
constexpr float kRoomAngle = 40.0f;
constexpr float kRoomSmoothing = 0.6f;
constexpr float kRoomLookHeight = 1.3f;

// --- Композиция (12а.12) -----------------------------------------------------
// Новый смысл сцены принимается, когда продержался столько секунд (угроза —
// быстрее), и не раньше, чем прежний кадр простоял kMinSenseAge.
constexpr float kSenseHold = 1.2f;
constexpr float kUrgentHold = 0.3f;
constexpr float kMinSenseAge = 2.5f;
// Идёт или стоит — тоже с гистерезисом: шаг на месте не меняет приём.
constexpr float kMovingHold = 1.5f;
// Место на экране меняется плавно (герой оглянулся).
constexpr float kFrameSmoothing = 1.2f;
// Голландский угол в панике, градусы.
constexpr float kDutch = 9.0f;
constexpr float kDutchSoft = 5.0f;
// Разговор — «восьмёрка»: двойной план kTwoShotTime секунд, дальше через плечо
// то одного, то другого по kReverseTime секунд. Двое не дальше kTalkReach.
constexpr float kTwoShotTime = 4.0f;
constexpr float kReverseTime = 5.5f;
constexpr float kTalkReach = 8.0f;
constexpr float kTwoShotHeight = 1.45f;
constexpr float kTwoShotPitch = 5.0f;
constexpr float kTwoShotFov = 36.0f;
// Через плечо: глаз за спиной переднего (не ближе kShoulderBack, дальше — по
// объективу), сбоку, на высоте kShoulderEye; смотрит на лицо (kFaceHeight).
constexpr float kShoulderBack = 1.15f;    // не ближе
constexpr float kShoulderBackMax = 6.0f;  // не дальше
constexpr float kShoulderWidth = 0.5f;    // ширина плеч, м
constexpr float kShoulderShare = 0.28f;   // доля ширины кадра на плечо
constexpr float kShoulderSideShare = 0.42f; // сбоку — такая доля отступа
constexpr float kShoulderEye = 1.62f;
constexpr float kFaceHeight = 1.55f;
constexpr float kShoulderFov = 40.0f;
constexpr float kShoulderSmoothing = 0.5f;
// Проезд в погоне: сбоку от бегущего, вровень, широким объективом.
constexpr float kTrackDistance = 8.0f;
constexpr float kTrackHeight = 1.2f;
constexpr float kTrackPitch = 4.0f;
constexpr float kTrackFov = 42.0f;
constexpr float kTrackReach = 25.0f; // преследователь ближе — оба в кадре
constexpr float kTrackSmoothing = 0.6f;
// Драка: двойной план чуть снизу; дальше kFightNear — через плечо стрелка.
constexpr float kFightNear = 14.0f;
constexpr float kFightHeight = 1.1f;
constexpr float kFightPitch = 2.0f;
constexpr float kFightFov = 40.0f;
// Засада: через плечо того, кто подкрадывается, низко.
constexpr float kAmbushEye = 1.25f;
// Через плечо заслонили (лес, дом) дольше kShoulderBlocked секунд — двойной
// план сбоку от линии.
constexpr float kShoulderBlocked = 0.5f;
// Угроза: глаз низко, угроза нависает.
constexpr float kThreatReach = 35.0f;
constexpr float kThreatEye = 0.9f;
constexpr float kMenaceEye = 0.7f;
constexpr float kMenacePitch = -11.0f;
constexpr float kMenaceDistance = 4.6f;
constexpr float kMenaceFov = 38.0f;
constexpr float kMenaceAngle = 35.0f;
// Верхний ракурс: горе и беспомощность.
constexpr float kGriefLook = 1.0f;
constexpr float kGriefPitch = 38.0f;
constexpr float kGriefDistance = 5.2f;
constexpr float kGriefFov = 32.0f;
constexpr float kHelplessLook = 0.9f;
constexpr float kHelplessPitch = 48.0f;
constexpr float kHelplessDistance = 6.0f;
constexpr float kHelplessFov = 34.0f;
constexpr float kHighAngle = 40.0f;
// Панорама: глаз стоит впереди-сбоку пути героя, поворачивается за ним; герой
// прошёл и отошёл — новое место встык.
constexpr float kPanAhead = 9.0f;
constexpr float kPanSide = 4.5f;
constexpr float kPanEye = 1.55f;
constexpr float kPanLook = 1.45f;
constexpr float kPanFov = 34.0f;
constexpr float kPanPassed = 12.0f;
constexpr float kPanFar = 22.0f;
constexpr float kPanSmoothing = 0.08f;
// Правило третей: третья линия — на шестую часть ширины от середины.
constexpr float kThird = 1.0f / 6.0f;
// В комнате: через плечо — ближе; кадр «через проём» — глаз за дверью.
constexpr float kRoomShoulderBack = 0.8f;
constexpr float kRoomShoulderSide = 0.35f;
constexpr float kRoomShoulderEye = 1.35f;
constexpr float kRoomFaceSeated = 1.1f;
constexpr float kDoorBack = 1.3f;
constexpr float kDoorEye = 1.5f;
constexpr float kDoorFov = 40.0f;
constexpr float kDoorMin = 0.8f; // герой не у самой двери

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

// Сколько можно пройти от точки from по направлению dir, не выходя из
// прямоугольника box (x0, z0, x1, z1); снаружи — 0.
float exitDistance(Vector2 from, Vector2 dir, Vector4 box) {
    if (from.x < box.x || from.x > box.z || from.y < box.y || from.y > box.w) {
        return 0.0f;
    }
    float t = 1e9f;
    if (dir.x > 1e-5f) {
        t = std::min(t, (box.z - from.x) / dir.x);
    } else if (dir.x < -1e-5f) {
        t = std::min(t, (box.x - from.x) / dir.x);
    }
    if (dir.y > 1e-5f) {
        t = std::min(t, (box.w - from.y) / dir.y);
    } else if (dir.y < -1e-5f) {
        t = std::min(t, (box.y - from.y) / dir.y);
    }
    return t;
}

// Перпендикуляр к направлению v: со стороны side (+1 / −1) — та же сторона,
// что у курса «вдоль линии + 90° × side».
Vector2 perpendicular(Vector2 v, float side) {
    return {v.y * side, -v.x * side};
}

// Где по ширине экрана окажется смещение v, если камера стоит с курса yaw:
// +1 — вправо.
float screenX(Vector2 v, float yaw) {
    return v.x * std::cos(yaw * DEG2RAD) - v.y * std::sin(yaw * DEG2RAD);
}

// Угол объектива, при котором предмет высотой size на расстоянии distance
// занимает треть кадра, в пределах lo..hi.
float lensFor(float size, float distance, float lo, float hi) {
    return std::clamp(2.0f * std::atan(size / std::max(distance, 0.5f)) * RAD2DEG, lo, hi);
}

bool urgentSense(SceneSense sense) {
    return sense == SceneSense::Chase || sense == SceneSense::Fight || sense == SceneSense::Ambush ||
           sense == SceneSense::Threat;
}

} // namespace

void CinemaCamera::reset() {
    pendingCut_ = true;
    started_ = false;
    indoor_ = false;
    stage_ = 0;
    pushIn_ = 1.0f;
    pullOut_ = 1.0f;
    blocked_ = 0.0f;
    dodgeRest_ = 0.0f;
    dodged_ = false;
    blackout_ = 0.0f;
    settle_ = 0.0f;
    memory_.clear();
    sense_ = SceneSense::Daily;
    other_ = kInvalidId;
    pendingSense_ = SceneSense::Daily;
    pendingOther_ = kInvalidId;
    pendingTime_ = 0.0f;
    senseAge_ = 0.0f;
    moving_ = false;
    movingTime_ = 0.0f;
    framing_ = Framing::Standard;
    frameX_ = 0.0f;
    shotClock_ = 0.0f;
    reverse_ = 0;
    doorDistance_ = 0.0f;
}

const char* CinemaCamera::framingTitle() const {
    switch (framing_) {
    case Framing::Standard:
        return "обычный";
    case Framing::TwoShot:
        return "двойной";
    case Framing::OverShoulder:
        return "через плечо";
    case Framing::Reverse:
        return "через плечо (обратный)";
    case Framing::Tracking:
        return "проезд";
    case Framing::Pan:
        return "панорама";
    case Framing::Low:
        return "нижний ракурс";
    case Framing::High:
        return "верхний ракурс";
    case Framing::Doorway:
        return "через проём";
    case Framing::Indoor:
        return "в комнате";
    }
    return "?";
}

void CinemaCamera::aimFrom(CinemaShot& shot, Vector2 anchor, Vector2 eye, float eyeY, Vector2 look, float lookY) {
    const Vector2 away = Vector2Subtract(eye, look);
    const float flat = std::max(Vector2Length(away), 0.3f);
    shot.offset = Vector2Subtract(look, anchor);
    shot.yaw = yawOf(away);
    shot.pitch = std::atan2(eyeY - lookY, flat) * RAD2DEG;
    shot.distance = std::hypot(flat, eyeY - lookY);
}

bool CinemaCamera::adoptSense(const ShotPlan& plan, float dt, bool cutting) {
    senseAge_ += dt;
    const EntityId other = plan.otherId;
    if (plan.sense == sense_ && other == other_) {
        pendingTime_ = 0.0f;
        return false;
    }
    if (plan.sense != pendingSense_ || other != pendingOther_) {
        pendingSense_ = plan.sense;
        pendingOther_ = other;
        pendingTime_ = 0.0f;
    }
    pendingTime_ += dt;
    // Угроза ворвалась в спокойную сцену — сразу; остальное — когда устоялось.
    const bool urgent = urgentSense(plan.sense) && !urgentSense(sense_);
    const bool ready = urgent ? pendingTime_ >= kUrgentHold : pendingTime_ >= kSenseHold && senseAge_ >= kMinSenseAge;
    if (!cutting && !ready) {
        return false;
    }
    sense_ = plan.sense;
    other_ = other;
    senseAge_ = 0.0f;
    pendingTime_ = 0.0f;
    return !cutting;
}

void CinemaCamera::update(CameraController& camera, const ShotPlan& plan, std::optional<float> heading,
                          bool climax, bool finale, float dt) {
    blackout_ = std::max(0.0f, blackout_ - dt / kFadeIn);
    settle_ = std::max(0.0f, settle_ - dt);
    if (!plan.valid) {
        return; // кадр остаётся прежним
    }
    if (finale) {
        pullOut_ += (kPullOut - pullOut_) * std::min(1.0f, dt / kPullTime);
    } else if (pendingCut_) {
        pullOut_ = 1.0f; // новый кадр — с обычного расстояния
    } else {
        pullOut_ += (1.0f - pullOut_) * std::min(1.0f, dt / kPushBack);
    }

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

    // Вошли в дом или вышли, пересел герой в комнате — склейка.
    const bool cutting = pendingCut_ || !started_ || plan.indoor != indoor_ || (plan.indoor && plan.stage != stage_);
    pendingCut_ = false;
    // Смысл сцены сменился и устоялся — кадр строится заново (встык).
    bool recompose = adoptSense(plan, dt, cutting);
    if (plan.moving != moving_) {
        movingTime_ += dt;
        if (cutting || movingTime_ >= kMovingHold) {
            moving_ = plan.moving;
            movingTime_ = 0.0f;
            recompose = recompose || (!cutting && sense_ == SceneSense::Daily && !plan.indoor);
        }
    } else {
        movingTime_ = 0.0f;
    }
    const bool fresh = cutting || recompose;
    if (fresh) {
        dodged_ = false;
        shotClock_ = 0.0f;
        reverse_ = 0;
        doorDistance_ = 0.0f;
        frameX_ = 0.0f;
        shoulderBlocked_ = false;
        shoulderTime_ = 0.0f;
    }
    shotClock_ += dt;
    if (cutting && plan.subjectId != kInvalidId) {
        side_ = plan.subjectId % 2 == 0 ? 1.0f : -1.0f;
    }
    // Куда идёт герой (радианы, как у моделей): по экрану — туда же, что и в
    // прошлых кадрах (правило 180°): камера с той стороны пути, откуда так видно.
    const std::optional<float> moveHeading =
        moving_ && Vector2Length(plan.move) > 0.5f ? std::optional<float>(std::atan2(plan.move.x, plan.move.y))
                                                   : std::nullopt;
    if (fresh && moveHeading) {
        const float motion = memory_.motionOf(plan.subjectId);
        if (motion != 0.0f) {
            side_ = -motion; // камера с курса «путь + 90° × side» видит его идущим на −side
        }
    }
    // Куда обращён: от аниматора, иначе по ходу, иначе по месту в комнате.
    std::optional<float> facing = heading ? std::optional<float>(*heading * RAD2DEG) : std::nullopt;
    if (!facing && moveHeading) {
        facing = *moveHeading * RAD2DEG;
    }
    if (!facing && plan.facing) {
        facing = *plan.facing * RAD2DEG;
    }
    // Повернуть камеру к желаемому курсу, если она ушла от него дальше turn.
    const auto aim = [&](float wanted, float turn) {
        if (fresh || (!dodged_ && std::abs(wrapDegrees(wanted - yaw_)) > turn)) {
            yaw_ = wanted;
        }
    };
    // Правило третей: герой — на третьей линии, свободное место — перед взглядом.
    const auto thirds = [&](float yaw) {
        if (facing) {
            frameX_ = thirdsOffset(screenDirection(*facing * DEG2RAD, yaw), frameX_);
        }
        return frameX_;
    };

    // С кем сцена: только принятый собеседник, и только если план его знает.
    const bool hasOther = other_ != kInvalidId && plan.otherId == other_;
    const Vector2 other = plan.other;
    const float pair = hasOther ? Vector2Distance(plan.subject, other) : 0.0f;
    const Vector2 along = pair > 0.05f ? Vector2Scale(Vector2Subtract(other, plan.subject), 1.0f / pair)
                                       : Vector2{0.0f, 1.0f};
    // Сторона линии действия для пары — одна и та же от склейки к склейке.
    const float pairSide = hasOther ? memory_.sideOf(plan.subjectId, other_, side_) : side_;

    // Ракурс и приём выбраны по смыслу; ниже — как всегда: наезд, обход,
    // склейка.
    float push = 1.0f;
    float pushTime = kPushBack;
    if (climax) {
        push = kPushIn;
        pushTime = kPushTime;
    } else if (sense_ == SceneSense::Grief || sense_ == SceneSense::Helpless) {
        push = kGriefPush;
        pushTime = kGriefPushTime;
    }
    pushIn_ += (push - pushIn_) * std::min(1.0f, dt / pushTime);

    CinemaShot shot;
    shot.anchorId = plan.subjectId;
    shot.anchor = plan.subject;
    shot.frameSmoothing = kFrameSmoothing;
    bool cutHere = false; // смена приёма или места в том же кадре — встык
    Framing framing = Framing::Standard;
    float roll = 0.0f;

    if (plan.indoor) {
        // В комнате: крупно — герой, средний — все, кто с ним в комнате.
        const bool close = plan.size == ShotSize::Close;
        const float fov = close ? kRoomCloseFov : kRoomMediumFov;
        const float pitch = close ? kRoomClosePitch : kRoomMediumPitch;
        const float distance =
            close ? kRoomCloseDistance : std::clamp(fitDistance(radius, fov, pitch), kRoomMin, kRoomMax);
        const float lookHeight = plan.lookHeight > 0.0f ? plan.lookHeight : kRoomLookHeight;
        shot.height = lookHeight;
        shot.pitch = pitch;
        shot.fovY = fov;
        shot.distance = distance;
        shot.smoothing = kRoomSmoothing;
        shot.offset = Vector2Subtract(center, plan.subject);
        framing = Framing::Indoor;

        // Кадр «через проём»: камера в соседней комнате, смотрит в дверь на
        // героя — подглядывающий взгляд для горя, семьи, тихого быта.
        const bool quiet = sense_ == SceneSense::Grief || sense_ == SceneSense::Family ||
                           sense_ == SceneSense::Daily || sense_ == SceneSense::Helpless;
        if (fresh && quiet && !plan.doorways.empty()) {
            const std::uint64_t roll100 = ((plan.stage ^ plan.subjectId) * 2654435761ull >> 9) % 100;
            const std::uint64_t chance = sense_ == SceneSense::Grief      ? 70
                                         : sense_ == SceneSense::Helpless ? 50
                                         : sense_ == SceneSense::Family   ? 40
                                                                          : 30;
            float bestScore = -1e9f;
            for (const Vector2 door : plan.doorways) {
                const float toDoor = Vector2Distance(plan.subject, door);
                if (toDoor < kDoorMin) {
                    continue;
                }
                const Vector2 dir = Vector2Scale(Vector2Subtract(door, plan.subject), 1.0f / toDoor);
                if (exitDistance(plan.subject, dir, plan.bounds) < toDoor + kDoorBack + 0.25f) {
                    continue; // за дверью нет места
                }
                // Лучше — откуда видно лицо.
                const float score = facing ? std::cos((yawOf(dir) - *facing) * DEG2RAD) : 0.0f;
                if (score > bestScore) {
                    bestScore = score;
                    yaw_ = wrapDegrees(yawOf(dir));
                    doorDistance_ = toDoor + kDoorBack;
                }
            }
            if (roll100 >= chance) {
                doorDistance_ = 0.0f;
            }
        }
        if (doorDistance_ > 0.0f) {
            framing = Framing::Doorway;
            shot.offset = {};
            shot.fovY = kDoorFov;
            shot.pitch = std::atan2(kDoorEye - lookHeight, doorDistance_) * RAD2DEG;
            shot.distance = std::hypot(doorDistance_, kDoorEye - lookHeight);
            shot.frameX = 0.0f;
        } else if (hasOther && pair < 3.5f &&
                   (sense_ == SceneSense::Talk || sense_ == SceneSense::Family)) {
            // Разговор за столом — через плечо, то одного, то другого.
            if (shotClock_ >= (reverse_ == 0 ? kTwoShotTime : kReverseTime)) {
                reverse_ = reverse_ == 1 ? 2 : 1;
                shotClock_ = 0.0f;
                cutHere = true;
            }
            if (reverse_ != 0) {
                const bool onHero = reverse_ == 1;
                const Vector2 front = onHero ? other : plan.subject;  // чьё плечо
                const Vector2 look = onHero ? plan.subject : other;   // на кого
                const Vector2 back = onHero ? along : Vector2Negate(along);
                const Vector2 eye = Vector2Add(Vector2Add(front, Vector2Scale(back, kRoomShoulderBack)),
                                               Vector2Scale(perpendicular(along, pairSide), kRoomShoulderSide));
                const Vector2 dir = Vector2Normalize(Vector2Subtract(eye, look));
                if (exitDistance(look, dir, plan.bounds) >= Vector2Distance(eye, look) + 0.15f) {
                    CinemaShot over = shot;
                    aimFrom(over, plan.subject, eye, plan.floor + kRoomShoulderEye, look,
                            plan.floor + kRoomFaceSeated);
                    over.height = kRoomFaceSeated;
                    over.fovY = kShoulderFov + 8.0f;
                    const float onScreen = screenX(perpendicular(along, pairSide), over.yaw);
                    over.frameX = onScreen >= 0.0f ? kThird : -kThird;
                    shot = over;
                    yaw_ = wrapDegrees(shot.yaw);
                    framing = onHero ? Framing::OverShoulder : Framing::Reverse;
                }
            }
        }
        if (framing == Framing::Indoor) {
            if (fresh) {
                // Курс: спереди-сбоку от героя (двое и больше — сбоку от линии
                // между ними), но туда, где в комнате хватает места.
                float preferred = yaw_;
                if (!close && reach > 1.0f) {
                    preferred = yawOf(Vector2Subtract(farthest, plan.subject)) + 90.0f * pairSide;
                } else if (plan.facing) {
                    preferred = *plan.facing * RAD2DEG + side_ * kRoomAngle;
                }
                const float needed = distance * std::cos(pitch * DEG2RAD);
                float bestScore = -1e9f;
                float best = preferred;
                for (int k = 0; k < 24; ++k) {
                    const float offset = static_cast<float>((k + 1) / 2) * 15.0f * (k % 2 == 0 ? 1.0f : -1.0f);
                    const float yaw = preferred + offset;
                    const Vector2 dir = {std::sin(yaw * DEG2RAD), std::cos(yaw * DEG2RAD)};
                    const float inRoom = exitDistance(center, dir, plan.room);
                    const float inStorey = exitDistance(center, dir, plan.bounds);
                    const float score = -std::abs(offset) / 90.0f + 2.0f * std::min(inRoom / needed, 1.0f) +
                                        0.8f * std::min(inStorey / needed, 1.0f);
                    if (score > bestScore + 1e-3f) {
                        bestScore = score;
                        best = yaw;
                    }
                }
                yaw_ = wrapDegrees(best);
            }
            if (close) {
                shot.frameX = thirds(yaw_);
            }
        }
        if (framing != Framing::OverShoulder && framing != Framing::Reverse) {
            shot.yaw = yaw_;
        }
        shot.fovY /= pushIn_;
        shot.indoor = true;
        shot.floor = plan.floor;
        shot.ceiling = plan.ceiling;
        shot.bounds = plan.bounds;
        if (fresh || cutHere) {
            shot.cut = true; // встык: снаружи — в комнату, к другому месту, другой приём
            settle_ = 0.0f;
            blackout_ = started_ ? blackout_ : 1.0f;
        }
        indoor_ = true;
        stage_ = plan.stage;
        started_ = true;
        dodged_ = false;
        blocked_ = 0.0f;
        framing_ = framing;
        camera.setCinemaShot(shot);
        return;
    }
    indoor_ = false;
    const float spread = plan.size == ShotSize::Wide ? kWideSpread : kMediumSpread;
    const float ground = camera.heightAt(plan.subject);
    const float otherGround = hasOther ? camera.heightAt(other) : ground;
    // Через плечо: глаз за спиной front (на продолжении линии back), сбоку,
    // смотрит на look; на третью линию — смотрящий, плечо — у края.
    // Отступ за плечо — по объективу: плечо занимает у края кадра примерно
    // одну и ту же долю ширины (длинный объектив — глаз дальше).
    const auto overShoulder = [&](Vector2 front, Vector2 back, float frontGround, float eyeHeight, Vector2 look,
                                  float lookGround, float lookHeight, float fov) {
        const Vector2 sideways = perpendicular(along, pairSide);
        const float aspect =
            static_cast<float>(GetScreenWidth()) / std::max(1.0f, static_cast<float>(GetScreenHeight()));
        const float halfWidth = std::tan(fov * 0.5f * DEG2RAD) * aspect;
        const float backDistance =
            std::clamp(kShoulderWidth / (kShoulderShare * 2.0f * halfWidth), kShoulderBack, kShoulderBackMax);
        const Vector2 eye = Vector2Add(Vector2Add(front, Vector2Scale(back, backDistance)),
                                       Vector2Scale(sideways, backDistance * kShoulderSideShare));
        shot.fovY = fov;
        aimFrom(shot, plan.subject, eye, frontGround + eyeHeight, look, lookGround + lookHeight);
        shot.height = lookHeight;
        // Плечо переднего — с той стороны, куда отступил глаз, наоборот: смотрящий
        // — на третьей линии со стороны глаза.
        shot.frameX = screenX(sideways, shot.yaw) >= 0.0f ? kThird : -kThird;
        yaw_ = wrapDegrees(shot.yaw);
    };
    const bool grammar = plan.vista <= 0.0f && plan.size != ShotSize::Wide;
    // Через плечо не выходит (заслонено) — до новой склейки только двойной план.
    const bool shoulders = !shoulderBlocked_;
    bool composed = false;
    if (grammar) {
        switch (sense_) {
        case SceneSense::Talk:
        case SceneSense::Family:
            if (!hasOther || pair > kTalkReach) {
                break;
            }
            // «Восьмёрка»: двойной план, потом через плечо то одного, то другого;
            // глаз всегда по одну сторону линии между ними (180°).
            if (shotClock_ >= (reverse_ == 0 ? kTwoShotTime : kReverseTime)) {
                reverse_ = reverse_ == 1 ? 2 : 1;
                shotClock_ = 0.0f;
                cutHere = true;
            }
            if (reverse_ == 0 || !shoulders) {
                framing = Framing::TwoShot;
                center = Vector2Scale(Vector2Add(plan.subject, other), 0.5f);
                shot.height = kTwoShotHeight;
                shot.pitch = kTwoShotPitch;
                shot.fovY = kTwoShotFov;
                shot.distance = std::clamp(fitDistance(pair * 0.5f + 0.9f, kTwoShotFov, kTwoShotPitch), 3.5f, 12.0f);
                shot.smoothing = kMediumSmoothing;
                yaw_ = wrapDegrees(yawOf(along) + 90.0f * pairSide);
                shot.offset = Vector2Subtract(center, plan.subject);
                shot.yaw = yaw_;
            } else if (reverse_ == 1) {
                framing = Framing::OverShoulder;
                overShoulder(other, along, otherGround, kShoulderEye, plan.subject, ground, kFaceHeight, kShoulderFov);
                shot.smoothing = kShoulderSmoothing;
            } else {
                framing = Framing::Reverse;
                overShoulder(plan.subject, Vector2Negate(along), ground, kShoulderEye, other, otherGround, kFaceHeight,
                             kShoulderFov);
                shot.smoothing = kShoulderSmoothing;
            }
            composed = true;
            break;
        case SceneSense::Chase: {
            // Проезд: камера идёт вровень с бегущим, сбоку от пути.
            const Vector2 run = moveHeading ? plan.move
                                : hasOther && pair > 0.5f ? Vector2Negate(along)
                                                          : Vector2{std::sin(yaw_ * DEG2RAD), std::cos(yaw_ * DEG2RAD)};
            framing = Framing::Tracking;
            aim(yawOf(run) + 90.0f * side_, kAxisTurn);
            shot.height = kTrackHeight;
            shot.pitch = kTrackPitch;
            shot.fovY = kTrackFov;
            shot.smoothing = kTrackSmoothing;
            if (hasOther && pair <= kTrackReach) {
                center = Vector2Scale(Vector2Add(plan.subject, other), 0.5f);
                shot.distance = std::clamp(fitDistance(pair * 0.5f + 2.0f, kTrackFov, kTrackPitch), 7.0f, 30.0f);
                shot.frameX = 0.0f;
            } else {
                center = plan.subject;
                shot.distance = kTrackDistance;
                // Свободное место — перед бегущим.
                frameX_ = thirdsOffset(screenDirection(std::atan2(run.x, run.y), yaw_), frameX_);
                shot.frameX = frameX_;
            }
            shot.offset = Vector2Subtract(center, plan.subject);
            shot.yaw = yaw_;
            roll = plan.panic ? kDutch * side_ : kDutchSoft * side_;
            composed = true;
            break;
        }
        case SceneSense::Fight:
            if (!hasOther) {
                break;
            }
            if (pair <= kFightNear) {
                // Двойной план сбоку от линии, чуть снизу.
                framing = Framing::TwoShot;
                center = Vector2Scale(Vector2Add(plan.subject, other), 0.5f);
                aim(yawOf(along) + 90.0f * pairSide, kAxisTurn);
                shot.height = kFightHeight;
                shot.pitch = kFightPitch;
                shot.fovY = kFightFov;
                shot.distance = std::clamp(fitDistance(pair * 0.5f + 1.5f, kFightFov, kFightPitch), 5.0f, 25.0f);
                shot.smoothing = kMediumSmoothing;
                shot.offset = Vector2Subtract(center, plan.subject);
                shot.yaw = yaw_;
            } else if (shoulders) {
                // Издалека — через плечо стрелка на цель.
                framing = Framing::OverShoulder;
                overShoulder(plan.subject, Vector2Negate(along), ground, kShoulderEye, other, otherGround, 1.3f,
                             lensFor(3.0f, pair + 2.0f, 16.0f, kShoulderFov));
                shot.smoothing = kShoulderSmoothing;
            }
            if (plan.panic) {
                roll = kDutchSoft * side_;
            }
            composed = framing != Framing::Standard;
            break;
        case SceneSense::Ambush:
            if (!hasOther || !shoulders) {
                break;
            }
            // Через плечо того, кто подкрадывается, на ничего не подозревающего.
            framing = Framing::OverShoulder;
            overShoulder(other, along, otherGround, kAmbushEye, plan.subject, ground, 1.45f,
                         lensFor(2.4f, pair + 2.0f, 16.0f, 42.0f));
            shot.smoothing = kShoulderSmoothing;
            composed = true;
            break;
        case SceneSense::Threat:
            framing = Framing::Low;
            if (hasOther && !shoulders) {
                framing = Framing::Standard;
                break;
            }
            if (hasOther && pair <= kThreatReach) {
                if (plan.menacing) {
                    // Герой — угроза: снизу из-за плеча жертвы, он нависает.
                    overShoulder(other, along, otherGround, kMenaceEye, plan.subject, ground, 1.5f,
                                 lensFor(2.4f, pair + 2.0f, 16.0f, 44.0f));
                } else {
                    // Угроза видна: низко из-за плеча героя — она нависает над ним.
                    overShoulder(plan.subject, Vector2Negate(along), ground, kThreatEye, other, otherGround, 1.4f,
                                 lensFor(2.6f, pair + 2.0f, 16.0f, 42.0f));
                    if (plan.panic) {
                        roll = kDutch * side_;
                    }
                }
                shot.smoothing = kShoulderSmoothing;
            } else {
                // Один — снизу на него, вполоборота.
                if (facing) {
                    aim(*facing + side_ * kMenaceAngle, kCloseTurn);
                }
                shot.height = 1.5f;
                shot.pitch = plan.menacing ? kMenacePitch : 2.0f;
                shot.distance = kMenaceDistance;
                shot.fovY = kMenaceFov;
                shot.smoothing = kCloseSmoothing;
                shot.offset = {};
                shot.yaw = yaw_;
                shot.frameX = thirds(yaw_);
                if (plan.panic && !plan.menacing) {
                    roll = kDutch * side_;
                }
            }
            composed = true;
            break;
        case SceneSense::Grief:
        case SceneSense::Helpless: {
            // Сверху: человек маленький, мир над ним.
            const bool grief = sense_ == SceneSense::Grief;
            framing = Framing::High;
            if (facing) {
                aim(*facing + side_ * kHighAngle, kCloseTurn);
            }
            shot.height = grief ? kGriefLook : kHelplessLook;
            shot.pitch = grief ? kGriefPitch : kHelplessPitch;
            shot.distance = grief ? kGriefDistance : kHelplessDistance;
            shot.fovY = grief ? kGriefFov : kHelplessFov;
            shot.smoothing = kCloseSmoothing;
            shot.offset = {};
            shot.yaw = yaw_;
            shot.frameX = thirds(yaw_);
            composed = true;
            break;
        }
        case SceneSense::Daily:
            if (plan.size == ShotSize::Close && moveHeading) {
                // Панорама: глаз стоит у пути впереди-сбоку и поворачивается за
                // героем; прошёл и отошёл — новое место встык.
                framing = Framing::Pan;
                const Vector2 run = plan.move;
                const float d = Vector2Distance(panEye_, plan.subject);
                const bool passed = Vector2DotProduct(Vector2Subtract(plan.subject, panEye_), run) > 0.0f;
                if (fresh || framing_ != Framing::Pan || d > kPanFar || (passed && d > kPanPassed)) {
                    panEye_ = Vector2Add(Vector2Add(plan.subject, Vector2Scale(run, kPanAhead)),
                                         Vector2Scale(perpendicular(run, side_), kPanSide));
                    cutHere = !fresh;
                }
                aimFrom(shot, plan.subject, panEye_, camera.heightAt(panEye_) + kPanEye, plan.subject,
                        ground + kPanLook);
                shot.height = kPanLook;
                shot.fovY = kPanFov;
                shot.smoothing = kPanSmoothing;
                yaw_ = wrapDegrees(shot.yaw);
                frameX_ = thirdsOffset(screenDirection(*moveHeading, yaw_), frameX_);
                shot.frameX = frameX_;
                composed = true;
            }
            break;
        default:
            break;
        }
    }

    if (!composed && plan.vista > 0.0f) {
        // Кадр-пауза: вдаль, без облёта; горизонт — на нижней трети.
        center = plan.subject;
        shot.height = kVistaHeight;
        shot.pitch = kVistaPitch;
        shot.fovY = kVistaFov;
        shot.distance = plan.vista;
        shot.smoothing = kWideSmoothing;
        shot.frameY = -kThird;
        if (plan.viewFrom) {
            yaw_ = *plan.viewFrom;
        }
    } else if (!composed && plan.size == ShotSize::Close) {
        shot.height = kCloseHeight;
        shot.distance = kCloseDistance;
        shot.pitch = kClosePitch;
        shot.fovY = kCloseFov;
        shot.smoothing = kCloseSmoothing;
        shot.frameY = kEyeLine;
        if (facing) {
            aim(*facing + side_ * kCloseAngle, kCloseTurn);
        }
        shot.frameX = thirds(yaw_);
    } else if (!composed && radius > spread && reach > 1.0f && !plan.viewFrom) {
        // Передний план и даль: позади героя, взгляд — туда, куда он идёт.
        const Vector2 ahead = Vector2Scale(Vector2Subtract(farthest, plan.subject), 1.0f / reach);
        center = Vector2Add(plan.subject, Vector2Scale(ahead, std::min(reach * kDepthLead, kDepthLeadMax)));
        shot.height = kMediumHeight;
        shot.pitch = kDepthPitch;
        shot.fovY = plan.size == ShotSize::Wide ? kWideFov : kMediumFov;
        shot.distance = plan.size == ShotSize::Wide ? kDepthWideDistance : kDepthMediumDistance;
        shot.smoothing = kMediumSmoothing;
        aim(yawOf(Vector2Negate(ahead)) + side_ * kDepthAngle, kAxisTurn);
    } else if (!composed && plan.size == ShotSize::Medium) {
        shot.height = kMediumHeight;
        shot.pitch = kMediumPitch;
        shot.fovY = kMediumFov;
        shot.distance = std::clamp(fitDistance(radius, shot.fovY, shot.pitch), kMediumMin, kMediumMax);
        shot.smoothing = kMediumSmoothing;
        if (plan.viewFrom) {
            aim(*plan.viewFrom + side_ * kViewFromAngle, kViewFromTurn);
        } else if (hasOther && pair > 1.0f) {
            // Двое — сбоку от линии между ними, всегда с одной стороны (180°).
            aim(yawOf(along) + 90.0f * pairSide, kAxisTurn);
        } else if (reach > 1.0f) {
            // Несколько — сбоку от линии между героем и самым дальним: из двух
            // сторон та, что ближе к нынешней камере (при склейке — своя).
            const float line = yawOf(Vector2Subtract(farthest, plan.subject));
            const float a = line + 90.0f;
            const float b = line - 90.0f;
            const bool first =
                fresh ? side_ > 0.0f : std::abs(wrapDegrees(a - yaw_)) <= std::abs(wrapDegrees(b - yaw_));
            aim(first ? a : b, kAxisTurn);
        } else if (facing) {
            aim(*facing + side_ * kCloseAngle, kCloseTurn);
        }
    } else if (!composed) {
        shot.height = 0.0f;
        shot.pitch = kWidePitch;
        shot.fovY = kWideFov;
        shot.distance = std::clamp(fitDistance(radius, shot.fovY, shot.pitch), kWideMin, kWideMax);
        shot.smoothing = kWideSmoothing;
        // Неподвижно: курс прежний (с прошлого кадра или подсказки места).
        if (fresh && plan.viewFrom) {
            yaw_ = *plan.viewFrom;
        }
    }
    if (!composed) {
        if (plan.lookHeight > 0.0f) {
            shot.height = plan.lookHeight;
            shot.frameY = 0.0f;
        }
        shot.offset = Vector2Subtract(center, plan.subject);
        // Паника — кадр наклонён и в обычном плане.
        if (plan.panic && plan.size != ShotSize::Wide && plan.vista <= 0.0f) {
            roll = kDutchSoft * side_;
        }
    }
    // Героя заслонили дом или кроны: не висеть над крышей, а обойти — только
    // в приёмах, где курс свободен (через плечо и панорама держат свою точку).
    const bool orbiting = framing == Framing::Standard || framing == Framing::High ||
                          (framing == Framing::Low && !hasOther);
    const bool movable = orbiting && plan.vista <= 0.0f && plan.size != ShotSize::Wide;
    dodgeRest_ = std::max(0.0f, dodgeRest_ - dt);
    blocked_ = movable && !fresh && settle_ <= 0.0f && dodgeRest_ <= 0.0f && camera.cinemaBlocked() ? blocked_ + dt
                                                                                                    : 0.0f;
    if (blocked_ >= kBlockedTime) {
        blocked_ = 0.0f;
        for (float step = kBlockedStep; step <= 180.0f; step += kBlockedStep) {
            if (camera.cinemaClearFrom(yaw_ + step)) {
                yaw_ += step;
                dodged_ = true;
                dodgeRest_ = kDodgeRest;
                break;
            }
            if (camera.cinemaClearFrom(yaw_ - step)) {
                yaw_ -= step;
                dodged_ = true;
                dodgeRest_ = kDodgeRest;
                break;
            }
        }
    }
    yaw_ = wrapDegrees(yaw_);
    if (orbiting || !composed) {
        shot.yaw = yaw_;
    }
    shot.roll = roll;
    shot.fovY /= pushIn_;
    shot.distance *= pullOut_;
    // Через плечо заслонили: камера, обходя ствол или стену, въехала бы в
    // переднего — дальше двойной план (встык).
    const bool shoulder = framing == Framing::OverShoulder || framing == Framing::Reverse ||
                          (framing == Framing::Low && hasOther);
    shoulderTime_ = shoulder && !fresh && camera.cinemaBlocked() ? shoulderTime_ + dt : 0.0f;
    if (shoulderTime_ >= kShoulderBlocked) {
        shoulderBlocked_ = true;
        shoulderTime_ = 0.0f;
        pendingCut_ = false;
        reverse_ = 0;
    }
    // Куда он идёт по экрану — запомнить для следующих склеек (180°).
    if (moveHeading) {
        memory_.rememberMotion(plan.subjectId, screenDirection(*moveHeading, shot.yaw));
    }

    if (fresh) {
        // Далеко или впервые — склейка с проявкой из чёрного, близко — плавно;
        // новый приём (смысл сцены сменился) — встык.
        const float away = Vector2Distance(camera.focus(), Vector2Add(plan.subject, shot.offset));
        const float before = std::max(camera.cinemaShot().distance, 0.1f);
        const float ratio = std::max(before, shot.distance) / std::max(std::min(before, shot.distance), 0.1f);
        if (!started_ || (cutting && away > std::max(kCutDistance, kCutShare * shot.distance))) {
            shot.cut = true;
            blackout_ = 1.0f;
            settle_ = 0.0f;
        } else if (recompose || ratio > kSizeCut || framing != Framing::Standard) {
            shot.cut = true; // другая крупность, другой приём — встык
            settle_ = 0.0f;
        } else {
            settle_ = kSettleTime;
        }
    } else if (cutHere || framing != framing_) {
        shot.cut = true; // «восьмёрка», новое место панорамы, другой приём — встык
        settle_ = 0.0f;
    }
    if (settle_ > 0.0f) {
        shot.smoothing = std::max(shot.smoothing, kSettleSmoothing);
    }
    framing_ = framing;
    started_ = true;
    camera.setCinemaShot(shot);
}

} // namespace lw::ui
