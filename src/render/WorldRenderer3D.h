#pragma once

#include "render/Terrain3D.h"

namespace lw {

class CameraController;
class Map;
class MapRenderer;
class World;

// Мир в 3D (этап 11): рельеф с водой (Terrain3D), здания — коробками по виду,
// существа — столбиками своих цветов; всё стоит на поверхности. Модели (11.3)
// и свет по времени суток (11.4) — дальше. Только читает состояние симуляции.
// Создавать после InitWindow, уничтожать до CloseWindow.
class WorldRenderer3D {
public:
    // Текстуры карты общие с видом сверху.
    WorldRenderer3D(MapRenderer& textures, const Map& map) : terrain_(map, textures) {}

    // Рисует сам между BeginMode3D и EndMode3D. alpha — доля тика, как у
    // WorldRenderer::draw.
    void draw(const World& world, const CameraController& camera, float alpha);

    int buildingsDrawn() const { return buildingsDrawn_; }
    const Terrain3D& terrain() const { return terrain_; }

private:
    Terrain3D terrain_;
    int buildingsDrawn_ = 0;
};

} // namespace lw
