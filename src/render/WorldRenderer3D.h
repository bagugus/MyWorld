#pragma once

#include "render/Animator.h"
#include "render/InteriorStage.h"
#include "render/LandUse.h"
#include "render/Lighting3D.h"
#include "render/Props3D.h"
#include "render/Terrain3D.h"
#include "render/Trees3D.h"
#include "sim/Id.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lw {

class CameraController;
struct Building;
struct BuildingModel;
class Map;
class MapRenderer;
class World;

// Мир в 3D (этап 11): рельеф с водой (Terrain3D), здания, существа, тела,
// грузы и вороны — низкополигональными моделями (render/Models3D), мосты — с
// перилами и опорами; всё стоит на поверхности. Свет — по времени суток и
// погоде (render/Lighting, 11.4): солнце или луна с тенями, небо, дымка, ночью —
// свет в окнах. Существа двигаются по занятию (Animator, этап 12а.11): сидят,
// едят, копают, дерутся, падают, над головой — значки эмоций. Леса — отдельными деревьями (Trees3D, 11.6). Вне леса (11.9) —
// посевы, трава, заборы, столбы и прочая мелочь (Props3D) по раскладке
// LandUse; дворы и столбы она узнаёт по зданиям (updateBuildingBounds). Только читает
// состояние симуляции. Создавать после InitWindow,
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
    const Trees3D& trees() const { return trees_; }
    bool treesEnabled() const { return treesEnabled_; }
    void setTreesEnabled(bool enabled) { treesEnabled_ = enabled; }
    const Props3D& props() const { return props_; }
    bool propsEnabled() const { return propsEnabled_; }
    void setPropsEnabled(bool enabled) { propsEnabled_ = enabled; }
    // Витрина поз (отладка, этап 12а.11): все движения людей, зомби и зверей
    // рядами в точке взгляда камеры — посмотреть их, не дожидаясь в мире.
    // row < 0 — все ряды, иначе только этот ряд (крупно); nullopt — выключена.
    void setPoseGallery(std::optional<int> row) { poseGallery_ = row; }
    // Сколько сеток деревьев и мелочи ещё не построено (снимок ждёт их).
    int pending() const { return trees_.pending() + props_.pending(); }
    // Облик сезона на заданную долю года (0 — 1 марта) вместо календаря: листва
    // и снежный покров — посмотреть лес в любой сезон без промотки. Только
    // облик, симуляция не меняется; nullopt — по календарю и погоде.
    std::optional<float> foliageYear() const { return foliageYear_; }
    void setFoliageYear(std::optional<float> year) { foliageYear_ = year; }
    const SceneShading& shading() const { return shading_; }
    bool shadowsEnabled() const { return shadowsEnabled_; }
    void setShadowsEnabled(bool enabled) { shadowsEnabled_ = enabled; }
    // Дальность прорисовки существ, тел, грузов, деревьев и мелочи на зданиях,
    // метры: в последней четверти они растворяются, дальше не рисуются (деревья —
    // не дальше kMaxTreeRange, за ней лес — текстурой на земле). Мосты — втрое дальше.
    float drawDistance() const { return drawDistance_; }
    void setDrawDistance(float meters);
    static constexpr float kMinDrawDistance = 200.0f;
    static constexpr float kMaxDrawDistance = 8000.0f;
    static constexpr float kMaxTreeRange = 2500.0f;
    // Заборы, столбы, стога и прочая крупная мелочь — не дальше этого.
    static constexpr float kMaxPropsRange = 900.0f;
    // Подробные модели деревьев — ближе этого, дальше — упрощённые.
    static constexpr float kNearTreeRange = 350.0f;
    // Выбранный в инспекторе: виден на любой дальности, над ним — метка.
    void setHighlight(EntityId id) { highlight_ = id; }
    // Куда обращено существо на экране (atan2(dx, dy), сглаженный поворот модели);
    // нет — если оно не рисовалось в прошлом кадре. Для вида из-за плеча.
    std::optional<float> drawnHeading(EntityId id) const { return animator_.heading(id); }
    // Коробки зданий (от фундамента до конька) для кинокамеры: она не заходит в
    // стены (CameraController::setObstacles). Считаются один раз (здания не
    // меняются) — при отрисовке или раньше; адрес списка не меняется. Тогда же
    // LandUse раскладывает дворы, заборы, столбы и мостки.
    const std::vector<BoundingBox>& buildingBounds() const { return buildingBounds_; }
    void updateBuildingBounds(const World& world);
    // Здание, открытое для кинокамеры (этап 12а.14): его режиссёр открывает,
    // когда герой сцены дома, — тогда вместо коробки снаружи рисуются комнаты,
    // жильцы сидят и лежат по местам, стены между камерой и героем спрятаны,
    // вечером горят лампы.
    InteriorStage& interiors() { return interior_; }
    const InteriorStage& interiors() const { return interior_; }
    // Камера внутри открытого здания: осадков на экране нет.
    bool interiorView(const CameraController& camera) const;

private:
    // Часть моста: коробка с серединой center, полуразмерами half, повёрнутая на yaw.
    struct Part {
        Vector3 center{};
        Vector3 half{};
        float yaw = 0.0f;
        Color color{};
        bool bottom = false; // с дном: настил виден снизу
    };

    // Модель здания без облика сезона и мелочи: участок, земля под ним, дверь.
    BuildingModel buildingModel(const Building& building, EntityId id) const;
    // Комната открытого здания: какие стены и мебель спрятать от камеры, горят
    // ли лампы и какие из них светят.
    void prepareInterior(const InteriorLayout& layout, Vector3 eye, const SceneLight& light);
    // Снежный покров для облика: погода или сезон вручную.
    float snowCover(const World& world) const;
    // Здания, мосты, существа, тела, грузы и вороны — в открытый rlBegin(RL_QUADS).
    // shadowPass — в карту теней: только то, что в её квадрате, без мелочи.
    void emitScene(const World& world, const CameraController& camera, float alpha, bool shadowPass);

    LandUse land_;
    Terrain3D terrain_;
    Trees3D trees_;
    Props3D props_;
    InteriorStage interior_;
    std::vector<bool> hiddenWalls_;
    std::vector<bool> hiddenFurniture_;
    bool lampsOn_ = false;
    SceneShading shading_;
    std::vector<Part> bridgeParts_; // настил, перила и опоры мостов: не меняются
    // Позы и движения существ (этап 12а.11): раз за кадр, второй проход
    // (тень и кадр) берёт уже посчитанное.
    Animator animator_;
    std::uint32_t frame_ = 0;
    float dt_ = 0.0f;
    std::unordered_set<EntityId> litBuildings_; // где ночью горит свет
    float lights_ = 0.0f;                      // доля светящихся окон в них
    bool shadowsEnabled_ = true;
    bool treesEnabled_ = true;
    bool propsEnabled_ = true;
    std::optional<int> poseGallery_;
    std::optional<float> foliageYear_;
    float drawDistance_ = 1500.0f;
    EntityId highlight_ = kInvalidId;
    std::vector<BoundingBox> buildingBounds_;
    int buildingsDrawn_ = 0;
    int creaturesDrawn_ = 0;
};

} // namespace lw
