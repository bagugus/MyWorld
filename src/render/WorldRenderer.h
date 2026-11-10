#pragma once

#include "render/MapRenderer.h"
#include "render/RoadRenderer.h"

#include <raylib.h>

namespace lw {

class World;

// Рисует мир в мировых координатах: карту, здания, существ.
// Только читает состояние симуляции.
// Создавать после InitWindow, уничтожать до CloseWindow.
class WorldRenderer {
public:
    explicit WorldRenderer(const World& world);

    // Вызывать между BeginMode2D и EndMode2D. alpha — доля тика для плавного
    // движения (SimDriver::interpolationAlpha): существа рисуются между позицией
    // на начало последнего тика (0) и текущей (1).
    void draw(const World& world, const Camera2D& camera, float alpha);

    const MapRenderer& map() const { return map_; }
    MapRenderer& map() { return map_; }

private:
    MapRenderer map_;
    // Дороги по классам на общем плане и разметка вблизи (этап 2а.3).
    RoadRenderer roads_;
};

} // namespace lw
