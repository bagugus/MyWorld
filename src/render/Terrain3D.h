#pragma once

#include "sim/Map.h"

#include <raylib.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace lw {

class MapRenderer;

// Рельеф и вода в 3D (этап 11.2). Высоты берутся из слоёв карты: сетка высот —
// по углам клеток (среднее соседних клеток; у воды — дно), поверх неё — вода на
// своём уровне (море — 0 м, озёра и реки — дно + глубина). Берег — там, где
// поверхность воды пересекает рельеф.
//
// Земля разбита на плитки kTileCells × kTileCells клеток; у каждой плитки пять
// уровней детализации (шаг 1, 2, 4, 8, 16 клеток), уровень выбирается по тому,
// сколько пикселей экрана занимает клетка: вдали — грубее. Щели между плитками
// разных уровней закрывают «юбки» — полосы вниз по краю плитки. Грубые уровни
// строятся сразу, подробные — по мере надобности и выгружаются, когда давно не
// нужны. Текстура — та же карта, что и в виде сверху (отмывка рельефа в ней).
//
// Только читает карту. Создавать после InitWindow, уничтожать до CloseWindow.
class Terrain3D {
public:
    Terrain3D(const Map& map, MapRenderer& textures);
    ~Terrain3D();
    Terrain3D(const Terrain3D&) = delete;
    Terrain3D& operator=(const Terrain3D&) = delete;

    // Мост — клетка дороги над водой: настил над уровнем воды.
    struct Bridge {
        Cell cell;
        float deck = 0.0f; // высота настила, метры
        Road road = Road::Dirt;
    };

    // Высота твёрдой земли (под водой — дна) в точке мира (x, y), метры; так же,
    // как её рисует самый подробный уровень. Точки за краем — по краю карты.
    float groundAt(float x, float y) const;
    // Высота поверхности: земли или воды над ней.
    float surfaceAt(float x, float y) const;
    // Где стоит существо: на земле, на мелководье — вброд по дну, на глубине —
    // плывёт с головой над водой, на мосту — на настиле.
    float standAt(float x, float y) const;
    // Точка поверхности, в которую упирается луч; нет — луч уходит в небо или за карту.
    std::optional<Vector2> raycast(const Ray& ray) const;

    const std::vector<Bridge>& bridges() const { return bridges_; }

    // Земля (и снег поверх, amount 0..1). Между BeginMode3D и EndMode3D, первым.
    void drawGround(const Camera3D& camera, float snowAmount);
    // Вода — после всего непрозрачного: она полупрозрачна. time — секунды для
    // ряби, sky — цвет неба для отражения.
    void drawWater(const Camera3D& camera, float time, Color sky);

    // Отладочные слои по рельефу: между ними всё, что рисуется батчем rlgl,
    // поднимается на поверхность (вершинный шейдер берёт высоту из текстуры высот).
    void beginDrape() const;
    void endDrape() const;

    int tilesDrawn() const { return tilesDrawn_; }
    int trianglesDrawn() const { return trianglesDrawn_; }
    int meshesLoaded() const { return meshesLoaded_; }

private:
    static constexpr int kTileCells = 128;
    static constexpr int kLevels = 5; // шаг уровня — 1 << level клеток

    struct Tile {
        int left = 0; // клетки
        int top = 0;
        int width = 0;
        int height = 0;
        float minY = 0.0f; // границы по высоте, метры
        float maxY = 0.0f;
        bool hasWater = false;
        std::array<Mesh, kLevels> ground{};
        std::array<Mesh, kLevels> water{};
        std::array<bool, kLevels> built{};
        std::array<std::uint32_t, kLevels> lastUsed{};
    };
    struct Visible {
        int tile = 0;
        int level = 0;
    };

    std::size_t vertexIndex(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(map_.width() + 1) +
               static_cast<std::size_t>(x);
    }
    // Значение сетки по углам клеток в точке мира: как на треугольниках сетки.
    float sample(const std::vector<float>& grid, float x, float y) const;
    // Уровень детализации, при котором клетка плитки на экране не крупнее kMaxQuadPixels.
    int levelFor(const Tile& tile, const Camera3D& camera) const;
    void build(Tile& tile, int level);
    void buildGround(Tile& tile, int level);
    void buildWater(Tile& tile, int level);
    void unload(Tile& tile, int level);

    const Map& map_;
    MapRenderer& textures_;
    // Сетки по углам клеток, (ширина + 1) × (высота + 1).
    std::vector<float> ground_;
    std::vector<float> water_;   // уровень воды; kNoWater — рядом нет воды
    std::vector<float> surface_; // max(земля, вода)
    float minSurface_ = 0.0f;
    float maxSurface_ = 0.0f;
    std::vector<Bridge> bridges_;

    std::vector<Tile> tiles_;
    int tilesX_ = 0;
    std::vector<Visible> visible_;
    std::uint32_t frame_ = 0;

    Material groundMaterial_{};
    Material waterMaterial_{};
    Shader waterShader_{};
    int viewPosLoc_ = -1;
    int timeLoc_ = -1;
    int skyLoc_ = -1;
    Shader drapeShader_{};
    int heightsLoc_ = -1;
    Texture2D heights_{}; // surface_ текстурой R32F для drapeShader_

    int tilesDrawn_ = 0;
    int trianglesDrawn_ = 0;
    int meshesLoaded_ = 0;
};

} // namespace lw
