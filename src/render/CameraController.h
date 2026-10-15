#pragma once

#include <raylib.h>

namespace lw {

// 2D-камера наблюдателя. Зум измеряется в пикселях на метр мира.
//   колесо мыши        — зум к курсору
//   ПКМ / СКМ + мышь   — панорама
//   WASD / стрелки     — панорама
//   Home               — показать весь мир
class CameraController {
public:
    CameraController(float worldWidth, float worldHeight);

    // mouseFree / keyboardFree — false, когда ввод забрал интерфейс.
    void update(float dt, bool mouseFree, bool keyboardFree);
    void reset();
    // Точка мира (x, y) в метрах — в центре экрана, зум в пикселях на метр.
    void lookAt(float x, float y, float zoom);

    const Camera2D& camera() const { return camera_; }
    float zoom() const { return camera_.zoom; }

private:
    float worldWidth_;
    float worldHeight_;
    Camera2D camera_{};
};

} // namespace lw
