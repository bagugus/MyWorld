#pragma once

#include "sim/Map.h"

#include <raylib.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lw {

class LandUse;
class MapRenderer;
class SceneShading;

// Коробка целиком вне пирамиды видимости viewProjection: все углы за одной из её плоскостей.
bool outsideFrustum(const Matrix& viewProjection, Vector3 min, Vector3 max);

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
// нужны. Текстура — та же карта, что и в виде сверху, но без отмывки: склоны
// лепит свет по нормалям (этап 11.4, render/Lighting3D).
//
// С этапа 11.9 русла рек врезаны (вода ниже берегов, откосы, мокрый берег и
// дно сквозь мелководье), вода течёт по направлению реки (render/LandUse:
// flowAt), на перекатах пенится, ветер гонит рябь; на земле — поля по сезону
// (render/FieldLayout: борозды, всходы, колосья, стерня, межи), трава и цветы
// на лугах, канавы вдоль дорог.
//
// С этапа 2а.2 (карта 16 × 16 км) высоты по углам клеток не хранятся на всю
// карту (три float на угол — 200 МБ): они считаются блоками на плитку по
// требованию и держатся в кэше не больше kMaxHeightBlocks плиток. Высота угла —
// функция соседних клеток, поэтому блок плитки считается сам по себе, и
// результат тот же, что у сетки на всю карту. Отладочным слоям высоты дают
// окно у камеры и грубая сетка на всю карту (beginDrape).
//
// Только читает карту. Создавать после InitWindow, уничтожать до CloseWindow.
class Terrain3D {
public:
    Terrain3D(const Map& map, MapRenderer& textures, const LandUse& land);
    ~Terrain3D();
    Terrain3D(const Terrain3D&) = delete;
    Terrain3D& operator=(const Terrain3D&) = delete;

    // Мост — клетка дороги над водой: настил над уровнем воды.
    struct Bridge {
        Cell cell;
        float deck = 0.0f; // высота настила, метры: общая у всего пролёта
        Road road = Road::Dirt;
        int span = 0;      // индекс в spans()
    };
    // Пролёт — связные (и по диагонали) клетки моста. Прямой пролёт — одна плита
    // вдоль дороги от берега до берега (a, b — середины кусков дороги на берегах,
    // мир x, z), иначе — настил по клеткам.
    struct Span {
        Vector2 a{};
        Vector2 b{};
        float deck = 0.0f;
        float halfWidth = 2.0f; // полуширина плиты прямого пролёта, м
        Road road = Road::Dirt;
        bool straight = false;
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
    // Карта, по которой построен рельеф (раскладка деревьев для кинокамеры).
    const Map& map() const { return map_; }

    const std::vector<Bridge>& bridges() const { return bridges_; }
    const std::vector<Span>& spans() const { return spans_; }

    // Земля со снегом (amount 0..1), светом, тенью и дымкой. Между BeginMode3D
    // и EndMode3D, первым. Лиственный лес на земле — по средней листве
    // (TreeLayout: averageFoliageAt): весной светлеет, осенью желтеет, зимой сереет.
    // year — доля года (с 1 марта): поля и луга по сезону.
    void drawGround(const Camera3D& camera, float snowAmount, Vector3 foliage, float year,
                    const SceneShading& shading);
    // Вода — после всего непрозрачного: она полупрозрачна. time — секунды для
    // ряби и течения, wind — ветер (куда дует, м/с).
    void drawWater(float time, Vector2 wind, const SceneShading& shading);
    // Рельеф в карту теней: плитки квадрата center ± radius тех же уровней, что в кадре.
    void drawShadowCasters(const Camera3D& camera, Vector2 center, float radius);

    // Отладочные слои по рельефу: между ними всё, что рисуется батчем rlgl,
    // поднимается на поверхность (вершинный шейдер берёт высоту из текстуры высот:
    // подробной — в окне вокруг center, мир x, z; грубой — дальше).
    void beginDrape(Vector2 center) const;
    void endDrape() const;

    int tilesDrawn() const { return tilesDrawn_; }
    int trianglesDrawn() const { return trianglesDrawn_; }
    int meshesLoaded() const { return meshesLoaded_; }
    // Сколько плиток в кадре ещё ждут нужного уровня (снимок их ждёт).
    int pending() const { return pending_; }
    // Сколько сеток рельефа построено с начала и сколько секунд на это ушло (этап 2а.1).
    int meshesBuilt() const { return meshesBuilt_; }
    double buildSeconds() const { return buildSeconds_; }
    // Блоки высот в кэше и сколько раз блок считался (этап 2а.2).
    int heightBlocks() const { return heightsLoaded_; }
    int heightComputes() const { return heightComputes_; }

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
        int level = -1; // уровень, выбранный в прошлом кадре (для гистерезиса)
    };
    struct Visible {
        int tile = 0;
        int level = 0;
    };
    // Высоты по углам клеток одной плитки с полосой в угол вокруг: углы
    // [left − 1, left + width + 1] × [top − 1, top + height + 1]; за краем карты —
    // как у ближайшего угла на карте.
    struct Heights {
        int x0 = 0; // угол карты, который лежит в блоке первым
        int z0 = 0;
        int width = 0; // углов
        int height = 0;
        std::vector<float> ground; // земля (под водой — дно)
        std::vector<float> water;  // уровень воды; kNoWater — рядом нет воды
        std::uint32_t used = 0;    // когда нужен в последний раз (для вытеснения)
        std::size_t index(int x, int z) const {
            return static_cast<std::size_t>(z - z0) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(x - x0);
        }
        float groundAt(int x, int z) const { return ground[index(x, z)]; }
        float waterAt(int x, int z) const { return water[index(x, z)]; }
    };
    static constexpr int kMaxHeightBlocks = 256;

    // Посчитать высоты плитки (без кэша; можно из нескольких потоков).
    void computeHeights(const Tile& tile, Heights& out) const;
    // Блок плитки из кэша; нет — посчитать (давно не нужный блок вытесняется).
    const Heights& heightsOf(std::size_t tile) const;
    void adopt(std::size_t tile, std::unique_ptr<Heights> block) const;
    std::size_t tileOfCell(int x, int z) const;
    std::size_t tileIndex(const Tile& tile) const {
        return static_cast<std::size_t>((tile.top / kTileCells) * tilesX_ + tile.left / kTileCells);
    }
    // Высота земли или поверхности в точке мира: как на треугольниках сетки.
    float sample(float x, float y, bool surface) const;
    // Окно подробных высот для отладочных слоёв — вокруг точки (клетки).
    void updateDrapeWindow(Vector2 center) const;
    // Уровень детализации, при котором клетка плитки на экране не крупнее kMaxQuadPixels.
    int levelFor(const Tile& tile, const Camera3D& camera) const;
    // Уровень, который рисуется: нужный или, пока его нет, ближайший грубее.
    int shownLevel(const Tile& tile, const Camera3D& camera) const;
    void build(Tile& tile, int level);
    void buildGround(Tile& tile, int level);
    void buildWater(Tile& tile, int level);
    void unload(Tile& tile, int level);

    const Map& map_;
    MapRenderer& textures_;
    const LandUse& land_;
    // Блоки высот по плиткам (nullptr — не в кэше).
    mutable std::vector<std::unique_ptr<Heights>> heights_;
    mutable std::uint32_t heightsClock_ = 0;
    mutable int heightsLoaded_ = 0;
    mutable int heightComputes_ = 0;
    float minSurface_ = 0.0f;
    float maxSurface_ = 0.0f;
    std::vector<Bridge> bridges_;
    std::vector<Span> spans_;
    std::unordered_map<std::int64_t, std::size_t> bridgeAt_; // клетка → индекс в bridges_

    void buildSpans();

    std::vector<Tile> tiles_;
    int tilesX_ = 0;
    std::vector<Visible> visible_;
    std::uint32_t frame_ = 0;

    Shader groundShader_{};
    int snowLoc_ = -1;
    int foliageLoc_ = -1;
    int yearLoc_ = -1;
    Material groundMaterial_{};
    Material casterMaterial_{}; // рельеф в карте теней: шейдер по умолчанию
    Material waterMaterial_{};
    Shader waterShader_{};
    int timeLoc_ = -1;
    int skyLoc_ = -1;
    int windLoc_ = -1;
    Shader drapeShader_{};
    int heightsLoc_ = -1;
    int coarseLoc_ = -1;
    int windowOriginLoc_ = -1;
    int windowSizeLoc_ = -1;
    // Высоты поверхности текстурами R32F для drapeShader_: грубая на всю карту
    // (каждый kDrapeStep-й угол) и подробное окно у камеры.
    Texture2D drapeCoarse_{};
    mutable Texture2D drapeWindow_{};
    mutable int drapeX_ = -1; // угол окна, клетки
    mutable int drapeZ_ = -1;
    mutable std::vector<float> drapeScratch_;

    int tilesDrawn_ = 0;
    int pending_ = 0;
    int trianglesDrawn_ = 0;
    int meshesLoaded_ = 0;
    int meshesBuilt_ = 0;
    double buildSeconds_ = 0.0;
};

} // namespace lw
