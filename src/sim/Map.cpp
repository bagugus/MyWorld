#include "sim/Map.h"

#include "sim/Hash.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace lw {

namespace {

template <typename T>
void addLayer(Hasher& hasher, const Grid<T>& layer) {
    for (const T value : layer.cells()) {
        if constexpr (std::is_enum_v<T>) {
            hasher.add(static_cast<std::underlying_type_t<T>>(value));
        } else {
            hasher.add(value);
        }
    }
}

} // namespace

Map::Map(int width, int height)
    : elevation(width, height),
      ground(width, height),
      water(width, height),
      depth(width, height),
      cover(width, height),
      road(width, height),
      width_(width),
      height_(height) {}

Cell Map::cellAt(float x, float y) {
    return {static_cast<int>(std::floor(x / kCellSize)),
            static_cast<int>(std::floor(y / kCellSize))};
}

Cell Map::clampedCellAt(float x, float y) const {
    const Cell cell = cellAt(x, y);
    return {std::clamp(cell.x, 0, width_ - 1), std::clamp(cell.y, 0, height_ - 1)};
}

std::uint64_t Map::hash() const {
    Hasher hasher;
    hasher.add(width_);
    hasher.add(height_);
    addLayer(hasher, elevation);
    addLayer(hasher, ground);
    addLayer(hasher, water);
    addLayer(hasher, depth);
    addLayer(hasher, cover);
    addLayer(hasher, road);
    return hasher.value();
}

} // namespace lw
