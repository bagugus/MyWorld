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

    int chunkCount() const { return static_cast<int>(chunks_.size()); }
    int chunksDrawn() const { return chunksDrawn_; }

private:
    struct Chunk {
        Texture2D texture{};
        Texture2D snow{}; // белая маска: прозрачна на воде, полупрозрачна на лесе и дорогах
        Rectangle bounds{}; // метры
    };

    std::vector<Chunk> chunks_;
    int filter_ = -1;
    int chunksDrawn_ = 0;
};

} // namespace lw
