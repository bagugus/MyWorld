#pragma once

#include "render/Models3D.h"
#include "sim/Id.h"

#include <raylib.h>

#include <cstdint>
#include <vector>

namespace lw {

class SceneShading;
class Terrain3D;
class World;

// Здания в 3D (этап 2а.2). На карте 16 × 16 км их тысячи, и собирать каждое
// каждый кадр заново (rlVertex3f) стало дороже всего остального кадра. Поэтому:
// - модель здания (участок, земля под ним, дверь, коробка до конька) считается
//   один раз — здания не меняются;
// - коробка — цоколь, стены, крыша со снегом, труба, парапет — лежит в
//   статичной сетке своего чанка kChunkMeters × kChunkMeters и рисуется одним
//   вызовом на чанк (и в карту теней тоже);
// - каждый кадр выводятся только окна, двери, крыльца и вывески ближних
//   зданий (emitBuildingDetails): они зависят от взгляда и ночного света.
// Снег на крышах меняется ступенями (kSnowSteps): тогда сетки чанков
// перестраиваются по нескольку за кадр. Здание, открытое кинокамере изнутри
// (12а.14), из сетки своего чанка убирается сразу — его рисуют комнатами.
//
// Только читает мир. Создавать после InitWindow, уничтожать до CloseWindow.
class Buildings3D {
public:
    static constexpr float kChunkMeters = 512.0f;

    struct Entry {
        EntityId id = kInvalidId;
        BuildingModel model;  // без облика кадра: снега, деталей, взгляда
        BoundingBox bounds{}; // от фундамента до конька
        int chunk = 0;
    };

    Buildings3D() = default;
    ~Buildings3D();
    Buildings3D(const Buildings3D&) = delete;
    Buildings3D& operator=(const Buildings3D&) = delete;

    // Модели всех зданий и раскладка по чанкам — один раз (здания не меняются).
    void build(const World& world, const Terrain3D& terrain);
    bool built() const { return built_; }
    const std::vector<Entry>& entries() const { return entries_; }
    // Коробки зданий в том же порядке, что entries(); адрес не меняется.
    const std::vector<BoundingBox>& bounds() const { return bounds_; }

    // Перед кадром: снег на крышах (0..1) и здание, открытое изнутри (kInvalidId — нет).
    void update(float snow, EntityId open);
    // Коробки в кадре — между SceneShading::beginModels и endModels, внутри
    // BeginMode3D. Возвращает, сколько зданий в нарисованных чанках.
    int drawShells(const SceneShading& shading);
    // Коробки в карту теней — чанки в её квадрате (внутри прохода теней).
    void drawShadows(const SceneShading& shading);

    // Здания чанков, которые задевает квадрат center ± radius (x, z мира).
    template <typename Visit>
    void forEachNear(Vector2 center, float radius, Visit&& visit) const {
        const int x0 = chunkIndex(center.x - radius, chunksX_);
        const int x1 = chunkIndex(center.x + radius, chunksX_);
        const int z0 = chunkIndex(center.y - radius, chunksZ_);
        const int z1 = chunkIndex(center.y + radius, chunksZ_);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                for (const int i : chunks_[static_cast<std::size_t>(z * chunksX_ + x)].entries) {
                    visit(entries_[static_cast<std::size_t>(i)]);
                }
            }
        }
    }

    // Для замеров: сколько сеток чанков построено с начала и за сколько секунд.
    int meshesBuilt() const { return meshesBuilt_; }
    double buildSeconds() const { return buildSeconds_; }
    int chunksDrawn() const { return chunksDrawn_; }

private:
    static constexpr int kSnowSteps = 24;
    static constexpr int kRebuildsPerFrame = 8;

    struct Chunk {
        Vector3 min{};
        Vector3 max{};
        std::vector<int> entries; // номера в entries_
        std::vector<Mesh> meshes;
        int snowStep = -1;           // с каким снегом построена сетка
        EntityId without = kInvalidId; // какого здания в сетке нет (открыто изнутри)
    };

    static int chunkIndex(float meters, int count) {
        const int i = static_cast<int>(meters / kChunkMeters);
        return i < 0 ? 0 : (i >= count ? count - 1 : i);
    }
    // Сетка чанка с нынешним снегом и без открытого здания.
    void rebuild(Chunk& chunk);
    void unload(Chunk& chunk);
    // Устарела ли сетка: снег другой ступени (можно подождать) или не то
    // открытое здание (нельзя: коробка закрыла бы комнаты).
    bool stale(const Chunk& chunk) const { return chunk.snowStep != snowStep_ || chunk.without != open_; }
    // Перестроить, если устарела и бюджет кадра позволяет (открытое — всегда).
    void refresh(Chunk& chunk);

    std::vector<Entry> entries_;
    std::vector<BoundingBox> bounds_;
    std::vector<Chunk> chunks_;
    int chunksX_ = 1;
    int chunksZ_ = 1;
    bool built_ = false;
    int snowStep_ = 0;
    EntityId open_ = kInvalidId;
    int rebuilds_ = 0; // перестроено в этом кадре
    Material material_{}; // шейдер моделей ставится при отрисовке
    Material caster_{};   // в карту теней: шейдер по умолчанию
    bool materials_ = false;
    ModelMesh scratch_;

    int meshesBuilt_ = 0;
    double buildSeconds_ = 0.0;
    int chunksDrawn_ = 0;
};

} // namespace lw
