#pragma once

#include "sim/Id.h"

#include <raylib.h>

#include <optional>

namespace lw {

class Terrain3D;

// Как смотрим на мир: сверху (2D, этапы 1–10) или в 3D (этап 11): орбита вокруг
// точки на земле, свободный полёт, из-за плеча выбранного существа (11.7).
enum class ViewMode { TopDown, Orbit, Fly, Shoulder };

// Настройки камеры (панель «Статистика» → «Камера», хранятся в camera.ini).
struct CameraSettings {
    float smoothing = 0.12f;       // секунд: за столько камера проходит ~63 % пути к цели; 0 — сразу
    float inertia = 0.35f;         // секунд: затухание скорости после отпускания; 0 — без инерции
    float mouseSensitivity = 1.0f; // множитель вращения мышью
    float keyboardSpeed = 1.0f;    // множитель панорамы и вращения клавишами
    float flySpeed = 1.0f;         // множитель скорости полёта (колесо в полёте)
    bool invertY = false;          // мышь вверх — взгляд вниз
    bool holdAltitude = true;      // в полёте держать высоту над рельефом

    bool operator==(const CameraSettings&) const = default;
};

// За кем смотрит вид из-за плеча: точка на земле, куда обращено (atan2(dx, dy),
// как у моделей), высота глаз над поверхностью.
struct CameraSubject {
    EntityId id = kInvalidId;
    Vector2 at{};
    float heading = 0.0f;
    float height = 1.6f;
};

// Камера наблюдателя. Мир один и тот же, меняется только взгляд на него.
// Точка мира (x, y) в метрах; в 3D x — восток (ось X), y — юг (ось Z), высота — ось Y.
// В 3D точка мира лежит на поверхности рельефа (Terrain3D), без него — на высоте 0.
// Зум измеряется в пикселях на метр; в 3D — у точки, на которую смотрит камера.
//
// Ввод меняет цель, а камера плавно её догоняет (CameraSettings::smoothing);
// панорама разгоняется и после отпускания катится по инерции.
//
// Вид сверху:
//   колесо мыши        — зум к курсору
//   ПКМ / СКМ + мышь   — панорама
// Орбита:
//   колесо мыши        — ближе / дальше, к курсору
//   ПКМ + мышь         — вращение и наклон
//   СКМ + мышь         — панорама
//   Q / E, R / F       — вращение, наклон
// Полёт (G):
//   WASD               — вперёд, назад, вбок; E / Q — выше / ниже; Shift — быстрее
//   ПКМ + мышь         — осмотреться
//   колесо мыши        — скорость
// Из-за плеча (T, нужен выбранный):
//   ПКМ + мышь         — осмотреться вокруг; колесо — ближе / дальше
// Общее:
//   WASD / стрелки     — панорама (в орбите — относительно взгляда)
//   Home               — показать весь мир
//   V                  — сверху / 3D
class CameraController {
public:
    CameraController(float worldWidth, float worldHeight);

    // Рельеф для 3D: камера смотрит на поверхность и не опускается под неё.
    void setTerrain(const Terrain3D* terrain);

    // mouseFree / keyboardFree — false, когда ввод забрал интерфейс.
    void update(float dt, bool mouseFree, bool keyboardFree);
    // Показать весь мир (плавно).
    void reset();
    // Точка мира (x, y) в метрах — в центре экрана, зум в пикселях на метр.
    // Сразу, без плавного перехода; полёт и вид из-за плеча сменяются орбитой.
    void lookAt(float x, float y, float zoom);
    // То же без смены зума и плавно: слежение за объектом. В полёте и из-за
    // плеча камера не двигается.
    void moveTo(float x, float y);
    // Закончить плавные переходы сразу: камера встаёт на цель.
    void snap();

    // Смена вида сохраняет точку в центре экрана и зум; переход между видом
    // сверху и 3D — плавным наклоном.
    void setMode(ViewMode mode);
    ViewMode mode() const { return mode_; }
    bool is3d() const { return mode_ != ViewMode::TopDown; }
    // Направление орбиты: yaw — с какой стороны смотрим (0 — с юга, север вверху
    // экрана, как на карте; 90 — с востока), pitch — наклон над горизонтом.
    void setOrbitAngles(float yawDegrees, float pitchDegrees);
    // За кем смотреть из-за плеча; нет — вид из-за плеча сменяется орбитой.
    // Сменился выбранный — камера перелетает к новому плавно.
    void setSubject(std::optional<CameraSubject> subject);
    bool hasSubject() const { return subject_.has_value(); }

    CameraSettings& settings() { return settings_; }
    const CameraSettings& settings() const { return settings_; }
    // Высота глаз над поверхностью, метры (в 3D).
    float eyeHeight() const;

    // Точка мира в центре экрана.
    Vector2 focus() const;
    float zoom() const;
    // Пикселей на метр у точки мира: в 3D дальнее мельче.
    float zoomAt(Vector2 point) const;
    // Точка земли под пикселем экрана; в 3D — нет, если там небо.
    std::optional<Vector2> screenToWorld(Vector2 screen) const;
    // Пиксель экрана точки мира на высоте height метров над поверхностью; нет,
    // если она за камерой.
    std::optional<Vector2> worldToScreen(Vector2 point, float height = 0.0f) const;

    // Камера вида сверху. В 3D — равносильная ей: та же точка в центре и тот же
    // зум; по ней рисуются отладочные слои (beginOverlay).
    const Camera2D& camera() const { return camera_; }
    const Camera3D& camera3d() const { return camera3d_; }

    // Мировые координаты для отладочных слоёв, нарисованных 2D-функциями
    // (линии, круги в метрах): в 3D они ложатся на рельеф поверх всего.
    void beginOverlay() const;
    void endOverlay() const;

private:
    // Положение орбиты: точка на земле, углы в градусах, расстояние в метрах.
    struct OrbitPose {
        Vector2 focus{};
        float yaw = 0.0f;
        float pitch = 55.0f;
        float distance = 1000.0f;
    };

    OrbitPose wholeWorldPose() const;
    // Орбита, из которой камера видна так же, как сейчас (смена вида без скачка).
    OrbitPose poseFromCamera() const;
    void enterOrbitFromCamera();
    void updateTopDown(float dt, bool mouseFree, bool keyboardFree);
    void updateOrbit(float dt, bool mouseFree, bool keyboardFree);
    void updateFly(float dt, bool mouseFree, bool keyboardFree);
    void updateShoulder(float dt, bool mouseFree);
    // Пересчитать camera3d_ и равносильную 2D-камеру из состояния вида.
    void refresh();
    void refreshTopDown();
    void refreshOrbit();
    void refreshFly();
    void refreshShoulder();
    void setClipPlanes(double nearPlane) const;
    // Доля пути к цели за кадр по настройке плавности; scale — во сколько раз
    // медленнее обычного.
    float approach(float dt, float scale = 1.0f) const;
    // Скорость панорамы к желаемой: разгон при нажатых клавишах, иначе инерция.
    Vector3 pannedVelocity(Vector3 velocity, Vector3 wanted, bool pressed, float dt) const;
    // Расстояние до точки, при котором у неё зум zoom пикселей на метр, и обратно.
    float distanceForZoom(float zoom) const;
    float zoomForDistance(float distance) const;
    // Высота поверхности в точке мира: в 3D — рельеф, иначе 0.
    float surfaceAt(Vector2 point) const;
    // Где луч встречает землю (рельеф, без него — плоскость высоты 0).
    std::optional<Vector2> groundHit(const Ray& ray) const;
    // Желаемые глаз и точка взгляда вида из-за плеча — смещения от головы существа.
    void shoulderGoal(Vector3& eye, Vector3& target) const;
    Vector3 shoulderPivot() const;
    float maxDistance() const;

    float worldWidth_;
    float worldHeight_;
    const Terrain3D* terrain_ = nullptr;
    ViewMode mode_ = ViewMode::TopDown;
    CameraSettings settings_;
    Camera2D camera_{};
    Camera3D camera3d_{};

    // Вид сверху: центр и зум — текущие и цель. Пока колесо приближает к
    // курсору, точка мира zoomAnchor_ держится под пикселем zoomAnchorScreen_.
    Vector2 topCenter_{};
    float topZoom_ = 1.0f;
    Vector2 topGoalCenter_{};
    float topGoalZoom_ = 1.0f;
    bool zoomAnchored_ = false;
    Vector2 zoomAnchor_{};
    Vector2 zoomAnchorScreen_{};
    Vector3 panVelocity_{}; // метров в секунду по (x, y мира): клавиши и бросок мышью

    // Орбита: текущее положение и цель; savedYaw_/savedPitch_ — углы, к которым
    // 3D вернётся после вида сверху; toTopDown_ — идёт выпрямление перед ним.
    OrbitPose orbit_;
    OrbitPose orbitGoal_;
    float savedYaw_ = 0.0f;
    float savedPitch_ = 55.0f;
    bool toTopDown_ = false;

    // Полёт: глаз, взгляд (yaw — как у орбиты, pitch — над горизонтом,
    // отрицательный — вниз), высота над рельефом и скорость.
    Vector3 flyEye_{};
    float flyYaw_ = 0.0f;
    float flyPitch_ = -30.0f;
    float flyGoalYaw_ = 0.0f;
    float flyGoalPitch_ = -30.0f;
    float flyAltitude_ = 50.0f;
    Vector3 flyVelocity_{};
    Vector2 flyFocus_{}; // точка земли в центре экрана

    // Из-за плеча: сглаженный курс существа, смещение взгляда мышью, расстояние;
    // глаз и точка взгляда — смещения от головы существа, догоняют желаемые
    // (камера не отстаёт от бегущего и на большой скорости времени).
    std::optional<CameraSubject> subject_;
    float shoulderHeading_ = 0.0f;
    float shoulderYaw_ = 0.0f;
    float shoulderPitch_ = 14.0f;
    float shoulderDistance_ = 4.5f;
    Vector3 shoulderEye_{};
    Vector3 shoulderTarget_{};
};

} // namespace lw
