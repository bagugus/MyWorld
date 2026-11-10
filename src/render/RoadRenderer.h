#pragma once

// Дороги по иерархии в виде сверху (этап 2а.3): на общем плане — линии по
// классам (шоссе, трасса, просёлок), чтобы сеть читалась, когда полотно
// мельче пикселя; вблизи — разметка шоссе и трасс. Только читает сеть дорог.

#include "sim/Roads.h"

#include <raylib.h>

#include <vector>

namespace lw {

class World;

class RoadRenderer {
public:
    explicit RoadRenderer(const World& world);

    // Вызывать между BeginMode2D и EndMode2D, поверх карты и под зданиями.
    void draw(const Map& map, const Rectangle& visible, float zoom) const;

private:
    // Участок сети — ось в метрах только по изломам, с длиной от начала у
    // каждой точки (для пунктира).
    struct Line {
        RoadClass kind = RoadClass::Track;
        std::vector<Vector2> points;
        std::vector<float> along;
        Rectangle bounds{};
    };

    void drawOverview(const Rectangle& visible, float zoom, float strength) const;
    void drawMarkings(const Map& map, const Rectangle& visible, float zoom, float strength) const;

    std::vector<Line> lines_;
};

} // namespace lw
