#include "render/MapRenderer.h"

#include "sim/Map.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Сторона чанка в клетках. Текстура такого размера есть на любой видеокарте.
constexpr int kChunkCells = 512;

constexpr Color kGrass = {96, 132, 72, 255};
constexpr Color kSand = {208, 194, 146, 255};
constexpr Color kRock = {132, 128, 122, 255};
constexpr Color kField = {176, 164, 96, 255};
constexpr Color kForest = {46, 86, 50, 255};
constexpr Color kDirtRoad = {150, 122, 88, 255};
constexpr Color kAsphalt = {72, 72, 78, 255};
constexpr Color kShallowSea = {72, 144, 178, 255};
constexpr Color kDeepSea = {24, 62, 112, 255};
constexpr Color kLake = {58, 122, 170, 255};
constexpr Color kRiver = {78, 146, 192, 255};

constexpr float kDeepSeaMeters = 45.0f;   // глубже цвет моря не меняется
constexpr float kReliefExaggeration = 3.0f; // во сколько раз подчёркнуты склоны в отмывке
constexpr float kMinShade = 0.6f;
constexpr float kMaxShade = 1.3f;

Color scale(Color color, float factor) {
    const auto channel = [factor](unsigned char value) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(value) * factor, 0.0f, 255.0f));
    };
    return {channel(color.r), channel(color.g), channel(color.b), color.a};
}

Color mix(Color a, Color b, float t) {
    const auto channel = [t](unsigned char from, unsigned char to) {
        return static_cast<unsigned char>(static_cast<float>(from) +
                                          (static_cast<float>(to) - static_cast<float>(from)) * t);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255};
}

// Отмывка рельефа: склоны, обращённые к северо-западу, светлее.
float hillshade(const Map& map, int x, int y) {
    const int x0 = std::max(x - 1, 0);
    const int x1 = std::min(x + 1, map.width() - 1);
    const int y0 = std::max(y - 1, 0);
    const int y1 = std::min(y + 1, map.height() - 1);
    const float scaleZ = kReliefExaggeration * Map::kElevationStep / (2.0f * Map::kCellSize);
    const float dzdx = static_cast<float>(map.elevation.at(x1, y) - map.elevation.at(x0, y)) * scaleZ;
    const float dzdy = static_cast<float>(map.elevation.at(x, y1) - map.elevation.at(x, y0)) * scaleZ;

    // Нормаль (-dzdx, -dzdy, 1), свет из точки (-1, -1, 1.5).
    constexpr float lightXY = 0.4851f;
    constexpr float lightZ = 0.7276f;
    const float lambert = (lightXY * (dzdx + dzdy) + lightZ) / std::sqrt(dzdx * dzdx + dzdy * dzdy + 1.0f);
    return std::clamp(lambert / lightZ, kMinShade, kMaxShade);
}

Color cellColor(const Map& map, int x, int y) {
    // Дорога поверх всего, включая воду (мост).
    switch (map.road.at(x, y)) {
    case Road::Asphalt:
        return kAsphalt;
    case Road::Dirt:
        return kDirtRoad;
    case Road::None:
        break;
    }

    switch (map.water.at(x, y)) {
    case Water::Sea: {
        const float depth = -map.elevationMeters({x, y});
        return mix(kShallowSea, kDeepSea, std::sqrt(std::clamp(depth / kDeepSeaMeters, 0.0f, 1.0f)));
    }
    case Water::Lake:
        return kLake;
    case Water::River:
        return kRiver;
    case Water::None:
        break;
    }

    Color color = kGrass;
    if (map.cover.at(x, y) == Cover::Forest) {
        color = kForest;
    } else {
        switch (map.ground.at(x, y)) {
        case Ground::Sand:
            color = kSand;
            break;
        case Ground::Rock:
            color = kRock;
            break;
        case Ground::Field:
            color = kField;
            break;
        case Ground::Grass:
            break;
        }
    }
    return scale(color, hillshade(map, x, y));
}

} // namespace

MapRenderer::MapRenderer(const Map& map) {
    std::vector<Color> pixels;
    for (int top = 0; top < map.height(); top += kChunkCells) {
        for (int left = 0; left < map.width(); left += kChunkCells) {
            const int width = std::min(kChunkCells, map.width() - left);
            const int height = std::min(kChunkCells, map.height() - top);
            pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)] = cellColor(map, left + x, top + y);
                }
            }

            const Image image = {pixels.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            Chunk chunk;
            chunk.texture = LoadTextureFromImage(image);
            // Мип-уровни нужны на общем плане, когда на пиксель экрана
            // приходится много клеток: без них карта рябит.
            GenTextureMipmaps(&chunk.texture);
            SetTextureWrap(chunk.texture, TEXTURE_WRAP_CLAMP);
            chunk.bounds = {static_cast<float>(left) * Map::kCellSize,
                            static_cast<float>(top) * Map::kCellSize,
                            static_cast<float>(width) * Map::kCellSize,
                            static_cast<float>(height) * Map::kCellSize};
            chunks_.push_back(chunk);
        }
    }
}

MapRenderer::~MapRenderer() {
    for (const Chunk& chunk : chunks_) {
        UnloadTexture(chunk.texture);
    }
}

void MapRenderer::draw(const Rectangle& visible, float zoom) {
    // Вблизи клетки рисуются чёткими квадратами, издали — сглаживаются.
    const int filter = zoom * Map::kCellSize >= 1.0f ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_TRILINEAR;
    if (filter != filter_) {
        for (const Chunk& chunk : chunks_) {
            SetTextureFilter(chunk.texture, filter);
        }
        filter_ = filter;
    }

    chunksDrawn_ = 0;
    for (const Chunk& chunk : chunks_) {
        if (!CheckCollisionRecs(chunk.bounds, visible)) {
            continue;
        }
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(chunk.texture.width),
                                  static_cast<float>(chunk.texture.height)};
        DrawTexturePro(chunk.texture, source, chunk.bounds, {0.0f, 0.0f}, 0.0f, WHITE);
        ++chunksDrawn_;
    }
}

} // namespace lw
