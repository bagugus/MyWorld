#pragma once

#include "render/Interior.h"
#include "render/Models3D.h"
#include "sim/Id.h"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
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
    // один раз) и кто где сейчас. dt — реальные секунды кадра: сменил занятие —
    // идёт к новому месту шагом (через двери комнат), вошёл в дом — от входа;
    // dt = 0 — сразу на местах.
    void update(const World& world, float dt = 0.0f);

    const InteriorLayout* layout() const { return active() ? &*layout_ : nullptr; }
    // Где сейчас человек в открытом здании; nullptr — его там нет.
    const Placement* placementOf(EntityId id) const;
    const std::vector<Placement>& placements() const { return placements_; }
    // Меняется с каждой новой планировкой (камера — склейкой).
    std::uint32_t version() const { return version_; }

    // Квартира человека среди всех квартир здания (этаж × квартир на этаже).
    static int flatOf(const World& world, entt::entity person, EntityId building, int totalFlats);

private:
    // Где человек сейчас нарисован и куда идёт.
    struct Walker {
        float x = 0.0f;
        float z = 0.0f;
        float heading = 0.0f;
        int spot = -2;        // к какому месту идёт (или где сидит); −1 — стоит без места
        float goalX = 0.0f;   // куда идёт
        float goalZ = 0.0f;
        int stage = -1;       // где сидел до этого
        std::vector<std::pair<float, float>> path; // точки пути до цели (последняя — цель)
        std::uint32_t seen = 0;
    };
    // Проём двери между комнатами.
    struct Doorway {
        float x = 0.0f;
        float z = 0.0f;
        bool alongX = true; // стена вдоль X: проходят по Z
        int a = -1;         // комнаты по обе стороны (a — с меньшей стороны)
        int b = -1;
    };
    void indexDoors();
    // Путь от точки до точки по комнатам через проёмы.
    std::vector<std::pair<float, float>> route(float x0, float z0, float x1, float z1) const;
    // Откуда входит пришедший домой: с лестницы или от входной двери.
    std::pair<float, float> entrance() const;

    const Terrain3D& terrain_;
    EntityId building_ = kInvalidId;
    EntityId focus_ = kInvalidId;
    std::optional<InteriorLayout> layout_;
    EntityId layoutBuilding_ = kInvalidId;
    int layoutLevel_ = -1;
    std::vector<Placement> placements_;
    std::unordered_map<EntityId, std::size_t> index_;
    std::uint32_t version_ = 0;
    std::unordered_map<EntityId, Walker> walkers_;
    std::vector<Doorway> doors_;
    std::uint32_t walkedVersion_ = 0; // для какой планировки помним, кто где
    std::uint32_t frame_ = 0;
    std::uint64_t lastTick_ = 0;
    float rate_ = 1.0f;               // секунд симуляции на реальную секунду (сглажено)
};

} // namespace lw
