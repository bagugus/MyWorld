#pragma once

#include "render/MapRenderer.h"

#include <raylib.h>

namespace lw {

class World;

// Рисует мир в мировых координатах: карту, здания, существ.
// Только читает состояние симуляции.
// Создавать после InitWindow, уничтожать до CloseWindow.
class WorldRenderer {
public:
    explicit WorldRenderer(const World& world);

    // Вызывать между BeginMode2D и EndMode2D.
    void draw(const World& world, const Camera2D& camera);

    const MapRenderer& map() const { return map_; }

private:
    MapRenderer map_;
};

} // namespace lw
