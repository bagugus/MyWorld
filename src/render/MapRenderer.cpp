#include "render/MapRenderer.h"

#include "render/TreeLayout.h"
#include "sim/Map.h"

#include <rlgl.h>

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
// Дно под водой в 3D (видно сквозь мелководье): у реки — галька с песком, у
// озера — ил, у моря — песок, глубже темнее.
constexpr Color kRiverBed = {122, 108, 82, 255};
constexpr Color kLakeBed = {98, 92, 72, 255};
constexpr Color kSeaSand = {176, 164, 128, 255};
constexpr Color kSeaDeep = {52, 66, 72, 255};

// Фильтр для 3D: билинейный с мип-уровнями и анизотропией — вдаль земля под
// углом не рябит и не мылится; чёткие клетки вблизи — в шейдере земли.
constexpr int kGroundFilter = 100;

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

// Цвет клетки; shade — с отмывкой рельефа (вид сверху), без неё — для 3D, где
// склоны лепит свет.
Color cellColor(const Map& map, int x, int y, bool shade) {
    // Дорога поверх всего, включая воду (мост).
    switch (map.road.at(x, y)) {
    case Road::Asphalt:
        return kAsphalt;
    case Road::Dirt:
        return kDirtRoad;
    case Road::None:
        break;
    }

    if (!shade) {
        switch (map.water.at(x, y)) {
        case Water::Sea:
            return mix(kSeaSand, kSeaDeep, std::clamp(-map.elevationMeters({x, y}) / 6.0f, 0.0f, 1.0f));
        case Water::Lake:
            return kLakeBed;
        case Water::River:
            return kRiverBed;
        case Water::None:
            break;
        }
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
    return shade ? scale(color, hillshade(map, x, y)) : color;
}

// Насколько клетку видно белой под снегом: вода не заносится, лес и дороги — меньше.
unsigned char snowAlpha(const Map& map, int x, int y) {
    if (map.road.at(x, y) != Road::None) {
        return 150;
    }
    if (map.water.at(x, y) != Water::None) {
        return 0;
    }
    if (map.cover.at(x, y) == Cover::Forest) {
        return 120;
    }
    return 255;
}

Texture2D makeTexture(const std::vector<Color>& pixels, int width, int height) {
    // Image не владеет пикселями, поэтому const_cast безопасен: LoadTextureFromImage только читает.
    const Image image = {const_cast<Color*>(pixels.data()), width, height, 1,
                         PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D texture = LoadTextureFromImage(image);
    // Мип-уровни нужны на общем плане, когда на пиксель экрана
    // приходится много клеток: без них карта рябит.
    GenTextureMipmaps(&texture);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    return texture;
}

} // namespace

MapRenderer::MapRenderer(const Map& map) : chunksX_((map.width() + kChunkCells - 1) / kChunkCells) {
    std::vector<Color> pixels;
    std::vector<Color> plain;
    std::vector<Color> snow;
    std::vector<Color> forest;
    for (int top = 0; top < map.height(); top += kChunkCells) {
        for (int left = 0; left < map.width(); left += kChunkCells) {
            const int width = std::min(kChunkCells, map.width() - left);
            const int height = std::min(kChunkCells, map.height() - top);
            pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
            plain.resize(pixels.size());
            snow.resize(pixels.size());
            forest.resize(pixels.size());
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                              static_cast<std::size_t>(x);
                    pixels[index] = cellColor(map, left + x, top + y, true);
                    plain[index] = cellColor(map, left + x, top + y, false);
                    // Снег тоже с отмывкой: склоны остаются читаемыми.
                    snow[index] = scale({236, 240, 246, snowAlpha(map, left + x, top + y)},
                                        std::min(1.0f, hillshade(map, left + x, top + y) * 1.05f));
                    // Маски для 3D: R — доля лиственных в лесу, G — поле, B — луг
                    // (трава и цветы по сезону), A — дорога (канавы вдоль неё).
                    const int cx = left + x;
                    const int cy = top + y;
                    const float deciduous = deciduousShareOf(map, {cx, cy});
                    const bool open = map.water.at(cx, cy) == Water::None && map.road.at(cx, cy) == Road::None &&
                                      map.cover.at(cx, cy) == Cover::None;
                    const bool field = open && map.ground.at(cx, cy) == Ground::Field;
                    const bool meadow = open && map.ground.at(cx, cy) == Ground::Grass;
                    const bool road = map.road.at(cx, cy) != Road::None && map.water.at(cx, cy) == Water::None;
                    forest[index] = {static_cast<unsigned char>(std::lround(deciduous * 255.0f)),
                                     static_cast<unsigned char>(field ? 255 : 0),
                                     static_cast<unsigned char>(meadow ? 255 : 0),
                                     static_cast<unsigned char>(road ? 255 : 0)};
                }
            }

            Chunk chunk;
            chunk.texture = makeTexture(pixels, width, height);
            chunk.plain = makeTexture(plain, width, height);
            chunk.snow = makeTexture(snow, width, height);
            // Маска — плавная: на земле не видно квадратов клеток.
            chunk.forest = makeTexture(forest, width, height);
            SetTextureFilter(chunk.forest, TEXTURE_FILTER_TRILINEAR);
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
        UnloadTexture(chunk.plain);
        UnloadTexture(chunk.snow);
        UnloadTexture(chunk.forest);
    }
}

void MapRenderer::setFilter(int filter) {
    if (filter == filter_) {
        return;
    }
    for (const Chunk& chunk : chunks_) {
        for (const Texture2D& texture : {chunk.texture, chunk.plain, chunk.snow}) {
            if (filter != kGroundFilter) {
                SetTextureFilter(texture, filter);
                continue;
            }
            // Увеличение — билинейное: чёткость клеткам даёт шейдер земли
            // (Terrain3D, crisp), а край клетки сглажен на пиксель и не рябит.
            rlTextureParameters(texture.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
            rlTextureParameters(texture.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_MIP_LINEAR);
            rlTextureParameters(texture.id, RL_TEXTURE_FILTER_ANISOTROPIC, 16);
        }
    }
    filter_ = filter;
}

void MapRenderer::draw(const Rectangle& visible, float zoom) {
    // Вблизи клетки рисуются чёткими квадратами, издали — сглаживаются.
    setFilter(zoom * Map::kCellSize >= 1.0f ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_TRILINEAR);

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

void MapRenderer::drawSnow(const Rectangle& visible, float amount) {
    if (amount <= 0.0f) {
        return;
    }
    const Color tint = {255, 255, 255, static_cast<unsigned char>(std::clamp(amount, 0.0f, 1.0f) * 255.0f)};
    for (const Chunk& chunk : chunks_) {
        if (!CheckCollisionRecs(chunk.bounds, visible)) {
            continue;
        }
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(chunk.snow.width),
                                  static_cast<float>(chunk.snow.height)};
        DrawTexturePro(chunk.snow, source, chunk.bounds, {0.0f, 0.0f}, 0.0f, tint);
    }
}

MapRenderer::GroundChunk MapRenderer::groundChunk(int x, int y) const {
    const Chunk& chunk = chunks_[static_cast<std::size_t>((y / kChunkCells) * chunksX_ + x / kChunkCells)];
    GroundChunk result;
    result.texture = chunk.plain;
    result.snow = chunk.snow;
    result.forest = chunk.forest;
    result.left = x / kChunkCells * kChunkCells;
    result.top = y / kChunkCells * kChunkCells;
    result.width = chunk.texture.width;
    result.height = chunk.texture.height;
    return result;
}

std::size_t MapRenderer::textureBytes() const {
    const auto bytes = [](const Texture2D& texture) -> std::size_t {
        if (texture.id == 0) {
            return 0;
        }
        const auto base = static_cast<std::size_t>(GetPixelDataSize(texture.width, texture.height, texture.format));
        // Мип-уровни добавляют треть.
        return texture.mipmaps > 1 ? base + base / 3 : base;
    };
    std::size_t total = 0;
    for (const Chunk& chunk : chunks_) {
        total += bytes(chunk.texture) + bytes(chunk.plain) + bytes(chunk.snow) + bytes(chunk.forest);
    }
    return total;
}

void MapRenderer::useGroundFilter() {
    setFilter(kGroundFilter);
}

} // namespace lw
