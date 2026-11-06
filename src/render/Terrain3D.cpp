#include "render/Terrain3D.h"

#include "render/FieldLayout.h"
#include "render/LandUse.h"
#include "render/Lighting3D.h"
#include "render/MapRenderer.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace lw {

namespace {

constexpr float kCell = Map::kCellSize;
constexpr float kNoWater = -1.0e9f;

// Поверхность воды чуть ниже расчётной (LandUse::channelInset: у озёр и моря
// на 0,3 м, у рек — глубже): берега и долины, засыпанные ровно до уровня озера,
// остаются сухими, а не мерцают вровень с водой.
// Угол на границе воды и суши — на столько ниже поверхности воды: берег
// проходит по краю водных клеток, а не заходит в сухие.
constexpr float kShoreDepth = 0.1f;
// У пресной воды угол на границе — не мельче этого (этап 11.9): кромка воды
// заходит в соседнюю сухую клетку на метр-полтора, выше неё — откос берега.
constexpr float kRiverEdgeDepth = 0.3f;
constexpr float kLakeEdgeDepth = 0.2f;
// Уровень воды в вершине земли, где воды рядом нет: дно и мокрый берег не рисуются.
constexpr float kDryLevel = -1000.0f;
// Глубже этого существо уже не идёт по дну, а плывёт.
constexpr float kSwimDepth = 1.3f;
// Настил моста над уровнем воды.
constexpr float kBridgeDeck = 0.6f;
// Отладочные слои — над поверхностью: вблизи — у самой земли (кольцо выбора —
// у ног), вдали выше, чтобы не тонуть в грубых уровнях рельефа.
constexpr float kDrapeLift = 0.15f;

// Клетка на экране не крупнее стольких пикселей: так выбирается уровень детализации.
constexpr float kMaxQuadPixels = 8.0f;
// Запас при смене уровня детализации, в долях уровня (двоичный логарифм клеток).
constexpr float kLevelHysteresis = 0.25f;
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

// Земля: текстура карты (пиксель — клетка) с рваными краями клеток, детальный
// шум, снег по маске, свет по нормали, тень и дымка (render/Lighting3D). С
// этапа 11.9: поля по сезону (лоскуты render/FieldLayout — борозды, всходы,
// колосья, стерня, межи), луга по сезону с цветами, канавы вдоль дорог, дно и
// мокрый берег у воды (texcoord2.x — уровень воды у вершины).
constexpr const char* kGroundVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec3 vertexNormal;
uniform mat4 mvp;
out vec3 fragPosition;
out vec2 fragTexCoord;
out vec3 fragNormal;
out float fragWater;
void main() {
    fragPosition = vertexPosition;
    fragTexCoord = vertexTexCoord;
    fragNormal = vertexNormal;
    fragWater = vertexTexCoord2.x;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kGroundFs = R"(
in vec3 fragPosition;
in vec2 fragTexCoord;
in vec3 fragNormal;
in float fragWater;
uniform sampler2D texture0; // карта
uniform sampler2D texture1; // снег: альфа — сколько клетки заносит
uniform sampler2D texture2; // маски: R — лиственный лес, G — поле, B — луг, A — дорога
uniform float snowAmount;
uniform vec3 foliage;       // листва, пожелтение, молодая листва — в среднем
uniform float year;         // доля года с 1 марта

out vec4 finalColor;

// Клетки чёткие, но без ступенек: внутри клетки координата прижата к её
// середине, а через край клетки переходит за один пиксель экрана (билинейный
// фильтр сглаживает именно этот пиксель). Ближайший тексель давал жёсткие края,
// которые на клетке в пиксель-два рябили при каждом движении камеры. Где
// клетка мельче пикселя, координата не меняется — дальше работают мип-уровни.
vec2 crisp(vec2 uv, vec2 size, vec2 cellsPerPixel) {
    vec2 t = uv * size;
    vec2 seam = floor(t + 0.5);
    // При клетке в пиксель и мельче делитель — 1, и t не меняется.
    t = seam + clamp((t - seam) / clamp(cellsPerPixel, vec2(1e-4), vec2(1.0)), -0.5, 0.5);
    return t / size;
}

// Поле: между рядами — пашня с бороздами или стерня, на рядах — растения по
// сезону; у границы лоскута — межа травой. footprint — метров на пиксель:
// ряды мельче пикселя сливаются в средний цвет, а не рябят.
vec3 fieldColor(vec2 p, vec3 meadow, float footprint) {
    Patch plot = fieldPatch(p);
    Crop crop = cropState(plot.crop, year, plot.timing);
    float spacing = rowSpacing(plot.crop);
    vec2 n = vec2(-sin(plot.angle), cos(plot.angle));
    float r = dot(p, n) / spacing;
    float rows = clamp(footprint / spacing * 1.5, 0.0, 1.0); // 1 — ряды мельче пикселя
    float furrow = mix(0.5 + 0.5 * cos(6.2831853 * r), 0.5, rows);
    vec3 soil = mix(vec3(0.27, 0.21, 0.16), vec3(0.42, 0.34, 0.25), furrow);
    // Стерня — светлая солома полосами по рядам.
    vec3 stubble = mix(vec3(0.60, 0.53, 0.34), vec3(0.76, 0.68, 0.44), furrow);
    vec3 between = mix(soil, stubble, crop.stubble);
    float from = abs(fract(r) - 0.5);
    float aa = max(fwidth(r), 0.02);
    float inRow = 1.0 - smoothstep(0.5 * crop.cover - aa, 0.5 * crop.cover + aa, from);
    inRow = mix(inRow, crop.cover, rows);
    float plants = inRow * clamp(crop.h * 6.0, 0.0, 1.0) * (1.0 - crop.stubble);
    vec3 color = mix(between, crop.plant * 0.82, plants);
    color = mix(color, meadow, crop.meadow);
    // Межа — полоса травы по границе лоскута.
    return mix(meadow, color, smoothstep(kBalkWidth - 0.3, kBalkWidth + 0.3, plot.edge));
}

// Цветы на лугу — точки по сезону, только вблизи (мельче пары пикселей рябили бы).
vec3 withFlowers(vec3 color, vec2 p, float footprint) {
    float visible = 1.0 - smoothstep(0.012, 0.03, footprint);
    if (visible <= 0.0) {
        return color;
    }
    const float kStep = 0.3;
    vec2 g = floor(p / kStep);
    uint h = hash3(int(g.x), int(g.y), 77u);
    float kind = hashUnit(h * 2654435761u);
    float spring = ramp(0.08, 0.13, year) * (1.0 - ramp(0.26, 0.32, year));
    float summer = ramp(0.22, 0.28, year) * (1.0 - ramp(0.44, 0.50, year));
    float density = 0.07 * (kind < 0.5 ? spring : summer);
    if (hashUnit(h) >= density) {
        return color;
    }
    vec2 center = (g + 0.2 + 0.6 * vec2(hashUnit(h >> 3), hashUnit(h >> 7))) * kStep;
    float spot = 1.0 - smoothstep(0.03, 0.03 + footprint, length(p - center));
    vec3 petals = kind < 0.25 ? vec3(0.95, 0.95, 0.90) : kind < 0.5 ? vec3(0.95, 0.85, 0.20)
                : kind < 0.7 ? vec3(0.45, 0.55, 0.95) : kind < 0.88 ? vec3(0.70, 0.40, 0.85)
                : vec3(0.90, 0.20, 0.15);
    return mix(color, petals, spot * visible);
}

void main() {
    vec2 size = vec2(textureSize(texture0, 0));
    vec2 p = fragPosition.xz;
    // Края клеток — рваные, а не квадраты: координата текстуры чуть сдвинута
    // шумом. Вдали, где клетка мельче пикселя, сдвиг только рябил бы.
    vec2 cellsPerPixel = fwidth(fragTexCoord * size);
    float near = 1.0 - smoothstep(0.3, 1.0, max(cellsPerPixel.x, cellsPerPixel.y));
    vec2 jitter = (texture(detailMap, p / 12.0).ba - 0.5) * 0.5 * near;
    vec2 uv = crisp(fragTexCoord + jitter / size, size, cellsPerPixel);
    // Мип-уровень и анизотропия — по настоящему шагу координаты, а не по
    // прижатой: иначе внутри клетки выбирался бы слишком чёткий уровень.
    vec2 dx = dFdx(fragTexCoord);
    vec2 dy = dFdy(fragTexCoord);
    vec3 base = textureGrad(texture0, uv, dx, dy).rgb;
    vec4 masks = textureGrad(texture2, uv, dx, dy);
    // Лиственный лес под кронами и вдали, где деревьев уже нет, — по сезону.
    float deciduous = masks.r;
    if (deciduous > 0.0) {
        vec3 green = base * mix(vec3(1.08, 1.06, 0.92), vec3(1.35, 1.32, 0.8), foliage.z);
        vec3 leafy = mix(green, vec3(0.44, 0.33, 0.12), foliage.y);
        base = mix(base, mix(vec3(0.27, 0.24, 0.2), leafy, foliage.x), deciduous);
    }
    float footprint = max(length(fwidth(p)), 1e-4);
    float broad = texture(detailMap, p / 230.0).g;
    float patchy = texture(detailMap, p / 70.0).g;
    // Луг и межи — по сезону; на поле — посевы.
    vec3 meadow = meadowColor(year, patchy) * (0.92 + 0.16 * texture(detailMap, p / 9.0).r);
    if (masks.b > 0.01) {
        base = mix(base, withFlowers(meadow, p, footprint), masks.b);
    }
    if (masks.g > 0.01) {
        base = mix(base, fieldColor(p, meadow, footprint), masks.g);
    }
    // Канава вдоль дороги: в полутора метрах от края полотна — сырая, темнее и зеленее.
    // Расстояние до полотна — до ближайшей из восьми соседних дорожных клеток,
    // с тем же сдвигом краёв, что у самой дороги: канава идёт вдоль неё, а не
    // уголками по клеткам.
    if (masks.a < 0.5 && near > 0.0) {
        vec2 t = (fragTexCoord + jitter / size) * size;
        ivec2 c = ivec2(floor(t));
        ivec2 last = ivec2(size) - 1;
        float edge = 9.0;
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                ivec2 n = clamp(c + ivec2(i, j), ivec2(0), last);
                if ((i != 0 || j != 0) && texelFetch(texture2, n, 0).a > 0.5) {
                    vec2 outside = max(max(vec2(n) - t, t - vec2(n) - 1.0), vec2(0.0));
                    edge = min(edge, length(outside));
                }
            }
        }
        float ditch = (1.0 - smoothstep(0.25, 0.5, abs(edge * 4.0 - 1.3))) * near;
        base = mix(base, base * vec3(0.74, 0.82, 0.70), ditch);
    }
    // У воды: мокрый берег темнее, под водой — дно (песок и галька, глубже — ил).
    float under = fragWater - fragPosition.y;
    if (under > -0.45) {
        base *= mix(1.0, 0.7, smoothstep(-0.45, 0.0, under));
        vec3 bed = mix(vec3(0.44, 0.40, 0.30), vec3(0.22, 0.22, 0.18), smoothstep(0.3, 2.5, under));
        base = mix(base, bed, smoothstep(0.0, 0.25, under));
    }
    float fine = texture(detailMap, p / 21.0).r;
    base *= 0.78 + 0.26 * fine + 0.18 * broad;
    float snow = textureGrad(texture1, uv, dx, dy).a * snowAmount * (1.0 - smoothstep(-0.1, 0.0, under));
    base = mix(base, vec3(0.90, 0.92, 0.96) * (0.94 + 0.08 * fine), snow);
    vec3 n = normalize(fragNormal);
    vec3 color = base * lighting(n, shadowAt(fragPosition, n));
    finalColor = vec4(withFog(color, fragPosition), 1.0);
}
)";

// Вода: рябь — сумма бегущих волн, цвет — от глубины и света, под углом
// отражает небо, у берега прозрачна; блик — от солнца или луны. С этапа 11.9:
// волны бегут по ветру и растут с ним; на реке рябь сносит по течению
// (нормаль вершины — скорость течения по x и z, в y — перекат), на перекатах — пена.
constexpr const char* kWaterVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
uniform mat4 mvp;
out vec3 fragPosition;
out float fragDepth;
out vec3 fragFlow;
void main() {
    fragPosition = vertexPosition;
    fragDepth = vertexTexCoord.x;
    fragFlow = vertexNormal;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kWaterFs = R"(
in vec3 fragPosition;
in float fragDepth;
in vec3 fragFlow;
uniform float time;
uniform vec3 skyColor;
uniform vec2 wind;
uniform vec4 colDiffuse;
out vec4 finalColor;

const vec3 shallowColor = vec3(0.20, 0.44, 0.48);
const vec3 deepColor = vec3(0.04, 0.15, 0.28);

// Метров воды на пиксель экрана — в main.
float footprint;

// Волна, мельче пары пикселей, гаснет: иначе её блик вспыхивал бы в случайных
// пикселях и вода искрилась при каждом движении камеры.
vec2 wave(vec2 p, vec2 dir, float frequency, float speed, float amplitude) {
    float resolved = 1.0 - smoothstep(0.4, 1.2, frequency * footprint);
    return dir * (amplitude * resolved * cos(dot(p, dir) * frequency + time * speed));
}

vec2 turn(vec2 v, float a) {
    float c = cos(a);
    float s = sin(a);
    return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

// Рябь течения: шум сносит по течению двумя фазами со сдвигом на полпериода —
// узор не растягивается (карта течения). noise — для пены.
vec2 current(vec2 p, vec2 velocity, out float noise) {
    const float kCycle = 1.6;
    float t = time / kCycle;
    float phase0 = fract(t);
    float phase1 = fract(t + 0.5);
    float w0 = 1.0 - abs(1.0 - 2.0 * phase0);
    vec2 a = p - velocity * phase0 * kCycle;
    vec2 b = p - velocity * phase1 * kCycle + vec2(1.37, 0.71);
    vec4 fineA = texture(detailMap, a / 4.0);
    vec4 fineB = texture(detailMap, b / 4.0);
    vec4 wideA = texture(detailMap, a / 13.0);
    vec4 wideB = texture(detailMap, b / 13.0);
    vec2 slope = mix((fineB.ba - 0.5) + 0.7 * (wideB.ba - 0.5), (fineA.ba - 0.5) + 0.7 * (wideA.ba - 0.5), w0);
    noise = mix(0.6 * fineB.r + 0.4 * wideB.g, 0.6 * fineA.r + 0.4 * wideA.g, w0);
    return slope;
}

void main() {
    vec3 toEye = viewPos - fragPosition;
    float distance = length(toEye);
    vec3 v = toEye / max(distance, 1e-3);
    vec2 p = fragPosition.xz;
    footprint = max(length(fwidth(p)), 1e-4);
    // Волны — по ветру, в ветер выше.
    float windSpeed = length(wind);
    vec2 along = windSpeed > 0.1 ? wind / windSpeed : vec2(0.80, 0.60);
    float gust = 0.35 + 0.65 * clamp(windSpeed / 8.0, 0.0, 1.5);
    vec2 slope = gust * (wave(p, along, 0.90, 1.7, 0.050)
               + wave(p, turn(along, 0.6), 1.70, 2.4, 0.030)
               + wave(p, turn(along, -0.5), 3.10, 3.3, 0.020)
               + wave(p, turn(along, 1.2), 0.35, 1.0, 0.040));
    // Течение реки.
    vec2 velocity = fragFlow.xz;
    float speed = length(velocity);
    float rapids = clamp(fragFlow.y, 0.0, 1.0);
    float foam = 0.0;
    if (speed > 0.02) {
        float noise;
        vec2 ripple = current(p, velocity, noise);
        float resolved = 1.0 - smoothstep(0.06, 0.25, footprint);
        slope = slope * 0.5 + ripple * (0.18 + 0.12 * speed + 0.3 * rapids) * resolved;
        foam = smoothstep(0.62, 0.8, noise + 0.3 * rapids) * rapids * (0.4 + 0.6 * resolved);
    }
    // Вдали рябь мельче пикселя и только мерцала бы.
    slope /= 1.0 + distance / 120.0;
    vec3 n = normalize(vec3(-slope.x, 1.0, -slope.y));
    float fresnel = 0.03 + 0.97 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    float depth = max(fragDepth, 0.0);
    float deep = clamp(depth / 10.0, 0.0, 1.0);
    vec3 body = mix(shallowColor, deepColor, sqrt(deep)) * lighting(vec3(0.0, 1.0, 0.0), 1.0);
    vec3 color = mix(body, skyColor, fresnel);
    color += lightColor * (0.9 * pow(max(dot(n, normalize(lightDir + v)), 0.0), 250.0));
    float alpha = smoothstep(0.0, 0.5, depth) * mix(0.5, 0.9, deep);
    alpha = mix(alpha, 1.0, fresnel * smoothstep(0.0, 0.3, depth));
    vec3 white = vec3(0.92, 0.94, 0.95) * lighting(vec3(0.0, 1.0, 0.0), 1.0);
    color = mix(color, white, foam);
    alpha = max(alpha, foam * smoothstep(0.0, 0.1, depth));
    finalColor = vec4(withFog(color, fragPosition), alpha) * colDiffuse;
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
    // w в перспективе — расстояние от камеры вдоль взгляда: на 250 м — ещё метр.
    float distance = (mvp * vec4(vertexPosition.x, h, vertexPosition.z, 1.0)).w;
    gl_Position = mvp * vec4(vertexPosition.x, h + lift + 0.004 * distance, vertexPosition.z, 1.0);
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

} // namespace

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

namespace {

Mesh allocateMesh(int vertexCount, int triangleCount, bool normals = false, bool texcoords2 = false) {
    Mesh mesh{};
    mesh.vertexCount = vertexCount;
    mesh.triangleCount = triangleCount;
    mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertexCount) * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertexCount) * 2 * sizeof(float)));
    if (normals) {
        mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertexCount) * 3 * sizeof(float)));
    }
    if (texcoords2) {
        mesh.texcoords2 = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertexCount) * 2 * sizeof(float)));
    }
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
    MemFree(mesh.normals);
    MemFree(mesh.texcoords2);
    mesh.vertices = nullptr;
    mesh.texcoords = nullptr;
    mesh.normals = nullptr;
    mesh.texcoords2 = nullptr;
}

} // namespace

Terrain3D::Terrain3D(const Map& map, MapRenderer& textures, const LandUse& land)
    : map_(map), textures_(textures), land_(land) {
    const int width = map.width();
    const int height = map.height();
    const std::size_t count = static_cast<std::size_t>(width + 1) * static_cast<std::size_t>(height + 1);
    ground_.resize(count);
    water_.assign(count, kNoWater);
    surface_.resize(count);
    minSurface_ = std::numeric_limits<float>::max();
    maxSurface_ = std::numeric_limits<float>::lowest();
    // Высота берега над водой у углов пресной воды (для откосов) — только на время постройки.
    std::vector<float> bank(count, 0.0f);
    for (int vy = 0; vy <= height; ++vy) {
        for (int vx = 0; vx <= width; ++vx) {
            // Угол делят до четырёх клеток. Среди воды высота — по дну. На границе
            // воды и суши — чуть ниже поверхности воды: по среднему соседей узкая
            // река оказалась бы выше своих берегов, а по дну берег проседал бы
            // внутрь сухой клетки, и вода растекалась бы по нему. Даже если суша
            // ниже реки (русло поперёк склона), угол не опускается: вдоль русла
            // получается невысокий вал, и вода остаётся в своих клетках.
            // С этапа 11.9 вода реки опущена глубже (русло врезано, LandUse::
            // channelInset), а угол на границе с пресной водой — заметно ниже
            // воды: кромка уходит в сухую клетку, выше — откос берега.
            float land = 0.0f;
            int lands = 0;
            float bed = 0.0f;
            float level = 0.0f;
            float banks = 0.0f;
            int wet = 0;
            bool river = false;
            bool lake = false;
            for (int cy = vy - 1; cy <= vy; ++cy) {
                for (int cx = vx - 1; cx <= vx; ++cx) {
                    const Cell cell{cx, cy};
                    if (!map.contains(cell)) {
                        continue;
                    }
                    const float elevation = map.elevationMeters(cell);
                    const Water water = map.water.at(cx, cy);
                    if (water != Water::None) {
                        bed += elevation;
                        level += waterLevel(map, cell) - land_.channelInset(cell);
                        banks = std::max(banks, land_.bankHeight(cell));
                        river = river || water == Water::River;
                        lake = lake || water == Water::Lake;
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
                water_[i] = level / static_cast<float>(wet);
                ground_[i] = bed / static_cast<float>(wet);
                bank[i] = banks;
                if (wet < lands) {
                    ground_[i] = std::max(ground_[i], water_[i] - kShoreDepth);
                    if (river || lake) {
                        ground_[i] = std::min(ground_[i], water_[i] - (river ? kRiverEdgeDepth : kLakeEdgeDepth));
                    }
                }
            }
        }
    }
    // Берег над пресной водой — не ниже своей высоты: у русла поперёк склона
    // нижний берег поднимается валом, и откос есть с обеих сторон.
    for (int vy = 0; vy <= height; ++vy) {
        for (int vx = 0; vx <= width; ++vx) {
            const std::size_t i = vertexIndex(vx, vy);
            if (water_[i] != kNoWater) {
                continue;
            }
            float wanted = std::numeric_limits<float>::lowest();
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = vx + dx;
                    const int ny = vy + dy;
                    if (nx < 0 || ny < 0 || nx > width || ny > height) {
                        continue;
                    }
                    const std::size_t n = vertexIndex(nx, ny);
                    if (water_[n] != kNoWater && bank[n] > 0.0f) {
                        wanted = std::max(wanted, water_[n] + bank[n]);
                    }
                }
            }
            ground_[i] = std::max(ground_[i], wanted);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        surface_[i] = std::max(ground_[i], water_[i]);
        minSurface_ = std::min(minSurface_, surface_[i]);
        maxSurface_ = std::max(maxSurface_, surface_[i]);
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (map.road.at(x, y) != Road::None && map.water.at(x, y) != Water::None) {
                bridges_.push_back({{x, y}, waterLevel(map, {x, y}) + kBridgeDeck, map.road.at(x, y), 0});
            }
        }
    }
    buildSpans();

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

    const std::string groundFs = std::string(kFieldGlsl) + kGroundFs;
    groundShader_ = SceneShading::loadShader(kGroundVs, groundFs.c_str());
    yearLoc_ = GetShaderLocation(groundShader_, "year");
    snowLoc_ = GetShaderLocation(groundShader_, "snowAmount");
    foliageLoc_ = GetShaderLocation(groundShader_, "foliage");
    groundMaterial_ = LoadMaterialDefault();
    groundMaterial_.shader = groundShader_;
    casterMaterial_ = LoadMaterialDefault();

    waterShader_ = SceneShading::loadShader(kWaterVs, kWaterFs);
    timeLoc_ = GetShaderLocation(waterShader_, "time");
    skyLoc_ = GetShaderLocation(waterShader_, "skyColor");
    windLoc_ = GetShaderLocation(waterShader_, "wind");
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
    UnloadShader(groundShader_);
    UnloadShader(waterShader_);
    UnloadShader(drapeShader_);
    UnloadTexture(heights_);
    // UnloadMaterial выгрузил бы и текстуры карты, а они не наши.
    MemFree(groundMaterial_.maps);
    MemFree(casterMaterial_.maps);
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

void Terrain3D::buildSpans() {
    const int width = map_.width();
    const auto key = [width](Cell cell) { return static_cast<std::int64_t>(cell.y) * width + cell.x; };
    for (std::size_t i = 0; i < bridges_.size(); ++i) {
        bridgeAt_[key(bridges_[i].cell)] = i;
    }
    const auto center = [](Cell cell) { return Vector2{Map::cellCenter(cell.x), Map::cellCenter(cell.y)}; };
    std::vector<bool> done(bridges_.size(), false);
    for (std::size_t first = 0; first < bridges_.size(); ++first) {
        if (done[first]) {
            continue;
        }
        // Связные клетки моста (и по диагонали: дорога идёт лесенкой) и клетки
        // дороги на суше рядом с ними — берега.
        std::vector<std::size_t> cells{first};
        std::vector<Cell> banks;
        done[first] = true;
        for (std::size_t k = 0; k < cells.size(); ++k) {
            const Cell cell = bridges_[cells[k]].cell;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const Cell next = {cell.x + dx, cell.y + dy};
                    if ((dx == 0 && dy == 0) || !map_.contains(next) || map_.road.at(next.x, next.y) == Road::None) {
                        continue;
                    }
                    if (const auto it = bridgeAt_.find(key(next)); it != bridgeAt_.end()) {
                        if (!done[it->second]) {
                            done[it->second] = true;
                            cells.push_back(it->second);
                        }
                    } else if (std::none_of(banks.begin(), banks.end(),
                                            [&](Cell b) { return b.x == next.x && b.y == next.y; })) {
                        banks.push_back(next);
                    }
                }
            }
        }

        Span span;
        span.deck = std::numeric_limits<float>::lowest();
        for (const std::size_t i : cells) {
            span.deck = std::max(span.deck, bridges_[i].deck);
            if (bridges_[i].road == Road::Asphalt) {
                span.road = Road::Asphalt;
            }
        }
        // Прямой пролёт: дорога на суше у моста — ровно два связных куска, по
        // берегу с каждой стороны; ось — от середины одного до середины другого,
        // все клетки моста — между ними.
        std::vector<int> group(banks.size(), -1);
        int groups = 0;
        for (std::size_t seed = 0; seed < banks.size(); ++seed) {
            if (group[seed] >= 0) {
                continue;
            }
            std::vector<std::size_t> open{seed};
            group[seed] = groups;
            while (!open.empty()) {
                const Cell cell = banks[open.back()];
                open.pop_back();
                for (std::size_t j = 0; j < banks.size(); ++j) {
                    if (group[j] < 0 && std::abs(banks[j].x - cell.x) <= 1 && std::abs(banks[j].y - cell.y) <= 1) {
                        group[j] = groups;
                        open.push_back(j);
                    }
                }
            }
            ++groups;
        }
        if (groups == 2) {
            Vector2 sum[2] = {};
            float count[2] = {};
            for (std::size_t j = 0; j < banks.size(); ++j) {
                sum[group[j]] = Vector2Add(sum[group[j]], center(banks[j]));
                count[group[j]] += 1.0f;
            }
            span.a = Vector2Scale(sum[0], 1.0f / count[0]);
            span.b = Vector2Scale(sum[1], 1.0f / count[1]);
            const float length = Vector2Distance(span.a, span.b);
            if (length > 1.0f) {
                const Vector2 along = Vector2Scale(Vector2Subtract(span.b, span.a), 1.0f / length);
                span.straight = true;
                float widest = 0.0f;
                for (const std::size_t i : cells) {
                    const Vector2 p = Vector2Subtract(center(bridges_[i].cell), span.a);
                    const float t = Vector2DotProduct(p, along);
                    const float off = std::abs(p.x * along.y - p.y * along.x);
                    widest = std::max(widest, off);
                    if (t < 0.0f || t > length || off > 1.5f * Map::kCellSize) {
                        span.straight = false;
                        break;
                    }
                }
                // Настил закрывает клетки дороги над водой: середины клеток — не
                // дальше 1,5 м от края.
                span.halfWidth = std::max(span.road == Road::Asphalt ? 3.0f : 2.0f, widest + 1.5f);
            }
        }
        if (span.straight) {
            // Настил — не ниже низкого берега: иначе плита упрётся в откос.
            const float low = std::min(groundAt(span.a.x, span.a.y), groundAt(span.b.x, span.b.y));
            span.deck = std::max(span.deck, low + 0.05f);
        }
        const int index = static_cast<int>(spans_.size());
        for (const std::size_t i : cells) {
            bridges_[i].deck = span.deck;
            bridges_[i].span = index;
        }
        spans_.push_back(span);
    }
}

float Terrain3D::groundAt(float x, float y) const {
    return sample(ground_, x, y);
}

float Terrain3D::surfaceAt(float x, float y) const {
    return sample(surface_, x, y);
}

float Terrain3D::standAt(float x, float y) const {
    const Cell cell = map_.clampedCellAt(x, y);
    if (const auto bridge = bridgeAt_.find(static_cast<std::int64_t>(cell.y) * map_.width() + cell.x);
        bridge != bridgeAt_.end()) {
        return bridges_[bridge->second].deck;
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
    // Уровень L — при log2(cells) в [L, L + 1). Прежний уровень держится, пока
    // значение не выйдет за эти границы с запасом: камера, качнувшаяся у
    // границы, не перещёлкивает плитку туда-обратно (рельеф вдали не дрожит).
    const float value = std::log2(std::max(cells, 1e-3f));
    if (tile.level >= 0 && value > static_cast<float>(tile.level) - kLevelHysteresis &&
        value < static_cast<float>(tile.level + 1) + kLevelHysteresis) {
        return tile.level;
    }
    return std::clamp(static_cast<int>(std::floor(value)), 0, kLevels - 1);
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
    Mesh mesh = allocateMesh(nx * nz + 2 * nx + 2 * nz, 2 * (gridQuads + skirtQuads), true, true);

    int vertices = 0;
    const auto put = [&](int cx, int cz, float y) {
        float* position = mesh.vertices + 3 * vertices;
        position[0] = static_cast<float>(cx) * kCell;
        position[1] = y;
        position[2] = static_cast<float>(cz) * kCell;
        // Пиксель текстуры — клетка; угол клетки — край пикселя.
        mesh.texcoords[2 * vertices] = static_cast<float>(cx - chunk.left) / static_cast<float>(chunk.width);
        mesh.texcoords[2 * vertices + 1] = static_cast<float>(cz - chunk.top) / static_cast<float>(chunk.height);
        // Уровень воды у вершины — для дна и мокрого берега; у сухих углов рядом
        // с водой — средний уровень соседних мокрых, дальше от воды — «сухо».
        const int gx = std::clamp(cx, 0, map_.width());
        const int gz = std::clamp(cz, 0, map_.height());
        float water = water_[vertexIndex(gx, gz)];
        if (water == kNoWater) {
            float sum = 0.0f;
            int wet = 0;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = gx + dx;
                    const int nz = gz + dz;
                    if (nx >= 0 && nz >= 0 && nx <= map_.width() && nz <= map_.height() &&
                        water_[vertexIndex(nx, nz)] != kNoWater) {
                        sum += water_[vertexIndex(nx, nz)];
                        ++wet;
                    }
                }
            }
            water = wet > 0 ? sum / static_cast<float>(wet) : kDryLevel;
        }
        mesh.texcoords2[2 * vertices] = water;
        mesh.texcoords2[2 * vertices + 1] = 0.0f;
        // Нормаль — по соседним углам самой подробной сетки: и на грубых уровнях
        // свет лепит мелкие склоны.
        const auto h = [&](int x, int z) {
            return ground_[vertexIndex(std::clamp(x, 0, map_.width()), std::clamp(z, 0, map_.height()))];
        };
        const Vector3 normal = Vector3Normalize(
            {(h(cx - 1, cz) - h(cx + 1, cz)) / (2.0f * kCell), 1.0f, (h(cx, cz - 1) - h(cx, cz + 1)) / (2.0f * kCell)});
        mesh.normals[3 * vertices] = normal.x;
        mesh.normals[3 * vertices + 1] = normal.y;
        mesh.normals[3 * vertices + 2] = normal.z;
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
            std::copy_n(mesh.texcoords2 + 2 * g, 2, mesh.texcoords2 + 2 * v);
            std::copy_n(mesh.normals + 3 * g, 3, mesh.normals + 3 * v);
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

    Mesh mesh = allocateMesh(vertexCount, 2 * quadCount, true);
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
            // Течение в узле — среднее по четырём клеткам вокруг (LandUse::flowAt
            // уже сглажено): x и z — скорость, м/с; y — перекат.
            float fx = 0.0f;
            float fz = 0.0f;
            float rapids = 0.0f;
            for (int dz = -1; dz <= 0; ++dz) {
                for (int dx = -1; dx <= 0; ++dx) {
                    const LandUse::Flow flow = land_.flowAt({cellX(i) + dx, cellZ(j) + dz});
                    fx += 0.25f * flow.x * flow.speed;
                    fz += 0.25f * flow.z * flow.speed;
                    rapids += 0.25f * flow.rapids;
                }
            }
            mesh.normals[3 * v] = fx;
            mesh.normals[3 * v + 1] = rapids;
            mesh.normals[3 * v + 2] = fz;
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

int Terrain3D::shownLevel(const Tile& tile, const Camera3D& camera) const {
    int level = levelFor(tile, camera);
    // Пока подробный уровень не построен — ближайший грубее (грубые есть всегда).
    while (!tile.built[level]) {
        ++level;
    }
    return level;
}

void Terrain3D::drawShadowCasters(const Camera3D& camera, Vector2 center, float radius) {
    // Тот же уровень, что и в кадре: тень ложится на ту же поверхность.
    rlDisableBackfaceCulling();
    for (const Tile& tile : tiles_) {
        const float x0 = static_cast<float>(tile.left) * kCell;
        const float z0 = static_cast<float>(tile.top) * kCell;
        const float x1 = static_cast<float>(tile.left + tile.width) * kCell;
        const float z1 = static_cast<float>(tile.top + tile.height) * kCell;
        if (x1 < center.x - radius || x0 > center.x + radius || z1 < center.y - radius || z0 > center.y + radius) {
            continue;
        }
        DrawMesh(tile.ground[static_cast<std::size_t>(shownLevel(tile, camera))], casterMaterial_, MatrixIdentity());
    }
    rlEnableBackfaceCulling();
}

void Terrain3D::drawGround(const Camera3D& camera, float snowAmount, Vector3 foliage, float year,
                           const SceneShading& shading) {
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
        const int wanted = levelFor(tile, camera);
        tile.level = wanted;
        if (!tile.built[wanted] && builds < kBuildsPerFrame) {
            build(tile, wanted);
            ++builds;
        }
        const int level = shownLevel(tile, camera);
        tile.lastUsed[level] = frame_;
        visible_.push_back({static_cast<int>(index), level});
    }

    tilesDrawn_ = static_cast<int>(visible_.size());
    trianglesDrawn_ = 0;
    shading.apply(groundShader_);
    const float snow = std::clamp(snowAmount, 0.0f, 1.0f);
    SetShaderValue(groundShader_, snowLoc_, &snow, SHADER_UNIFORM_FLOAT);
    SetShaderValue(groundShader_, foliageLoc_, &foliage, SHADER_UNIFORM_VEC3);
    SetShaderValue(groundShader_, yearLoc_, &year, SHADER_UNIFORM_FLOAT);
    // Юбки видны с обеих сторон; снизу на землю не смотрят, так что отсечение не нужно.
    rlDisableBackfaceCulling();
    for (const Visible& visible : visible_) {
        const Tile& tile = tiles_[static_cast<std::size_t>(visible.tile)];
        const MapRenderer::GroundChunk chunk = textures_.groundChunk(tile.left, tile.top);
        groundMaterial_.maps[MATERIAL_MAP_DIFFUSE].texture = chunk.texture;
        groundMaterial_.maps[MATERIAL_MAP_SPECULAR].texture = chunk.snow;
        groundMaterial_.maps[MATERIAL_MAP_NORMAL].texture = chunk.forest;
        const Mesh& mesh = tile.ground[static_cast<std::size_t>(visible.level)];
        DrawMesh(mesh, groundMaterial_, MatrixIdentity());
        trianglesDrawn_ += mesh.triangleCount;
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

void Terrain3D::drawWater(float time, Vector2 wind, const SceneShading& shading) {
    // Всё непрозрачное, накопленное в батче rlgl, — раньше воды: сквозь воду
    // видно то, что под ней.
    rlDrawRenderBatchActive();
    // Отражается небо чуть выше горизонта.
    const SceneLight& light = shading.light();
    const Vector3 sky = Vector3Lerp(light.horizon, light.zenith, 0.3f);
    shading.apply(waterShader_);
    SetShaderValue(waterShader_, timeLoc_, &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(waterShader_, skyLoc_, &sky, SHADER_UNIFORM_VEC3);
    SetShaderValue(waterShader_, windLoc_, &wind, SHADER_UNIFORM_VEC2);
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
