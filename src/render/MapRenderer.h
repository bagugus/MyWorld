#pragma once

#include <raylib.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lw {

class Map;

// Рисует слои карты. Карта неизменна, поэтому запекается в текстуры (пиксель
// на клетку), разбитые на чанки; каждый кадр рисуются только чанки, попавшие в
// видимую область.
//
// С этапа 2а.2 (карта 16 × 16 км) текстуры двух уровней. Грубые (тексель на
// kCoarse × kCoarse клеток) есть у всех чанков: пекутся на старте параллельно
// и годятся там, где клетка на экране не крупнее пикселя. Подробные (тексель на
// клетку) — только у чанков рядом с камерой: их просят вид сверху (draw) и
// рельеф в 3D (requestFine), пекутся по одному за кадр, а давно не нужные
// выгружаются, когда их больше kMaxFine. Пока подробных нет, рисуются грубые.
// Координаты текстуры — в долях чанка, поэтому уровни взаимозаменяемы.
//
// Создавать после InitWindow, уничтожать до CloseWindow.
class MapRenderer {
public:
    // Клеток в грубом текселе по каждой оси.
    static constexpr int kCoarse = 2;
    // Подробных чанков в видеопамяти не больше этого.
    static constexpr int kMaxFine = 16;

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
    // Подробные, если они готовы, иначе грубые.
    struct GroundChunk {
        Texture2D texture{}; // без отмывки: в 3D склоны лепит свет
        Texture2D snow{};    // в 3D нужна только альфа: сколько клетки заносит
        // Маски: R — доля лиственных в клетке леса (render/TreeLayout), G — поле,
        // B — луг, A — дорога (этап 11.9).
        Texture2D forest{};
        int left = 0;
        int top = 0;
        int width = 0;  // клетки
        int height = 0; // клетки
        bool fine = false;
    };
    // Чанк с клеткой (x, y); клетка должна лежать на карте.
    GroundChunk groundChunk(int x, int y) const;
    // Рельеф в 3D: чанку с клеткой (x, y) нужны подробные текстуры; priority —
    // чем меньше, тем нужнее (уровень детализации плитки). Просьбы кадра
    // исполняет commitFine: самые нужные kMaxFine чанков.
    void requestFine(int x, int y, float priority);
    // Исполнить просьбы кадра: испечь не больше одного подробного чанка,
    // запомнить, какие нужны, лишние давно не нужные — выгрузить.
    void commitFine();
    // Фильтр текстур для 3D: вблизи клетки чёткие, вдаль — мип-уровни и анизотропия.
    void useGroundFilter();

    int chunkCount() const { return static_cast<int>(chunks_.size()); }
    int chunksDrawn() const { return chunksDrawn_; }
    // Сколько чанков с подробными текстурами сейчас в видеопамяти.
    int fineChunks() const;
    // Сколько нужных подробных чанков ещё не испечено (снимок их ждёт).
    int pending() const { return pending_; }
    // Сколько подробных чанков испечено с начала и за сколько секунд.
    int fineBaked() const { return fineBaked_; }
    double fineSeconds() const { return fineSeconds_; }
    // Память текстур карты в видеокарте, байты (оценка с мип-уровнями; этап 2а.1).
    std::size_t textureBytes() const;

private:
    // Текстуры одного уровня. Вид сверху берёт shaded и snow, 3D — plain, snow и masks.
    struct Set {
        Texture2D shaded{}; // с отмывкой рельефа
        Texture2D plain{};  // без отмывки рельефа — для 3D
        Texture2D snow{};   // белая маска: прозрачна на воде, полупрозрачна на лесе и дорогах
        Texture2D masks{};  // лиственный лес, поле, луг, дорога — для 3D
        bool ready() const { return shaded.id != 0; }
    };
    struct Chunk {
        int left = 0; // клетки
        int top = 0;
        int width = 0;
        int height = 0;
        Rectangle bounds{}; // метры
        Set coarse;
        Set fine;
        std::uint32_t wanted = 0; // кадр, в который подробные нужны были в последний раз
    };
    struct Request {
        int chunk = 0;
        float priority = 0.0f;
    };

    // Испечь текстуры чанка с factor × factor клетками в текселе.
    void bake(const Chunk& chunk, int factor, Set& set);
    void unload(Set& set);
    // Фильтр текстур: значение TextureFilter или kGroundFilter.
    void setFilter(int filter);
    void applyFilter(const Set& set, int filter) const;
    int chunkAt(int x, int y) const;

    const Map& map_;
    std::vector<Chunk> chunks_;
    int chunksX_ = 0; // чанков в ряду
    int filter_ = -1;
    int chunksDrawn_ = 0;
    std::uint32_t frame_ = 0;
    std::vector<Request> requests_;
    int pending_ = 0;
    int fineBaked_ = 0;
    double fineSeconds_ = 0.0;
};

} // namespace lw
