#pragma once

#include "sim/Id.h"

#include <raylib.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace lw {

class CameraController;

namespace ui {

// Операторская работа режиссёра (этап 12а.5), пока только в виде сверху:
// план кадра (общий — орда у деревни, средний — группа или двое, крупный —
// герой), кадрирование нескольких точек сразу, плавный пролёт с отъездом,
// когда новый кадр далеко, и медленный наезд в кульминации. Колесо
// пользователя не спорит с режиссёром: оно меняет общий множитель зума.
//
// В 3D этот оператор ничего не делает: в режиме «Сериал» кадр по тому же
// плану ставит кинокамера (CinemaCamera, 12а.7).

// План кадра.
enum class ShotSize : std::uint8_t {
    Wide,   // общий: орда и поселение, гонка за грузом издалека
    Medium, // средний: группа, двое, что вот-вот столкнутся
    Close,  // крупный: герой
    Count
};

// Что режиссёр хочет видеть в кадре (метры мира).
struct ShotPlan {
    bool valid = false;
    ShotSize size = ShotSize::Close;
    EntityId subjectId = kInvalidId; // кто главное (якорь кинокамеры)
    Vector2 subject{};          // главное: всегда в кадре
    std::vector<Vector2> extra; // что ещё уместить, если влезает в план
    // Откуда снимать, если это важно (курс камеры в градусах, как у орбиты):
    // дом, где сидит герой, — со стороны двери. Только кинокамера (12а.7).
    std::optional<float> viewFrom;
    // На какой высоте над землёй точка взгляда, метры; 0 — как принято для плана
    // (дом — середина стены, а не грудь человека внутри). Только кинокамера.
    float lookHeight = 0.0f;
    // Кадр-пауза (12а.9): не 0 — камера стоит на таком расстоянии (метры) от
    // точки взгляда низко, почти у горизонта, неподвижно, со стороны viewFrom:
    // рассвет, гроза, туман над рекой, орда вдали. Только кинокамера.
    float vista = 0.0f;
    // Сцена в комнате (12а.14): кинокамера стоит внутри здания. bounds — где
    // ей можно стоять (весь этаж: x0, z0, x1, z1 мира), room — комната героя,
    // facing — куда он обращён (atan2(dx, dz)), пол и потолок — мир; stage —
    // планировка и место героя: сменились — склейка. Только кинокамера.
    bool indoor = false;
    float floor = 0.0f;
    float ceiling = 0.0f;
    Vector4 bounds{};
    Vector4 room{};
    std::optional<float> facing;
    std::uint64_t stage = 0;
};

class Camerawork {
public:
    // Режиссёра включили заново: забыть множитель пользователя и наезд.
    void reset();
    // Сменился кадр: если новый далеко — пролёт.
    void cut() { pendingCut_ = true; }
    // Раз за кадр после слежения инспектора: поставить цель камеры. climax —
    // вот-вот кульминация (медленный наезд); dt — реальные секунды.
    void update(CameraController& camera, const ShotPlan& plan, bool climax, float dt);
    // Режиссёр не ведёт: только забыть, какой зум ставили.
    void idle() { lastZoom_ = 0.0f; flying_ = false; }

    bool flying() const { return flying_; }
    // Насколько камера наехала (1 — нет).
    float pushIn() const { return pushIn_; }

private:
    struct Framing {
        Vector2 center{};
        float zoom = 1.0f;
    };
    // Центр и зум, в которые уложится план (с множителями наезда и пользователя).
    Framing compose(const ShotPlan& plan) const;

    bool pendingCut_ = false;
    bool flying_ = false;
    float flightTime_ = 0.0f;
    float flightLength_ = 0.0f;
    Framing from_;
    float pushIn_ = 1.0f;
    float userScale_ = 1.0f;
    float lastZoom_ = 0.0f; // зум, поставленный в прошлом кадре; 0 — не ставили
};

const char* shotSizeTitle(ShotSize size);

} // namespace ui
} // namespace lw
