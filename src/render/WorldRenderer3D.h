#pragma once

#include "render/Terrain3D.h"
#include "sim/Id.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lw {

class CameraController;
class Map;
class MapRenderer;
class World;

// Мир в 3D (этап 11): рельеф с водой (Terrain3D), здания, существа, тела,
// грузы и вороны — низкополигональными моделями (render/Models3D), мосты — с
// перилами и опорами; всё стоит на поверхности. Свет по времени суток (11.4) —
// дальше. Только читает состояние симуляции. Создавать после InitWindow,
// уничтожать до CloseWindow.
class WorldRenderer3D {
public:
    // Текстуры карты общие с видом сверху.
    WorldRenderer3D(MapRenderer& textures, const Map& map);

    // Рисует сам между BeginMode3D и EndMode3D. alpha — доля тика, как у
    // WorldRenderer::draw.
    void draw(const World& world, const CameraController& camera, float alpha);

    int buildingsDrawn() const { return buildingsDrawn_; }
    int creaturesDrawn() const { return creaturesDrawn_; }
    const Terrain3D& terrain() const { return terrain_; }

private:
    // Как движется существо на экране: куда смотрит и фаза шага. Только для
    // облика — в симуляцию не входит.
    struct Motion {
        float heading = 0.0f;
        float phase = 0.0f;
        std::uint32_t seen = 0; // кадр, когда существо рисовалось
    };
    struct Part {
        Vector3 min{};
        Vector3 max{};
        Color color{};
    };

    Motion& motionOf(EntityId id, float dx, float dy, float stride, float dt);

    Terrain3D terrain_;
    std::vector<Part> bridgeParts_; // перила и опоры мостов: не меняются
    std::unordered_map<EntityId, Motion> motion_;
    std::uint32_t frame_ = 0;
    int buildingsDrawn_ = 0;
    int creaturesDrawn_ = 0;
};

} // namespace lw
