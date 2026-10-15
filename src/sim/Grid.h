#pragma once

#include <cassert>
#include <cstddef>
#include <vector>

namespace lw {

// Прямоугольный слой карты: одно значение на клетку, хранение по строкам
// (индекс = y * width + x).
template <typename T>
class Grid {
public:
    Grid() = default;
    Grid(int width, int height, T fill = T{})
        : width_(width),
          height_(height),
          cells_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill) {}

    int width() const { return width_; }
    int height() const { return height_; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }

    T& at(int x, int y) { return cells_[index(x, y)]; }
    const T& at(int x, int y) const { return cells_[index(x, y)]; }

    // Все клетки подряд — для обхода, хеша и сохранения.
    std::vector<T>& cells() { return cells_; }
    const std::vector<T>& cells() const { return cells_; }

private:
    std::size_t index(int x, int y) const {
        assert(contains(x, y));
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(x);
    }

    int width_ = 0;
    int height_ = 0;
    std::vector<T> cells_;
};

} // namespace lw
