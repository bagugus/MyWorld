#include "render/MapRenderer.h"

#include "render/TreeLayout.h"
#include "sim/Map.h"
#include "sim/Parallel.h"

#include <rlgl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

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
// Шоссе темнее и холоднее трассы: на общем плане главные дороги читаются сразу.
constexpr Color kHighway = {56, 58, 70, 255};
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
// Вид сверху просит подробные текстуры, когда клетка на экране не мельче этого (пиксели).
constexpr float kFineCellPixels = 1.0f;

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
Color cellColor(const Map& map, int x, int y, bool shade, float relief) {
    // Дорога поверх всего, включая воду (мост).
    switch (map.road.at(x, y)) {
    case Road::Highway:
        return kHighway;
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
    return shade ? scale(color, relief) : color;
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

// Четыре текселя клетки: с отмывкой, без неё, снег и маски для 3D.
struct CellTexels {
    Color shaded{};
    Color plain{};
    Color snow{};
    Color masks{};
};

CellTexels texelsOf(const Map& map, int x, int y) {
    CellTexels t;
    const float relief = hillshade(map, x, y);
    t.shaded = cellColor(map, x, y, true, relief);
    t.plain = cellColor(map, x, y, false, relief);
    // Снег тоже с отмывкой: склоны остаются читаемыми.
    t.snow = scale({236, 240, 246, snowAlpha(map, x, y)}, std::min(1.0f, relief * 1.05f));
    // Маски для 3D: R — доля лиственных в лесу, G — поле, B — луг (трава и цветы
    // по сезону), A — дорога (канавы вдоль неё).
    const float deciduous = map.cover.at(x, y) == Cover::Forest ? deciduousShareOf(map, {x, y}) : 0.0f;
    const bool open = map.water.at(x, y) == Water::None && map.road.at(x, y) == Road::None &&
                      map.cover.at(x, y) == Cover::None;
    const bool field = open && map.ground.at(x, y) == Ground::Field;
    const bool meadow = open && map.ground.at(x, y) == Ground::Grass;
    const bool road = map.road.at(x, y) != Road::None && map.water.at(x, y) == Water::None;
    t.masks = {static_cast<unsigned char>(std::lround(deciduous * 255.0f)),
               static_cast<unsigned char>(field ? 255 : 0), static_cast<unsigned char>(meadow ? 255 : 0),
               static_cast<unsigned char>(road ? 255 : 0)};
    return t;
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

MapRenderer::MapRenderer(const Map& map) : map_(map), chunksX_((map.width() + kChunkCells - 1) / kChunkCells) {
    for (int top = 0; top < map.height(); top += kChunkCells) {
        for (int left = 0; left < map.width(); left += kChunkCells) {
            Chunk chunk;
            chunk.left = left;
            chunk.top = top;
            chunk.width = std::min(kChunkCells, map.width() - left);
            chunk.height = std::min(kChunkCells, map.height() - top);
            chunk.bounds = {static_cast<float>(left) * Map::kCellSize, static_cast<float>(top) * Map::kCellSize,
                            static_cast<float>(chunk.width) * Map::kCellSize,
                            static_cast<float>(chunk.height) * Map::kCellSize};
            chunks_.push_back(chunk);
        }
    }
    for (Chunk& chunk : chunks_) {
        bake(chunk, kCoarse, chunk.coarse);
    }
}

MapRenderer::~MapRenderer() {
    for (Chunk& chunk : chunks_) {
        unload(chunk.coarse);
        unload(chunk.fine);
    }
}

void MapRenderer::bake(const Chunk& chunk, int factor, Set& set) {
    const int width = (chunk.width + factor - 1) / factor;
    const int height = (chunk.height + factor - 1) / factor;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<Color> shaded(count);
    std::vector<Color> plain(count);
    std::vector<Color> snow(count);
    std::vector<Color> masks(count);
    // Тексель — среднее своих клеток; строки текселей независимы.
    parallelFor(static_cast<std::size_t>(height), 8, 16, [&](std::size_t begin, std::size_t end, int) {
        for (std::size_t ty = begin; ty < end; ++ty) {
            for (int tx = 0; tx < width; ++tx) {
                int sum[4][4] = {};
                int cells = 0;
                for (int dy = 0; dy < factor; ++dy) {
                    const int y = chunk.top + static_cast<int>(ty) * factor + dy;
                    if (y >= chunk.top + chunk.height) {
                        break;
                    }
                    for (int dx = 0; dx < factor; ++dx) {
                        const int x = chunk.left + tx * factor + dx;
                        if (x >= chunk.left + chunk.width) {
                            break;
                        }
                        const CellTexels t = texelsOf(map_, x, y);
                        const Color colors[4] = {t.shaded, t.plain, t.snow, t.masks};
                        for (int k = 0; k < 4; ++k) {
                            sum[k][0] += colors[k].r;
                            sum[k][1] += colors[k].g;
                            sum[k][2] += colors[k].b;
                            sum[k][3] += colors[k].a;
                        }
                        ++cells;
                    }
                }
                const std::size_t i = ty * static_cast<std::size_t>(width) + static_cast<std::size_t>(tx);
                Color* out[4] = {&shaded[i], &plain[i], &snow[i], &masks[i]};
                for (int k = 0; k < 4; ++k) {
                    const auto avg = [&](int c) {
                        return static_cast<unsigned char>((sum[k][c] + cells / 2) / std::max(cells, 1));
                    };
                    *out[k] = {avg(0), avg(1), avg(2), avg(3)};
                }
            }
        }
    });
    set.shaded = makeTexture(shaded, width, height);
    set.plain = makeTexture(plain, width, height);
    set.snow = makeTexture(snow, width, height);
    // Маска — плавная: на земле не видно квадратов клеток.
    set.masks = makeTexture(masks, width, height);
    SetTextureFilter(set.masks, TEXTURE_FILTER_TRILINEAR);
    if (filter_ >= 0) {
        applyFilter(set, filter_);
    }
}

void MapRenderer::unload(Set& set) {
    for (Texture2D* texture : {&set.shaded, &set.plain, &set.snow, &set.masks}) {
        if (texture->id != 0) {
            UnloadTexture(*texture);
        }
        *texture = {};
    }
}

void MapRenderer::applyFilter(const Set& set, int filter) const {
    for (const Texture2D& texture : {set.shaded, set.plain, set.snow}) {
        if (texture.id == 0) {
            continue;
        }
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

void MapRenderer::setFilter(int filter) {
    if (filter == filter_) {
        return;
    }
    for (const Chunk& chunk : chunks_) {
        applyFilter(chunk.coarse, filter);
        applyFilter(chunk.fine, filter);
    }
    filter_ = filter;
}

int MapRenderer::chunkAt(int x, int y) const {
    return (y / kChunkCells) * chunksX_ + x / kChunkCells;
}

void MapRenderer::requestFine(int x, int y, float priority) {
    requests_.push_back({chunkAt(x, y), priority});
}

void MapRenderer::commitFine() {
    ++frame_;
    // У чанка — самая высокая из его просьб; самые нужные kMaxFine — нужны.
    std::unordered_map<int, float> best;
    for (const Request& request : requests_) {
        const auto [it, added] = best.emplace(request.chunk, request.priority);
        if (!added) {
            it->second = std::min(it->second, request.priority);
        }
    }
    requests_.clear();
    std::vector<Request> wanted;
    wanted.reserve(best.size());
    for (const auto& [chunk, priority] : best) {
        wanted.push_back({chunk, priority});
    }
    std::sort(wanted.begin(), wanted.end(), [](const Request& a, const Request& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.chunk < b.chunk;
    });
    if (wanted.size() > static_cast<std::size_t>(kMaxFine)) {
        wanted.resize(kMaxFine);
    }

    // Испечь самый нужный из неготовых — один за кадр.
    pending_ = 0;
    bool baked = false;
    for (const Request& request : wanted) {
        Chunk& chunk = chunks_[static_cast<std::size_t>(request.chunk)];
        chunk.wanted = frame_;
        if (chunk.fine.ready()) {
            continue;
        }
        if (!baked) {
            const auto start = std::chrono::steady_clock::now();
            bake(chunk, 1, chunk.fine);
            fineSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            ++fineBaked_;
            baked = true;
        } else {
            ++pending_;
        }
    }

    // Лишние — те, что давно не нужны, — из видеопамяти.
    int resident = fineChunks();
    while (resident > kMaxFine) {
        Chunk* oldest = nullptr;
        for (Chunk& chunk : chunks_) {
            if (chunk.fine.ready() && chunk.wanted != frame_ && (oldest == nullptr || chunk.wanted < oldest->wanted)) {
                oldest = &chunk;
            }
        }
        if (oldest == nullptr) {
            break;
        }
        unload(oldest->fine);
        --resident;
    }
}

int MapRenderer::fineChunks() const {
    int count = 0;
    for (const Chunk& chunk : chunks_) {
        count += chunk.fine.ready() ? 1 : 0;
    }
    return count;
}

void MapRenderer::draw(const Rectangle& visible, float zoom) {
    // Вблизи клетки рисуются чёткими квадратами, издали — сглаживаются.
    setFilter(zoom * Map::kCellSize >= 1.0f ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_TRILINEAR);

    // Клетка на экране крупнее пикселя — нужны подробные текстуры; ближе к
    // середине экрана — нужнее.
    const bool fine = zoom * Map::kCellSize >= kFineCellPixels;
    const Vector2 middle = {visible.x + visible.width * 0.5f, visible.y + visible.height * 0.5f};
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const Chunk& chunk = chunks_[i];
        if (fine && CheckCollisionRecs(chunk.bounds, visible)) {
            const float dx = chunk.bounds.x + chunk.bounds.width * 0.5f - middle.x;
            const float dy = chunk.bounds.y + chunk.bounds.height * 0.5f - middle.y;
            requestFine(chunk.left, chunk.top, std::sqrt(dx * dx + dy * dy));
        }
    }
    commitFine();

    chunksDrawn_ = 0;
    for (const Chunk& chunk : chunks_) {
        if (!CheckCollisionRecs(chunk.bounds, visible)) {
            continue;
        }
        const Texture2D& texture = chunk.fine.ready() ? chunk.fine.shaded : chunk.coarse.shaded;
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)};
        DrawTexturePro(texture, source, chunk.bounds, {0.0f, 0.0f}, 0.0f, WHITE);
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
        const Texture2D& snow = chunk.fine.ready() ? chunk.fine.snow : chunk.coarse.snow;
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(snow.width), static_cast<float>(snow.height)};
        DrawTexturePro(snow, source, chunk.bounds, {0.0f, 0.0f}, 0.0f, tint);
    }
}

MapRenderer::GroundChunk MapRenderer::groundChunk(int x, int y) const {
    const Chunk& chunk = chunks_[static_cast<std::size_t>(chunkAt(x, y))];
    const Set& set = chunk.fine.ready() ? chunk.fine : chunk.coarse;
    GroundChunk result;
    result.texture = set.plain;
    result.snow = set.snow;
    result.forest = set.masks;
    result.left = chunk.left;
    result.top = chunk.top;
    result.width = chunk.width;
    result.height = chunk.height;
    result.fine = chunk.fine.ready();
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
        for (const Set* set : {&chunk.coarse, &chunk.fine}) {
            total += bytes(set->shaded) + bytes(set->plain) + bytes(set->snow) + bytes(set->masks);
        }
    }
    return total;
}

void MapRenderer::useGroundFilter() {
    setFilter(kGroundFilter);
}

} // namespace lw
