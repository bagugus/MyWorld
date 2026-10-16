#pragma once

namespace lw {

class CameraController;
class MapRenderer;
class World;

// Мир в 3D (этап 11.1) — та же симуляция, другой взгляд: карта плоскостью на
// земле, здания — коробками по виду, существа — столбиками своих цветов.
// Рельеф и вода (11.2), модели (11.3) и свет по времени суток (11.4) — дальше.
// Только читает состояние симуляции.
class WorldRenderer3D {
public:
    // Текстуры карты общие с видом сверху.
    explicit WorldRenderer3D(MapRenderer& map) : map_(map) {}

    // Рисует сам между BeginMode3D и EndMode3D. alpha — доля тика, как у
    // WorldRenderer::draw.
    void draw(const World& world, const CameraController& camera, float alpha);

    int buildingsDrawn() const { return buildingsDrawn_; }

private:
    MapRenderer& map_;
    int buildingsDrawn_ = 0;
};

} // namespace lw
