#pragma once

#include "sim/Grid.h"

#include <cstdint>

namespace lw {

// Значения перечислений слоёв попадают в хеш мира и в сохранения:
// новые добавлять только в конец.

// Поверхность земли (под водой — дно). Field — пашня у деревень.
enum class Ground : std::uint8_t { Grass, Sand, Rock, Field };
enum class Water : std::uint8_t { None, Sea, Lake, River };
// Растительность: влияет на обзор, скорость движения, укрытие.
enum class Cover : std::uint8_t { None, Forest };
// Дорога на клетке с водой — мост. Значения упорядочены по качеству покрытия.
enum class Road : std::uint8_t { None, Dirt, Asphalt };

// Координаты клетки. Клетка (x, y) занимает в мире квадрат
// [x * kCellSize, (x + 1) * kCellSize) по обеим осям; ось y направлена вниз (на юг).
struct Cell {
    int x = 0;
    int y = 0;

    friend bool operator==(const Cell&, const Cell&) = default;
};

// Статичная карта мира: набор слоёв одного размера, по одному значению на клетку.
// После генерации не меняется. Здания, предметы и всё изменяемое — сущности.
// Производные данные (проходимость, стоимость движения) здесь не хранятся:
// их пересчитывают из слоёв и сущностей.
class Map {
public:
    // Сторона клетки в метрах. Степень двойки: деление на неё точное.
    static constexpr float kCellSize = 4.0f;
    // Метров в одной единице слоя elevation.
    static constexpr float kElevationStep = 0.1f;
    // Метров в одной единице слоя depth.
    static constexpr float kDepthStep = 0.1f;

    Map(int width, int height);

    // Размер в клетках.
    int width() const { return width_; }
    int height() const { return height_; }
    // Размер в метрах.
    float worldWidth() const { return static_cast<float>(width_) * kCellSize; }
    float worldHeight() const { return static_cast<float>(height_) * kCellSize; }

    bool contains(Cell cell) const {
        return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_;
    }
    // Клетка под точкой мира; может лежать вне карты.
    static Cell cellAt(float x, float y);
    // То же, но точки за краем относятся к ближайшей клетке карты.
    Cell clampedCellAt(float x, float y) const;
    // Координата центра клетки с номером index по одной оси.
    static float cellCenter(int index) { return (static_cast<float>(index) + 0.5f) * kCellSize; }

    float elevationMeters(Cell cell) const {
        return static_cast<float>(elevation.at(cell.x, cell.y)) * kElevationStep;
    }
    float depthMeters(Cell cell) const {
        return static_cast<float>(depth.at(cell.x, cell.y)) * kDepthStep;
    }

    // Хеш всех слоёв. Считается долго (вся карта), результат стоит запомнить.
    std::uint64_t hash() const;

    // Высота твёрдой поверхности над уровнем моря в единицах kElevationStep;
    // под водой — высота дна, у моря она отрицательная.
    Grid<std::int16_t> elevation;
    Grid<Ground> ground;
    Grid<Water> water;
    // Глубина воды в единицах kDepthStep; на суше — ноль. У озёр и рек
    // поверхность воды — elevation + depth. Насыщается на 25,5 м: глубину моря
    // больше этой брать из elevation (поверхность моря — ноль).
    Grid<std::uint8_t> depth;
    Grid<Cover> cover;
    Grid<Road> road;

private:
    int width_;
    int height_;
};

} // namespace lw
