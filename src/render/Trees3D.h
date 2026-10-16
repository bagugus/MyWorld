#pragma once

#include <raylib.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lw {

class Map;
class SceneShading;
class Terrain3D;

// Деревья в 3D (этап 11.6): леса и рощи из клеток карты (раскладка —
// render/TreeLayout). Две детализации: вблизи — модели из ствола, ветвей и
// ярусов или клубов кроны, дальше — упрощённые (пирамида или ромб на стволе),
// ещё дальше — только текстура леса на земле. Детализации сменяются
// растворением по матрице Байера по расстоянию до корня дерева, так же деревья
// тают у края дальности.
//
// Сетки строятся по чанкам kChunkCells × kChunkCells клеток (подробные — блоками
// kBlockCells × kBlockCells) по мере надобности, с бюджетом времени на кадр, и
// выгружаются, когда давно не нужны. Сетка неизменна: время года, снег и ветер
// — в шейдере. Листва распускается весной, желтеет и облетает осенью у каждого
// дерева в свой срок, голая крона — редкие ветки; на хвое и ветвях лежит снег;
// деревья качаются по ветру. Нормаль — по производным позиции во фрагментном
// шейдере (грани плоские, вершины общие — сетки вдвое-втрое меньше).
//
// Только читает карту. Создавать после InitWindow, уничтожать до CloseWindow.
class Trees3D {
public:
    Trees3D(const Map& map, const Terrain3D& terrain);
    ~Trees3D();
    Trees3D(const Trees3D&) = delete;
    Trees3D& operator=(const Trees3D&) = delete;

    // Что нужно в этом кадре: подробные — до nearRange от камеры, упрощённые —
    // до farRange. Строит недостающее (ближнее — первым), выгружает ненужное.
    void update(const Camera3D& camera, float nearRange, float farRange);

    // Облик кадра: доля года (CalendarTime::yearFraction), снег 0..1, ветер —
    // куда дует, м/с, time — секунды для качания.
    struct Look {
        float year = 0.0f;
        float snow = 0.0f;
        Vector2 wind{};
        float time = 0.0f;
    };
    // Деревья в кадре: между BeginMode3D и EndMode3D, после земли.
    void draw(const SceneShading& shading, const Look& look);
    // Деревья в карту теней — между SceneShading::beginShadowPass и endShadowPass;
    // детализация та же, что в кадре (по расстоянию от камеры кадра).
    void drawShadows(const SceneShading& shading, const Look& look);

    int treesDrawn() const { return treesDrawn_; }
    int meshesDrawn() const { return meshesDrawn_; }
    int meshesLoaded() const { return meshesLoaded_; }
    // Сколько нужных сеток ещё не построено.
    int pending() const { return pending_; }

private:
    static constexpr int kChunkCells = 32;
    static constexpr int kBlockCells = 16;
    static constexpr int kBlocks = (kChunkCells / kBlockCells) * (kChunkCells / kBlockCells);

    struct Part {
        Mesh mesh{};
        bool built = false;
        int trees = 0;
        std::uint32_t lastUsed = 0;
    };
    struct Chunk {
        int left = 0; // клетки
        int top = 0;
        int width = 0;
        int height = 0;
        bool forest = false;
        Vector3 min{}; // границы с деревьями, метры
        Vector3 max{};
        Part far;
        std::array<Part, kBlocks> near{};
        // Что рисовать в этом кадре.
        bool drawFar = false;
        bool drawNear = false;
        bool farOnly = false; // подробные ещё не готовы: все деревья — упрощённые
    };

    void buildFar(Chunk& chunk);
    void buildNear(Chunk& chunk, int block);
    void unload(Part& part);
    void setUniforms(const Shader& shader, const Look& look) const;
    // Отрисовка выбранного в update; inside — нужен ли чанк этому проходу.
    template <typename Inside>
    void drawChunks(const Shader& shader, Material& material, Inside inside, bool count);

    const Map& map_;
    const Terrain3D& terrain_;
    std::vector<Chunk> chunks_;
    std::uint32_t frame_ = 0;
    float nearRange_ = 0.0f;
    float farRange_ = 0.0f;

    Shader shader_{};
    Shader shadowShader_{};
    Material material_{};
    Material shadowMaterial_{};
    int lodLoc_ = -1;
    int shadowLodLoc_ = -1;
    int nearLoc_ = -1;
    int shadowNearLoc_ = -1;

    int treesDrawn_ = 0;
    int meshesDrawn_ = 0;
    int meshesLoaded_ = 0;
    int pending_ = 0;
    double cpuTime_ = 0.0; // секунды на постройку сеток в этом кадре
};

} // namespace lw
