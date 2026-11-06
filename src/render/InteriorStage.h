#pragma once

#include "render/Interior.h"
#include "render/Models3D.h"
#include "sim/Id.h"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lw {

struct Building;
class Terrain3D;
class World;

// Модель здания без облика сезона: участок, земля под ним (пол — выше самой
// высокой точки рельефа на участке), дверь, seed из id.
BuildingModel buildingModelOf(const Building& building, EntityId id, const Terrain3D& terrain);

// Здание, открытое для кинокамеры (этап 12а.14): когда герой сцены дома,
// режиссёр открывает его дом — вместо коробки снаружи рисуются комнаты
// (render/Interior), жильцы, которые сейчас дома, сидят и лежат по своим
// местам: спящий — в кровати, едящий — за столом (а с ним бодрствующие
// домашние — общий ужин), отдыхающий — на диване. В многоэтажке — этаж
// квартиры героя; квартира человека — по семье (фамилии) или группе.
// Только облик: читает мир, в симуляцию и хеш не входит; без графики.
class InteriorStage {
public:
    explicit InteriorStage(const Terrain3D& terrain) : terrain_(terrain) {}

    // Открыть здание: focus — за кем смотрит камера (его этаж и квартира).
    void open(EntityId building, EntityId focus);
    void close();
    bool active() const { return building_ != kInvalidId && layout_.has_value(); }
    EntityId building() const { return building_; }
    EntityId focus() const { return focus_; }

    // Раз за кадр до камеры и отрисовки: планировка (по зданию и этажу —
    // один раз) и кто где сейчас.
    void update(const World& world);

    const InteriorLayout* layout() const { return active() ? &*layout_ : nullptr; }
    // Где сейчас человек в открытом здании; nullptr — его там нет.
    const Placement* placementOf(EntityId id) const;
    const std::vector<Placement>& placements() const { return placements_; }
    // Меняется с каждой новой планировкой (камера — склейкой).
    std::uint32_t version() const { return version_; }

    // Квартира человека среди всех квартир здания (этаж × квартир на этаже).
    static int flatOf(const World& world, entt::entity person, EntityId building, int totalFlats);

private:
    const Terrain3D& terrain_;
    EntityId building_ = kInvalidId;
    EntityId focus_ = kInvalidId;
    std::optional<InteriorLayout> layout_;
    EntityId layoutBuilding_ = kInvalidId;
    int layoutLevel_ = -1;
    std::vector<Placement> placements_;
    std::unordered_map<EntityId, std::size_t> index_;
    std::uint32_t version_ = 0;
};

} // namespace lw
