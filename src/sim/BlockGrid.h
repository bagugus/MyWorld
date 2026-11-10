#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace lw {

// Разреженный слой (этап 2а.2): то же, что Grid, но клетки хранятся блоками
// kBlock × kBlock, и блок заводится только при первой записи значения,
// отличного от «пустого» fill. Для слоёв, где почти везде одно значение (реки,
// дворы, обочины): на карте 16 × 16 км плотный слой — 16 МБ на байт клетки.
template <typename T>
class BlockGrid {
public:
    static constexpr int kBlockShift = 5;
    static constexpr int kBlock = 1 << kBlockShift;

    BlockGrid() = default;
    BlockGrid(int width, int height, T fill = T{})
        : width_(width),
          height_(height),
          blocksX_((width + kBlock - 1) >> kBlockShift),
          fill_(fill),
          blocks_(static_cast<std::size_t>(blocksX_) * static_cast<std::size_t>((height + kBlock - 1) >> kBlockShift)) {}

    int width() const { return width_; }
    int height() const { return height_; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    T fill() const { return fill_; }

    // Значение клетки; без блока — fill.
    T get(int x, int y) const {
        assert(contains(x, y));
        const std::unique_ptr<T[]>& block = blocks_[blockIndex(x, y)];
        return block ? block[inner(x, y)] : fill_;
    }
    // Клетка для записи: блок заводится, если его ещё нет.
    T& at(int x, int y) {
        assert(contains(x, y));
        std::unique_ptr<T[]>& block = blocks_[blockIndex(x, y)];
        if (!block) {
            block = std::make_unique<T[]>(static_cast<std::size_t>(kBlock * kBlock));
            std::fill_n(block.get(), kBlock * kBlock, fill_);
        }
        return block[inner(x, y)];
    }
    // Записать; «пустое» значение в клетку без блока блока не заводит.
    void set(int x, int y, T value) {
        if (value == fill_ && !blocks_[blockIndex(x, y)]) {
            return;
        }
        at(x, y) = value;
    }

    // Память слоя, байты (для замеров).
    std::size_t memoryBytes() const {
        std::size_t used = 0;
        for (const std::unique_ptr<T[]>& block : blocks_) {
            used += block ? static_cast<std::size_t>(kBlock * kBlock) * sizeof(T) : 0;
        }
        return used + blocks_.capacity() * sizeof(std::unique_ptr<T[]>);
    }

private:
    std::size_t blockIndex(int x, int y) const {
        return static_cast<std::size_t>(y >> kBlockShift) * static_cast<std::size_t>(blocksX_) +
               static_cast<std::size_t>(x >> kBlockShift);
    }
    static std::size_t inner(int x, int y) {
        return static_cast<std::size_t>(y & (kBlock - 1)) * static_cast<std::size_t>(kBlock) +
               static_cast<std::size_t>(x & (kBlock - 1));
    }

    int width_ = 0;
    int height_ = 0;
    int blocksX_ = 0;
    T fill_{};
    std::vector<std::unique_ptr<T[]>> blocks_;
};

} // namespace lw
