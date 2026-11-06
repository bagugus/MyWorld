#pragma once

#include <raylib.h>

#include <cstdint>
#include <vector>

namespace lw {

class LandUse;
class Map;
class SceneShading;
class Terrain3D;

// Мелочь мира вне леса в 3D (этап 11.9) — только облик, симуляция о ней не
// знает. Две группы по дальности: мелкая (посевы рядами, трава и цветы, камыш
// и осока, огородные грядки, камешки, мусор после коллапса) — до kSmallRange;
// крупная (заборы и плетни с калитками, колодцы, скамейки, поленницы, стога и
// тюки, столбы с проводами, мостки, валуны) — до дальности прорисовки. У края
// дальности предметы растворяются по матрице Байера целиком (по точке опоры).
//
// Раскладка — детерминированно по клетке (хеш клетки) и по render/LandUse
// (дворы, столбы, мостки), поля — render/FieldLayout. Сетки строятся по чанкам
// kChunkCells × kChunkCells клеток по мере надобности, с бюджетом времени на
// кадр, и выгружаются, когда давно не нужны. Сетка неизменна: сезон, снег,
// ветер и запустение (мусор после коллапса) — в шейдере.
//
// Только читает карту. Создавать после InitWindow, уничтожать до CloseWindow.
class Props3D {
public:
    Props3D(const Map& map, const Terrain3D& terrain, const LandUse& land);
    ~Props3D();
    Props3D(const Props3D&) = delete;
    Props3D& operator=(const Props3D&) = delete;

    static constexpr float kSmallRange = 150.0f;

    // LandUse узнал о дворах и столбах: построить сетки заново.
    void reset();
    // Что нужно в этом кадре: мелкое — до smallRange, крупное — до largeRange.
    void update(const Camera3D& camera, float smallRange, float largeRange);

    // Облик кадра: доля года (с 1 марта), снег 0..1, ветер (куда дует, м/с),
    // time — секунды для качания, decay — запустение 0..1 (мусор после коллапса).
    struct Look {
        float year = 0.0f;
        float snow = 0.0f;
        Vector2 wind{};
        float time = 0.0f;
        float decay = 0.0f;
    };
    // Между BeginMode3D и EndMode3D, после земли.
    void draw(const SceneShading& shading, const Look& look);
    // Крупное — в карту теней (между beginShadowPass и endShadowPass).
    void drawShadows(const SceneShading& shading, const Look& look);

    int itemsDrawn() const { return itemsDrawn_; }
    int meshesLoaded() const { return meshesLoaded_; }
    int pending() const { return pending_; }

private:
    static constexpr int kChunkCells = 32;

    struct Part {
        std::vector<Mesh> meshes;
        bool built = false;
        int items = 0;
        std::uint32_t lastUsed = 0;
    };
    struct Chunk {
        int left = 0; // клетки
        int top = 0;
        int width = 0;
        int height = 0;
        Vector3 min{}; // метры
        Vector3 max{};
        Part small;
        Part large;
        bool drawSmall = false;
        bool drawLarge = false;
    };

    void buildSmall(Chunk& chunk);
    void buildLarge(Chunk& chunk);
    void unload(Part& part);
    void setUniforms(const Shader& shader, const Look& look) const;
    void drawPart(const Shader& shader, Material& material, const Part& part, float range, bool count);

    const Map& map_;
    const Terrain3D& terrain_;
    const LandUse& land_;
    std::vector<Chunk> chunks_;
    std::uint32_t frame_ = 0;
    float smallRange_ = 0.0f;
    float largeRange_ = 0.0f;
    float pixelAngle_ = 0.001f; // радиан на пиксель экрана: провода не тоньше пикселя

    Shader shader_{};
    Shader shadowShader_{};
    Material material_{};
    Material shadowMaterial_{};
    int rangeLoc_ = -1;
    int shadowRangeLoc_ = -1;

    int itemsDrawn_ = 0;
    int meshesLoaded_ = 0;
    int pending_ = 0;
    double cpuTime_ = 0.0;
};

} // namespace lw
