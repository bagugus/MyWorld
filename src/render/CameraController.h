#pragma once

#include <raylib.h>

#include <optional>

namespace lw {

class Terrain3D;

// Как смотрим на мир: сверху (2D, этапы 1–10) или в 3D (этап 11).
enum class ViewMode { TopDown, Orbit };

// Камера наблюдателя. Мир один и тот же, меняется только взгляд на него.
// Точка мира (x, y) в метрах; в 3D x — восток (ось X), y — юг (ось Z), высота — ось Y.
// В 3D точка мира лежит на поверхности рельефа (Terrain3D), без него — на высоте 0.
// Зум измеряется в пикселях на метр; в 3D — у точки, на которую смотрит камера.
//
// Вид сверху:
//   колесо мыши        — зум к курсору
//   ПКМ / СКМ + мышь   — панорама
// 3D (орбита вокруг точки на земле):
//   колесо мыши        — ближе / дальше, к курсору
//   ПКМ + мышь         — вращение и наклон
//   СКМ + мышь         — панорама
//   Q / E, R / F       — вращение, наклон
// Общее:
//   WASD / стрелки     — панорама (в 3D — относительно взгляда)
//   Home               — показать весь мир
//   V                  — сменить вид
class CameraController {
public:
    CameraController(float worldWidth, float worldHeight);

    // Рельеф для 3D: камера смотрит на поверхность и не опускается под неё.
    void setTerrain(const Terrain3D* terrain);

    // mouseFree / keyboardFree — false, когда ввод забрал интерфейс.
    void update(float dt, bool mouseFree, bool keyboardFree);
    void reset();
    // Точка мира (x, y) в метрах — в центре экрана, зум в пикселях на метр.
    void lookAt(float x, float y, float zoom);
    // То же без смены зума: слежение за объектом.
    void moveTo(float x, float y);

    // Смена вида сохраняет точку в центре экрана и зум.
    void setMode(ViewMode mode);
    ViewMode mode() const { return mode_; }
    bool orbit() const { return mode_ == ViewMode::Orbit; }
    // Направление 3D-камеры: yaw — с какой стороны смотрим (0 — с юга, север вверху
    // экрана, как на карте; 90 — с востока), pitch — наклон над горизонтом.
    void setOrbitAngles(float yawDegrees, float pitchDegrees);

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
    void resetOrbit();
    // Пересчитать camera3d_ и равносильную 2D-камеру из точки, углов и расстояния.
    void refreshOrbit();
    void updateTopDown(float dt, bool mouseFree, bool keyboardFree);
    void updateOrbit(float dt, bool mouseFree, bool keyboardFree);
    // Расстояние до точки, при котором у неё зум zoom пикселей на метр, и обратно.
    float distanceForZoom(float zoom) const;
    float zoomForDistance(float distance) const;
    // Высота поверхности в точке мира: в 3D — рельеф, иначе 0.
    float surfaceAt(Vector2 point) const;

    float worldWidth_;
    float worldHeight_;
    const Terrain3D* terrain_ = nullptr;
    ViewMode mode_ = ViewMode::TopDown;
    Camera2D camera_{};

    Vector2 orbitFocus_{};
    float yaw_ = 0.0f;   // градусы
    float pitch_ = 55.0f; // градусы
    float distance_ = 1000.0f; // метры от камеры до точки
    Camera3D camera3d_{};
};

} // namespace lw
