#include "render/Terrain3D.h"

#include "render/MapRenderer.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lw {

namespace {

constexpr float kCell = Map::kCellSize;
constexpr float kNoWater = -1.0e9f;

// Поверхность воды чуть ниже расчётной: берега и долины, засыпанные ровно до
// уровня озера, остаются сухими, а не мерцают вровень с водой.
constexpr float kWaterInset = 0.3f;
// Угол на границе воды и суши — на столько ниже поверхности воды: берег
// проходит по краю водных клеток, а не заходит в сухие.
constexpr float kShoreDepth = 0.1f;
// Глубже этого существо уже не идёт по дну, а плывёт.
constexpr float kSwimDepth = 1.3f;
// Настил моста над уровнем воды.
constexpr float kBridgeDeck = 0.6f;
// Отладочные слои — на метр над поверхностью.
constexpr float kDrapeLift = 1.0f;

// Клетка на экране не крупнее стольких пикселей: так выбирается уровень детализации.
constexpr float kMaxQuadPixels = 8.0f;
// Уровни от этого и грубее строятся сразу и не выгружаются.
constexpr int kFirstEagerLevel = 2;
// Подробных уровней за кадр строится не больше: при быстром полёте пока
// рисуется грубее, зато кадр не замирает.
constexpr int kBuildsPerFrame = 4;
// Подробный уровень, не нужный столько кадров, выгружается.
constexpr std::uint32_t kEvictFrames = 600;

// Юбка плитки — вниз на столько, чтобы закрыть расхождение с соседом грубее.
float skirtDepth(int step) {
    return 2.0f + 0.5f * static_cast<float>(step) * kCell;
}

// Уровень воды в клетке с водой, метры: у моря — ноль, у озёр и рек — дно + глубина.
float waterLevel(const Map& map, Cell cell) {
    if (map.water.at(cell.x, cell.y) == Water::Sea) {
        return 0.0f;
    }
    return map.elevationMeters(cell) + map.depthMeters(cell);
}

// Вода: рябь — сумма бегущих волн, цвет — от глубины, под углом отражает небо,
// у берега прозрачна. Солнце пока постоянное, с северо-запада, как отмывка карты
// (свет по времени суток — этап 11.4).
constexpr const char* kWaterVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec3 fragPosition;
out float fragDepth;
void main() {
    fragPosition = vertexPosition;
    fragDepth = vertexTexCoord.x;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kWaterFs = R"(#version 330
in vec3 fragPosition;
in float fragDepth;
uniform vec3 viewPos;
uniform float time;
uniform vec3 skyColor;
uniform vec4 colDiffuse;
out vec4 finalColor;

const vec3 shallowColor = vec3(0.20, 0.44, 0.48);
const vec3 deepColor = vec3(0.04, 0.15, 0.28);
const vec3 sunDir = vec3(-0.45, 0.77, -0.45);

vec2 wave(vec2 p, vec2 dir, float frequency, float speed, float amplitude) {
    return dir * (amplitude * cos(dot(p, dir) * frequency + time * speed));
}

void main() {
    vec3 toEye = viewPos - fragPosition;
    float distance = length(toEye);
    vec3 v = toEye / max(distance, 1e-3);
    vec2 p = fragPosition.xz;
    vec2 slope = wave(p, vec2(0.80, 0.60), 0.90, 1.7, 0.050)
               + wave(p, vec2(-0.53, 0.85), 1.70, 2.4, 0.030)
               + wave(p, vec2(0.20, -0.98), 3.10, 3.3, 0.020)
               + wave(p, vec2(-0.92, -0.39), 0.35, 1.0, 0.040);
    // Вдали рябь мельче пикселя и только мерцала бы.
    slope /= 1.0 + distance / 120.0;
    vec3 n = normalize(vec3(-slope.x, 1.0, -slope.y));
    float fresnel = 0.03 + 0.97 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    float depth = max(fragDepth, 0.0);
    float deep = clamp(depth / 10.0, 0.0, 1.0);
    vec3 color = mix(mix(shallowColor, deepColor, sqrt(deep)), skyColor, fresnel);
    color += vec3(0.7 * pow(max(dot(n, normalize(sunDir + v)), 0.0), 250.0));
    float alpha = smoothstep(0.0, 0.5, depth) * mix(0.5, 0.9, deep);
    alpha = mix(alpha, 1.0, fresnel * smoothstep(0.0, 0.3, depth));
    finalColor = vec4(color, alpha) * colDiffuse;
}
)";

// Отладочные слои по рельефу: высота вершины — из текстуры высот поверхности
// (по углам клеток), как у треугольников рельефа — билинейно.
constexpr const char* kDrapeVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform sampler2D heights;
uniform vec2 gridSize;
uniform float cellSize;
uniform float lift;
out vec2 fragTexCoord;
out vec4 fragColor;

float heightAt(ivec2 p) {
    return texelFetch(heights, clamp(p, ivec2(0), ivec2(gridSize) - 1), 0).r;
}

void main() {
    vec2 g = clamp(vertexPosition.xz / cellSize, vec2(0.0), gridSize - 1.0);
    vec2 base = min(floor(g), gridSize - 2.0);
    ivec2 i = ivec2(base);
    vec2 f = g - base;
    float h = mix(mix(heightAt(i), heightAt(i + ivec2(1, 0)), f.x),
                  mix(heightAt(i + ivec2(0, 1)), heightAt(i + ivec2(1, 1)), f.x), f.y);
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition.x, h + lift, vertexPosition.z, 1.0);
}
)";

constexpr const char* kDrapeFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main() {
    finalColor = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
}
)";

// Коробка целиком вне пирамиды видимости: все углы за одной из её плоскостей.
bool outsideFrustum(const Matrix& m, Vector3 min, Vector3 max) {
    float clip[8][4];
    for (int i = 0; i < 8; ++i) {
        const float x = (i & 1) != 0 ? max.x : min.x;
        const float y = (i & 2) != 0 ? max.y : min.y;
        const float z = (i & 4) != 0 ? max.z : min.z;
        clip[i][0] = m.m0 * x + m.m4 * y + m.m8 * z + m.m12;
        clip[i][1] = m.m1 * x + m.m5 * y + m.m9 * z + m.m13;
        clip[i][2] = m.m2 * x + m.m6 * y + m.m10 * z + m.m14;
        clip[i][3] = m.m3 * x + m.m7 * y + m.m11 * z + m.m15;
    }
    for (int axis = 0; axis < 3; ++axis) {
        for (const float sign : {-1.0f, 1.0f}) {
            bool all = true;
            for (int i = 0; i < 8 && all; ++i) {
                // Внутри: −w ≤ c ≤ w.
                all = sign * clip[i][axis] > clip[i][3];
            }
            if (all) {
                return true;
            }
        }
    }
    return false;
}

Mesh allocateMesh(int vertexCount, int triangleCount) {
    Mesh mesh{};
    mesh.vertexCount = vertexCount;
    mesh.triangleCount = triangleCount;
    mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertexCount) * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertexCount) * 2 * sizeof(float)));
    mesh.indices = static_cast<unsigned short*>(
        MemAlloc(static_cast<unsigned>(triangleCount) * 3 * sizeof(unsigned short)));
    return mesh;
}

// Загрузить в видеопамять. Вершины на процессоре больше не нужны; индексы
// остаются: по ним DrawMesh понимает, что сетка индексная.
void upload(Mesh& mesh) {
    UploadMesh(&mesh, false);
    MemFree(mesh.vertices);
    MemFree(mesh.texcoords);
    mesh.vertices = nullptr;
    mesh.texcoords = nullptr;
}

} // namespace

Terrain3D::Terrain3D(const Map& map, MapRenderer& textures) : map_(map), textures_(textures) {
    const int width = map.width();
    const int height = map.height();
    const std::size_t count = static_cast<std::size_t>(width + 1) * static_cast<std::size_t>(height + 1);
    ground_.resize(count);
    water_.assign(count, kNoWater);
    surface_.resize(count);
    minSurface_ = std::numeric_limits<float>::max();
    maxSurface_ = std::numeric_limits<float>::lowest();
    for (int vy = 0; vy <= height; ++vy) {
        for (int vx = 0; vx <= width; ++vx) {
            // Угол делят до четырёх клеток. Среди воды высота — по дну. На границе
            // воды и суши — чуть ниже поверхности воды: по среднему соседей узкая
            // река оказалась бы выше своих берегов, а по дну берег проседал бы
            // внутрь сухой клетки, и вода растекалась бы по нему. Даже если суша
            // ниже реки (русло поперёк склона), угол не опускается: вдоль русла
            // получается невысокий вал, и вода остаётся в своих клетках.
            float land = 0.0f;
            int lands = 0;
            float bed = 0.0f;
            float level = 0.0f;
            int wet = 0;
            for (int cy = vy - 1; cy <= vy; ++cy) {
                for (int cx = vx - 1; cx <= vx; ++cx) {
                    const Cell cell{cx, cy};
                    if (!map.contains(cell)) {
                        continue;
                    }
                    const float elevation = map.elevationMeters(cell);
                    if (map.water.at(cx, cy) != Water::None) {
                        bed += elevation;
                        level += waterLevel(map, cell);
                        ++wet;
                    }
                    land += elevation;
                    ++lands;
                }
            }
            const std::size_t i = vertexIndex(vx, vy);
            if (wet == 0) {
                ground_[i] = land / static_cast<float>(lands);
            } else {
                water_[i] = level / static_cast<float>(wet) - kWaterInset;
                ground_[i] = bed / static_cast<float>(wet);
                if (wet < lands) {
                    ground_[i] = std::max(ground_[i], water_[i] - kShoreDepth);
                }
            }
            surface_[i] = std::max(ground_[i], water_[i]);
            minSurface_ = std::min(minSurface_, surface_[i]);
            maxSurface_ = std::max(maxSurface_, surface_[i]);
        }
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (map.road.at(x, y) != Road::None && map.water.at(x, y) != Water::None) {
                bridges_.push_back({{x, y}, waterLevel(map, {x, y}) + kBridgeDeck, map.road.at(x, y)});
            }
        }
    }

    tilesX_ = (width + kTileCells - 1) / kTileCells;
    const int tilesY = (height + kTileCells - 1) / kTileCells;
    tiles_.resize(static_cast<std::size_t>(tilesX_) * static_cast<std::size_t>(tilesY));
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            Tile& tile = tiles_[static_cast<std::size_t>(ty * tilesX_ + tx)];
            tile.left = tx * kTileCells;
            tile.top = ty * kTileCells;
            tile.width = std::min(kTileCells, width - tile.left);
            tile.height = std::min(kTileCells, height - tile.top);
            tile.minY = std::numeric_limits<float>::max();
            tile.maxY = std::numeric_limits<float>::lowest();
            for (int vy = tile.top; vy <= tile.top + tile.height; ++vy) {
                for (int vx = tile.left; vx <= tile.left + tile.width; ++vx) {
                    const std::size_t i = vertexIndex(vx, vy);
                    tile.minY = std::min(tile.minY, ground_[i]);
                    tile.maxY = std::max(tile.maxY, surface_[i]);
                    tile.hasWater = tile.hasWater || water_[i] != kNoWater;
                }
            }
            for (int level = kFirstEagerLevel; level < kLevels; ++level) {
                build(tile, level);
            }
        }
    }

    groundMaterial_ = LoadMaterialDefault();

    waterShader_ = LoadShaderFromMemory(kWaterVs, kWaterFs);
    viewPosLoc_ = GetShaderLocation(waterShader_, "viewPos");
    timeLoc_ = GetShaderLocation(waterShader_, "time");
    skyLoc_ = GetShaderLocation(waterShader_, "skyColor");
    waterMaterial_ = LoadMaterialDefault();
    waterMaterial_.shader = waterShader_;

    // Высоты поверхности — текстурой из одного канала float, без фильтрации:
    // шейдер читает углы клеток сам.
    heights_.id = rlLoadTexture(surface_.data(), width + 1, height + 1, RL_PIXELFORMAT_UNCOMPRESSED_R32, 1);
    heights_.width = width + 1;
    heights_.height = height + 1;
    heights_.mipmaps = 1;
    heights_.format = PIXELFORMAT_UNCOMPRESSED_R32;
    SetTextureFilter(heights_, TEXTURE_FILTER_POINT);
    SetTextureWrap(heights_, TEXTURE_WRAP_CLAMP);

    drapeShader_ = LoadShaderFromMemory(kDrapeVs, kDrapeFs);
    heightsLoc_ = GetShaderLocation(drapeShader_, "heights");
    const Vector2 gridSize = {static_cast<float>(width + 1), static_cast<float>(height + 1)};
    SetShaderValue(drapeShader_, GetShaderLocation(drapeShader_, "gridSize"), &gridSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(drapeShader_, GetShaderLocation(drapeShader_, "cellSize"), &kCell, SHADER_UNIFORM_FLOAT);
    SetShaderValue(drapeShader_, GetShaderLocation(drapeShader_, "lift"), &kDrapeLift, SHADER_UNIFORM_FLOAT);
}

Terrain3D::~Terrain3D() {
    for (Tile& tile : tiles_) {
        for (int level = 0; level < kLevels; ++level) {
            unload(tile, level);
        }
    }
    UnloadShader(waterShader_);
    UnloadShader(drapeShader_);
    UnloadTexture(heights_);
    // UnloadMaterial выгрузил бы и текстуры карты, а они не наши.
    MemFree(groundMaterial_.maps);
    MemFree(waterMaterial_.maps);
}

float Terrain3D::sample(const std::vector<float>& grid, float x, float y) const {
    const float width = static_cast<float>(map_.width());
    const float height = static_cast<float>(map_.height());
    const float gx = std::clamp(x / kCell, 0.0f, width);
    const float gy = std::clamp(y / kCell, 0.0f, height);
    const int ix = std::min(static_cast<int>(gx), map_.width() - 1);
    const int iy = std::min(static_cast<int>(gy), map_.height() - 1);
    const float u = gx - static_cast<float>(ix);
    const float w = gy - static_cast<float>(iy);
    const float h00 = grid[vertexIndex(ix, iy)];
    const float h10 = grid[vertexIndex(ix + 1, iy)];
    const float h01 = grid[vertexIndex(ix, iy + 1)];
    const float h11 = grid[vertexIndex(ix + 1, iy + 1)];
    // Клетка делится на треугольники по диагонали от (0, 0) к (1, 1), как в сетке.
    if (w >= u) {
        return h00 + (h11 - h01) * u + (h01 - h00) * w;
    }
    return h00 + (h10 - h00) * u + (h11 - h10) * w;
}

float Terrain3D::groundAt(float x, float y) const {
    return sample(ground_, x, y);
}

float Terrain3D::surfaceAt(float x, float y) const {
    return sample(surface_, x, y);
}

float Terrain3D::standAt(float x, float y) const {
    const Cell cell = map_.clampedCellAt(x, y);
    if (map_.road.at(cell.x, cell.y) != Road::None && map_.water.at(cell.x, cell.y) != Water::None) {
        return waterLevel(map_, cell) + kBridgeDeck;
    }
    return std::max(groundAt(x, y), surfaceAt(x, y) - kSwimDepth);
}

std::optional<Vector2> Terrain3D::raycast(const Ray& ray) const {
    const Vector3 origin = ray.position;
    const Vector3 direction = Vector3Normalize(ray.direction);
    const auto point = [&](float t) { return Vector3Add(origin, Vector3Scale(direction, t)); };

    // Отрезок луча внутри коробки: карта по горизонтали, от низшей до высшей
    // точки поверхности по высоте.
    float t0 = 0.0f;
    float t1 = std::numeric_limits<float>::max();
    const float lows[3] = {0.0f, minSurface_, 0.0f};
    const float highs[3] = {map_.worldWidth(), maxSurface_, map_.worldHeight()};
    const float from[3] = {origin.x, origin.y, origin.z};
    const float along[3] = {direction.x, direction.y, direction.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(along[axis]) < 1e-6f) {
            if (from[axis] < lows[axis] || from[axis] > highs[axis]) {
                t1 = -1.0f;
            }
            continue;
        }
        const float a = (lows[axis] - from[axis]) / along[axis];
        const float b = (highs[axis] - from[axis]) / along[axis];
        t0 = std::max(t0, std::min(a, b));
        t1 = std::min(t1, std::max(a, b));
    }

    if (t0 < t1) {
        const auto above = [&](float t) {
            const Vector3 p = point(t);
            return p.y - surfaceAt(p.x, p.z);
        };
        // Шагом, растущим с расстоянием, до первой точки под поверхностью,
        // затем делением пополам.
        float t = t0;
        bool hit = above(t) <= 0.0f;
        float hitAt = t;
        while (!hit && t < t1) {
            const float next = std::min(t + std::clamp(t * 0.002f, 0.5f, 8.0f), t1);
            if (above(next) <= 0.0f) {
                float low = t;
                float high = next;
                for (int i = 0; i < 20; ++i) {
                    const float middle = 0.5f * (low + high);
                    (above(middle) <= 0.0f ? high : low) = middle;
                }
                hit = true;
                hitAt = high;
            }
            t = next;
        }
        if (hit) {
            const Vector3 p = point(hitAt);
            return Vector2{p.x, p.z};
        }
    }

    // Мимо рельефа — за край карты: плоскость уровня моря, как раньше.
    if (direction.y > -1e-4f) {
        return std::nullopt;
    }
    const Vector3 p = point(-origin.y / direction.y);
    return Vector2{p.x, p.z};
}

int Terrain3D::levelFor(const Tile& tile, const Camera3D& camera) const {
    const Vector3 eye = camera.position;
    const float x0 = static_cast<float>(tile.left) * kCell;
    const float z0 = static_cast<float>(tile.top) * kCell;
    const float x1 = static_cast<float>(tile.left + tile.width) * kCell;
    const float z1 = static_cast<float>(tile.top + tile.height) * kCell;
    const float dx = std::max({x0 - eye.x, 0.0f, eye.x - x1});
    const float dy = std::max({tile.minY - eye.y, 0.0f, eye.y - tile.maxY});
    const float dz = std::max({z0 - eye.z, 0.0f, eye.z - z1});
    const float distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0f);
    const float pixelsPerMeter = static_cast<float>(GetScreenHeight()) /
                                 (2.0f * distance * std::tan(camera.fovy * 0.5f * DEG2RAD));
    const float cells = kMaxQuadPixels / (kCell * pixelsPerMeter);
    if (cells < 2.0f) {
        return 0;
    }
    return std::min(static_cast<int>(std::log2(cells)), kLevels - 1);
}

void Terrain3D::build(Tile& tile, int level) {
    buildGround(tile, level);
    buildWater(tile, level);
    tile.built[level] = true;
    ++meshesLoaded_;
}

void Terrain3D::buildGround(Tile& tile, int level) {
    const int step = 1 << level;
    const int nx = (tile.width + step - 1) / step + 1;
    const int nz = (tile.height + step - 1) / step + 1;
    const MapRenderer::GroundChunk chunk = textures_.groundChunk(tile.left, tile.top);
    const int gridQuads = (nx - 1) * (nz - 1);
    const int skirtQuads = 2 * (nx - 1) + 2 * (nz - 1);
    Mesh mesh = allocateMesh(nx * nz + 2 * nx + 2 * nz, 2 * (gridQuads + skirtQuads));

    int vertices = 0;
    const auto put = [&](int cx, int cz, float y) {
        float* position = mesh.vertices + 3 * vertices;
        position[0] = static_cast<float>(cx) * kCell;
        position[1] = y;
        position[2] = static_cast<float>(cz) * kCell;
        // Пиксель текстуры — клетка; угол клетки — край пикселя.
        mesh.texcoords[2 * vertices] = static_cast<float>(cx - chunk.left) / static_cast<float>(chunk.width);
        mesh.texcoords[2 * vertices + 1] = static_cast<float>(cz - chunk.top) / static_cast<float>(chunk.height);
        return vertices++;
    };
    const auto cellX = [&](int i) { return tile.left + std::min(i * step, tile.width); };
    const auto cellZ = [&](int j) { return tile.top + std::min(j * step, tile.height); };
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            put(cellX(i), cellZ(j), ground_[vertexIndex(cellX(i), cellZ(j))]);
        }
    }

    int indices = 0;
    const auto triangle = [&](int a, int b, int c) {
        mesh.indices[indices++] = static_cast<unsigned short>(a);
        mesh.indices[indices++] = static_cast<unsigned short>(b);
        mesh.indices[indices++] = static_cast<unsigned short>(c);
    };
    // Против часовой при взгляде сверху: (x0, z0) → (x0, z1) → (x1, z1) → (x1, z0).
    for (int j = 0; j + 1 < nz; ++j) {
        for (int i = 0; i + 1 < nx; ++i) {
            const int v00 = j * nx + i;
            const int v10 = v00 + 1;
            const int v01 = v00 + nx;
            const int v11 = v01 + 1;
            triangle(v00, v01, v11);
            triangle(v00, v11, v10);
        }
    }

    // Юбки по четырём краям: копия края, опущенная вниз. Рисуются с обеих
    // сторон (отсечение граней у земли выключено), обход не важен.
    const float skirt = skirtDepth(step);
    const auto addSkirt = [&](int count, auto gridVertex) {
        const int first = vertices;
        for (int k = 0; k < count; ++k) {
            const int g = gridVertex(k);
            const float* position = mesh.vertices + 3 * g;
            const int v = vertices++;
            std::copy_n(position, 3, mesh.vertices + 3 * v);
            mesh.vertices[3 * v + 1] -= skirt;
            std::copy_n(mesh.texcoords + 2 * g, 2, mesh.texcoords + 2 * v);
        }
        for (int k = 0; k + 1 < count; ++k) {
            triangle(gridVertex(k), first + k, first + k + 1);
            triangle(gridVertex(k), first + k + 1, gridVertex(k + 1));
        }
    };
    addSkirt(nx, [&](int k) { return k; });
    addSkirt(nx, [&](int k) { return (nz - 1) * nx + k; });
    addSkirt(nz, [&](int k) { return k * nx; });
    addSkirt(nz, [&](int k) { return k * nx + nx - 1; });

    upload(mesh);
    tile.ground[level] = mesh;
}

void Terrain3D::buildWater(Tile& tile, int level) {
    if (!tile.hasWater) {
        return;
    }
    const int step = 1 << level;
    const int nx = (tile.width + step - 1) / step + 1;
    const int nz = (tile.height + step - 1) / step + 1;
    const auto latticeIndex = [nx](int i, int j) { return static_cast<std::size_t>(j * nx + i); };
    const auto cellX = [&](int i) { return tile.left + std::min(i * step, tile.width); };
    const auto cellZ = [&](int j) { return tile.top + std::min(j * step, tile.height); };

    // Уровень в узлах: у воды — свой, у суши рядом с водой — средний по
    // соседним мокрым узлам, но не выше земли: берег — край водных клеток,
    // даже если суша за ним ниже воды.
    std::vector<float> levels(static_cast<std::size_t>(nx * nz), kNoWater);
    std::vector<unsigned char> wet(levels.size(), 0);
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            const float w = water_[vertexIndex(cellX(i), cellZ(j))];
            if (w != kNoWater) {
                levels[latticeIndex(i, j)] = w;
                wet[latticeIndex(i, j)] = 1;
            }
        }
    }
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            if (wet[latticeIndex(i, j)] != 0) {
                continue;
            }
            float sum = 0.0f;
            int count = 0;
            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    const int ni = i + di;
                    const int nj = j + dj;
                    if (ni >= 0 && nj >= 0 && ni < nx && nj < nz && wet[latticeIndex(ni, nj)] != 0) {
                        sum += levels[latticeIndex(ni, nj)];
                        ++count;
                    }
                }
            }
            if (count > 0) {
                const float ground = ground_[vertexIndex(cellX(i), cellZ(j))];
                levels[latticeIndex(i, j)] = std::min(sum / static_cast<float>(count), ground - kShoreDepth);
            }
        }
    }

    // Клетка решётки с водой хотя бы в одном углу.
    const auto quadWet = [&](int i, int j) {
        return wet[latticeIndex(i, j)] != 0 || wet[latticeIndex(i + 1, j)] != 0 ||
               wet[latticeIndex(i, j + 1)] != 0 || wet[latticeIndex(i + 1, j + 1)] != 0;
    };
    std::vector<int> remap(levels.size(), -1);
    int vertexCount = 0;
    int quadCount = 0;
    for (int j = 0; j + 1 < nz; ++j) {
        for (int i = 0; i + 1 < nx; ++i) {
            if (!quadWet(i, j)) {
                continue;
            }
            ++quadCount;
            for (const std::size_t corner : {latticeIndex(i, j), latticeIndex(i + 1, j),
                                             latticeIndex(i, j + 1), latticeIndex(i + 1, j + 1)}) {
                if (remap[corner] < 0) {
                    remap[corner] = vertexCount++;
                }
            }
        }
    }
    if (quadCount == 0) {
        return;
    }

    Mesh mesh = allocateMesh(vertexCount, 2 * quadCount);
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int v = remap[latticeIndex(i, j)];
            if (v < 0) {
                continue;
            }
            const float y = levels[latticeIndex(i, j)];
            mesh.vertices[3 * v] = static_cast<float>(cellX(i)) * kCell;
            mesh.vertices[3 * v + 1] = y;
            mesh.vertices[3 * v + 2] = static_cast<float>(cellZ(j)) * kCell;
            // Глубина в узле — для цвета и прозрачности в шейдере; у берега отрицательна.
            mesh.texcoords[2 * v] = y - ground_[vertexIndex(cellX(i), cellZ(j))];
            mesh.texcoords[2 * v + 1] = 0.0f;
        }
    }
    int indices = 0;
    for (int j = 0; j + 1 < nz; ++j) {
        for (int i = 0; i + 1 < nx; ++i) {
            if (!quadWet(i, j)) {
                continue;
            }
            const auto v00 = static_cast<unsigned short>(remap[latticeIndex(i, j)]);
            const auto v10 = static_cast<unsigned short>(remap[latticeIndex(i + 1, j)]);
            const auto v01 = static_cast<unsigned short>(remap[latticeIndex(i, j + 1)]);
            const auto v11 = static_cast<unsigned short>(remap[latticeIndex(i + 1, j + 1)]);
            for (const unsigned short index : {v00, v01, v11, v00, v11, v10}) {
                mesh.indices[indices++] = index;
            }
        }
    }
    upload(mesh);
    tile.water[level] = mesh;
}

void Terrain3D::unload(Tile& tile, int level) {
    if (!tile.built[level]) {
        return;
    }
    for (Mesh* mesh : {&tile.ground[level], &tile.water[level]}) {
        if (mesh->vertexCount > 0) {
            UnloadMesh(*mesh);
        }
        *mesh = Mesh{};
    }
    tile.built[level] = false;
    --meshesLoaded_;
}

void Terrain3D::drawGround(const Camera3D& camera, float snowAmount) {
    ++frame_;
    textures_.useGroundFilter();
    // Внутри BeginMode3D модельно-видовая матрица — это вид камеры.
    const Matrix viewProjection = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());

    visible_.clear();
    int builds = 0;
    for (std::size_t index = 0; index < tiles_.size(); ++index) {
        Tile& tile = tiles_[index];
        const Vector3 min = {static_cast<float>(tile.left) * kCell, tile.minY,
                             static_cast<float>(tile.top) * kCell};
        const Vector3 max = {static_cast<float>(tile.left + tile.width) * kCell, tile.maxY,
                             static_cast<float>(tile.top + tile.height) * kCell};
        if (outsideFrustum(viewProjection, min, max)) {
            continue;
        }
        int level = levelFor(tile, camera);
        if (!tile.built[level] && builds < kBuildsPerFrame) {
            build(tile, level);
            ++builds;
        }
        // Пока подробный уровень не построен — ближайший грубее (грубые есть всегда).
        while (!tile.built[level]) {
            ++level;
        }
        tile.lastUsed[level] = frame_;
        visible_.push_back({static_cast<int>(index), level});
    }

    tilesDrawn_ = static_cast<int>(visible_.size());
    trianglesDrawn_ = 0;
    const unsigned char snow = static_cast<unsigned char>(std::clamp(snowAmount, 0.0f, 1.0f) * 255.0f);
    // Цвета вершин у сетки нет: шейдер берёт значение атрибута по умолчанию,
    // а оно общее для всех — ставим белое явно.
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    rlSetVertexAttributeDefault(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR, white, SHADER_ATTRIB_VEC4, 4);
    // Юбки видны с обеих сторон; снизу на землю не смотрят, так что отсечение не нужно.
    rlDisableBackfaceCulling();
    for (int layer = 0; layer < (snow > 0 ? 2 : 1); ++layer) {
        // Снег — та же сетка второй раз: глубина совпадает, и он ложится сверху.
        const unsigned char alpha = layer == 0 ? static_cast<unsigned char>(255) : snow;
        groundMaterial_.maps[MATERIAL_MAP_DIFFUSE].color = {255, 255, 255, alpha};
        for (const Visible& visible : visible_) {
            const Tile& tile = tiles_[static_cast<std::size_t>(visible.tile)];
            const MapRenderer::GroundChunk chunk = textures_.groundChunk(tile.left, tile.top);
            groundMaterial_.maps[MATERIAL_MAP_DIFFUSE].texture = layer == 0 ? chunk.texture : chunk.snow;
            const Mesh& mesh = tile.ground[static_cast<std::size_t>(visible.level)];
            DrawMesh(mesh, groundMaterial_, MatrixIdentity());
            trianglesDrawn_ += layer == 0 ? mesh.triangleCount : 0;
        }
    }
    rlEnableBackfaceCulling();

    // Подробные уровни, давно не нужные, — из видеопамяти.
    for (Tile& tile : tiles_) {
        for (int level = 0; level < kFirstEagerLevel; ++level) {
            if (tile.built[level] && frame_ - tile.lastUsed[level] > kEvictFrames) {
                unload(tile, level);
            }
        }
    }
}

void Terrain3D::drawWater(const Camera3D& camera, float time, Color sky) {
    // Всё непрозрачное, накопленное в батче rlgl, — раньше воды: сквозь воду
    // видно то, что под ней.
    rlDrawRenderBatchActive();
    const Vector3 skyColor = {static_cast<float>(sky.r) / 255.0f, static_cast<float>(sky.g) / 255.0f,
                              static_cast<float>(sky.b) / 255.0f};
    SetShaderValue(waterShader_, viewPosLoc_, &camera.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(waterShader_, timeLoc_, &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(waterShader_, skyLoc_, &skyColor, SHADER_UNIFORM_VEC3);
    for (const Visible& visible : visible_) {
        const Mesh& mesh = tiles_[static_cast<std::size_t>(visible.tile)].water[static_cast<std::size_t>(visible.level)];
        if (mesh.vertexCount > 0) {
            DrawMesh(mesh, waterMaterial_, MatrixIdentity());
            trianglesDrawn_ += mesh.triangleCount;
        }
    }
}

void Terrain3D::beginDrape() const {
    BeginShaderMode(drapeShader_);
    // После BeginShaderMode: текстура привязывается к текущему шейдеру батча.
    SetShaderValueTexture(drapeShader_, heightsLoc_, heights_);
}

void Terrain3D::endDrape() const {
    EndShaderMode();
}

} // namespace lw
