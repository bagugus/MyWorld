#pragma once

#include "render/Lighting3D.h"
#include "render/Terrain3D.h"
#include "sim/Id.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lw {

class CameraController;
class Map;
class MapRenderer;
class World;

// Мир в 3D (этап 11): рельеф с водой (Terrain3D), здания, существа, тела,
// грузы и вороны — низкополигональными моделями (render/Models3D), мосты — с
// перилами и опорами; всё стоит на поверхности. Свет — по времени суток и
// погоде (render/Lighting, 11.4): солнце или луна с тенями, небо, дымка, ночью —
// свет в окнах. Только читает состояние симуляции. Создавать после InitWindow,
// уничтожать до CloseWindow.
class WorldRenderer3D {
public:
    // Текстуры карты общие с видом сверху.
    WorldRenderer3D(MapRenderer& textures, const Map& map);

    // Рисует небо, карту теней и мир (сам вызывает BeginMode3D и EndMode3D).
    // alpha — доля тика, как у WorldRenderer::draw.
    void draw(const World& world, const CameraController& camera, float alpha);

    int buildingsDrawn() const { return buildingsDrawn_; }
    int creaturesDrawn() const { return creaturesDrawn_; }
    const Terrain3D& terrain() const { return terrain_; }
    const SceneShading& shading() const { return shading_; }
    bool shadowsEnabled() const { return shadowsEnabled_; }
    void setShadowsEnabled(bool enabled) { shadowsEnabled_ = enabled; }

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

    // Облик меняется раз за кадр: второй проход берёт уже посчитанный.
    Motion& motionOf(EntityId id, float dx, float dy, float stride, float dt);
    // Здания, мосты, существа, тела, грузы и вороны — в открытый rlBegin(RL_QUADS).
    // shadowPass — в карту теней: только то, что в её квадрате, без мелочи.
    void emitScene(const World& world, const CameraController& camera, float alpha, bool shadowPass);

    Terrain3D terrain_;
    SceneShading shading_;
    std::vector<Part> bridgeParts_; // перила и опоры мостов: не меняются
    std::unordered_map<EntityId, Motion> motion_;
    std::uint32_t frame_ = 0;
    float dt_ = 0.0f;
    std::unordered_set<EntityId> litBuildings_; // где ночью горит свет
    float lights_ = 0.0f;                      // доля светящихся окон в них
    bool shadowsEnabled_ = true;
    int buildingsDrawn_ = 0;
    int creaturesDrawn_ = 0;
};

} // namespace lw
