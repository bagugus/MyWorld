#pragma once

#include <raylib.h>

#include <vector>

namespace lw {

class Map;

// Рисует слои карты. Карта неизменна, поэтому один раз запекается в текстуры
// (пиксель на клетку), разбитые на чанки; каждый кадр рисуются только чанки,
// попавшие в видимую область.
// Создавать после InitWindow, уничтожать до CloseWindow.
class MapRenderer {
public:
    explicit MapRenderer(const Map& map);
    ~MapRenderer();
    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;

    // visible — видимая часть мира в метрах, zoom — пикселей на метр.
    // Вызывать между BeginMode2D и EndMode2D.
    void draw(const Rectangle& visible, float zoom);
    // Снег на суше поверх карты; amount — 0..1. Вызывать после draw.
    void drawSnow(const Rectangle& visible, float amount);

    // Текстуры чанка и его место на карте в клетках — для рельефа в 3D (Terrain3D).
    struct GroundChunk {
        Texture2D texture{}; // без отмывки: в 3D склоны лепит свет
        Texture2D snow{};    // в 3D нужна только альфа: сколько клетки заносит
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
    };
    // Чанк с клеткой (x, y); клетка должна лежать на карте.
    GroundChunk groundChunk(int x, int y) const;
    // Фильтр текстур для 3D: вблизи клетки чёткие, вдаль — мип-уровни и анизотропия.
    void useGroundFilter();

    int chunkCount() const { return static_cast<int>(chunks_.size()); }
    int chunksDrawn() const { return chunksDrawn_; }

private:
    struct Chunk {
        Texture2D texture{};
        Texture2D plain{}; // без отмывки рельефа — для 3D
        Texture2D snow{}; // белая маска: прозрачна на воде, полупрозрачна на лесе и дорогах
        Rectangle bounds{}; // метры
    };

    // Фильтр текстур: значение TextureFilter или kGroundFilter.
    void setFilter(int filter);

    std::vector<Chunk> chunks_;
    int chunksX_ = 0; // чанков в ряду
    int filter_ = -1;
    int chunksDrawn_ = 0;
};

} // namespace lw
