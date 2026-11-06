#include "render/Props3D.h"

#include "render/FieldLayout.h"
#include "render/LandUse.h"
#include "render/Lighting3D.h"
#include "render/Terrain3D.h"
#include "sim/Map.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <type_traits>

namespace lw {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr int kMaxMeshVertices = 65535;
// Время на постройку сеток за кадр (только процессор), секунды.
constexpr double kBuildBudget = 0.003;
constexpr std::uint32_t kEvictFrames = 600;
// Самое высокое — столбы: запас границ чанка вверх, метры.
constexpr float kTallest = 10.0f;

// Части — texcoord.x вершины; шейдер обходится с ними по-своему.
constexpr float kSolid = 0.0f;  // дерево, камень, металл
constexpr float kGrass = 1.0f;  // травинка: цвет и высота по сезону
constexpr float kFlower = 2.0f; // цветок: только в своё время цветения
constexpr float kCrop = 3.0f;   // ряд посева (cropState)
constexpr float kBale = 4.0f;   // тюк или стог: после уборки
constexpr float kReed = 5.0f;   // камыш, осока
constexpr float kLitter = 7.0f; // мусор: при запустении не меньше порога
constexpr float kFence = 8.0f;  // полотно забора: вырезы по виду забора
constexpr float kWire = 9.0f;   // провод: лента к камере, не тоньше пикселя

// Стог виден с середины лета до весны, стог у сарая — всегда.
constexpr float kHaystackParam = 20.0f;
constexpr float kBarnStackParam = 21.0f;

constexpr Color kWood = {118, 92, 66, 255};
constexpr Color kGreyWood = {124, 116, 102, 255};
constexpr Color kDarkWood = {84, 66, 50, 255};
constexpr Color kStone = {128, 124, 118, 255};
constexpr Color kHay = {176, 156, 96, 255};
constexpr Color kStraw = {188, 168, 108, 255};
constexpr Color kSoil = {92, 72, 54, 255};
constexpr Color kWireColor = {40, 40, 44, 255};
constexpr Color kInsulator = {214, 214, 206, 255};

constexpr const char* kPropsVs = R"(
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform vec3 viewPos;
uniform float year;
uniform float snow;
uniform float time;
uniform vec2 wind;
uniform float range;
uniform float decay;
uniform float pixelAngle;
out vec3 fragPosition;
out vec4 fragColor;
out vec2 fragLocal;
out float fragCover;
flat out int fragPart;
flat out vec3 fragInfo; // x — параметр (срок, вид забора), y — культура, z — видимость по расстоянию

const int PART_SOLID = 0;
const int PART_GRASS = 1;
const int PART_FLOWER = 2;
const int PART_CROP = 3;
const int PART_BALE = 4;
const int PART_REED = 5;
const int PART_LITTER = 7;
const int PART_FENCE = 8;
const int PART_WIRE = 9;

void main() {
    int part = int(vertexTexCoord.x + 0.5);
    float param = vertexTexCoord.y;
    float share = vertexTexCoord2.x; // доля высоты (у провода — сторона ленты)
    vec3 anchor = part == PART_WIRE ? vertexPosition : vertexNormal;
    vec3 p = vertexPosition;
    vec4 color = vertexColor;
    float kind = floor(vertexColor.a * 255.0 + 0.5);
    bool hidden = false;
    fragCover = 1.0;
    // Ветер качает верхушки, каждую — вразнобой.
    float speed = length(wind);
    vec2 along = speed > 0.01 ? wind / speed : vec2(1.0, 0.0);
    float phase = dot(anchor.xz, vec2(0.37, 0.29));
    float sway = (0.6 + 0.4 * sin(time * 2.2 + phase)) * (0.05 + 0.03 * speed);
    // Растения у края дальности сначала оседают в землю (там их продолжает
    // шейдер земли), а растворяются только в самом конце.
    float away = length(anchor - viewPos);
    bool plant = part == PART_GRASS || part == PART_FLOWER || part == PART_CROP || part == PART_REED;
    float settle = plant ? 1.0 - smoothstep(0.5 * range, 0.92 * range, away) : 1.0;
    float keep = 1.0 - smoothstep(plant ? 0.9 * range : 0.75 * range, range, away);
    if (part == PART_GRASS) {
        float h = meadowHeight(year) * (1.0 - 0.85 * snow) * settle;
        p.y = anchor.y + (p.y - anchor.y) * h;
        p.xz += along * sway * share * share * (p.y - anchor.y);
        color.rgb = meadowColor(year, color.a) * (color.rgb * 2.0) * (0.72 + 0.4 * share);
    } else if (part == PART_FLOWER) {
        float h = meadowHeight(year) * (1.0 - 0.85 * snow) * settle;
        p.y = anchor.y + (p.y - anchor.y) * h;
        p.xz += along * sway * (p.y - anchor.y);
        // Весенние (param < 0,5) и летние цветы: у каждого свой срок.
        float start = param < 0.5 ? 0.09 + 0.12 * param : 0.22 + 0.24 * (param - 0.5);
        hidden = year < start || year > start + 0.1 || snow > 0.3;
    } else if (part == PART_CROP) {
        Crop c = cropState(int(kind), year, param);
        float h = c.h * (1.0 - 0.9 * snow) * settle;
        hidden = h < 0.015;
        p.y += share * h;
        p.xz += along * sway * share * h * 0.6;
        color.rgb = c.plant * (color.rgb * 2.0) * (0.72 + 0.4 * share);
    } else if (part == PART_BALE) {
        if (param < 19.5) {
            // Рулоны лежат на стерне: с уборки до пахоты.
            hidden = cropState(int(param + 0.5), year, 0.5).stubble < 0.5;
        } else if (param < 20.5) {
            hidden = year > 0.18 && year < 0.36;
        }
    } else if (part == PART_REED) {
        float broken = mix(1.0, 0.6, ramp(0.70, 0.80, year)) * (1.0 - 0.4 * snow) * settle;
        p.y = anchor.y + (p.y - anchor.y) * broken;
        p.xz += along * sway * share * share * (p.y - anchor.y) * 0.8;
        vec3 straw = vec3(0.68, 0.60, 0.40);
        vec3 reed = mix(straw, vec3(0.30, 0.45, 0.20), ramp(0.08, 0.20, year));
        reed = mix(reed, straw, ramp(0.50, 0.62, year));
        color.rgb = reed * (color.rgb * 2.0) * (0.75 + 0.35 * share);
    } else if (part == PART_LITTER) {
        hidden = decay < param;
    } else if (part == PART_WIRE) {
#ifdef SHADOW_PASS
        hidden = true; // провод тоньше текселя тени: в карте теней лёг бы широкой полосой
#endif
        // Лента провода повёрнута к камере и не тоньше полутора пикселей;
        // шире настоящей — реже (fragCover): вдали провод тает, а не мерцает.
        vec3 toEye = viewPos - p;
        float d = length(toEye);
        vec3 side = normalize(cross(vertexNormal, toEye));
        float width = max(0.025, 1.5 * d * pixelAngle);
        p += side * share * 0.5 * width;
        fragCover = 0.025 / width * 1.6;
    }
    fragPosition = p;
    fragColor = color;
    fragLocal = vec2(share, vertexTexCoord2.y);
    fragPart = part;
    fragInfo = vec3(param, kind, keep);
    gl_Position = hidden || keep <= 0.0 ? vec4(0.0, 0.0, -2.0, 1.0) : mvp * vec4(p, 1.0);
}
)";

constexpr const char* kPropsFs = R"(
in vec3 fragPosition;
in vec4 fragColor;
in vec2 fragLocal;
in float fragCover;
flat in int fragPart;
flat in vec3 fragInfo;
uniform float snow;
uniform float year;
out vec4 finalColor;

const int PART_GRASS = 1;
const int PART_FLOWER = 2;
const int PART_CROP = 3;
const int PART_REED = 5;
const int PART_FENCE = 8;
const int PART_WIRE = 9;

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main() {
    ivec2 cell = ivec2(mod(gl_FragCoord.xy, 4.0));
    float threshold = (kBayer[cell.y * 4 + cell.x] + 0.5) / 16.0;
    if (threshold >= fragInfo.z) {
        discard;
    }
    float v = fragLocal.x;
    float u = fragLocal.y;
    float px = max(fwidth(u), 1e-5); // метров на пиксель вдоль ряда или забора
    vec3 n = normalize(cross(dFdx(fragPosition), dFdy(fragPosition)));
    if (dot(n, viewPos - fragPosition) < 0.0) {
        n = -n;
    }
    vec3 base = fragColor.rgb;
    int part = fragPart;
    if (part == PART_WIRE) {
        // Провод сплошной, пока лента шире настоящего не больше чем впятеро,
        // дальше его нет: редкие точки по экранному узору рябили.
        if (fragCover < 0.3) {
            discard;
        }
    } else if (part == PART_CROP) {
        // Верх ряда неровный: растения разной высоты; картофель — кустами,
        // кукуруза и подсолнечник — отдельными стеблями.
        int kind = int(fragInfo.y + 0.5);
        float top = 1.0 - 0.25 * hashUnit(hash3(int(floor(u * 9.0)), int(fragInfo.x * 977.0), 3u));
        if (kind == CROP_POTATO || kind == CROP_GARDEN) {
            float hump = 2.0 * fract(u / 0.42) - 1.0;
            top = 0.45 + 0.55 * sqrt(max(0.0, 1.0 - hump * hump));
        } else if (kind == CROP_CORN || kind == CROP_SUNFLOWER) {
            float stem = abs(fract(u / 0.32) - 0.5);
            if (v < 0.55 && stem > 0.16 && px < 0.04) {
                discard;
            }
            top = 1.0 - 0.12 * hashUnit(hash3(int(floor(u / 0.32)), 5, 3u));
            if (kind == CROP_SUNFLOWER && v > top - 0.14 && stem < 0.22) {
                base = sunflowerHead(year, fragInfo.x);
            }
        } else if (v > top - 0.2) {
            base *= 1.12; // колосья светлее
        }
        if (v > top) {
            discard;
        }
    } else if (part == PART_FENCE) {
        int style = int(fragInfo.x + 0.5);
        float fine = 1.0 - smoothstep(0.004, 0.012, px); // узор крупнее пикселя
        if (style == 0) {
            // Штакетник: планки с заострённым верхом на двух прожилинах. Просвет
            // уже трёх пикселей при движении то попадал бы в пиксель, то нет —
            // забор рябил бы: тогда он сплошной и темнее, как планки с просветами
            // в среднем (экранный узор из точек тоже рябит).
            float slat = fract(u / 0.13);
            bool rail = (v > 0.22 && v < 0.30) || (v > 0.70 && v < 0.78);
            bool gaps = 0.13 * 0.4 / px > 3.0;
            if (gaps && v > 1.0 - 0.12 * abs(slat - 0.3) / 0.7) {
                discard;
            }
            if (!rail) {
                if (gaps) {
                    if (slat > 0.6) {
                        discard;
                    }
                } else {
                    base *= 0.78;
                }
            }
            base *= 0.9 + 0.1 * hashUnit(hash3(int(floor(u / 0.13)), 1, 9u));
        } else if (style == 1) {
            // Плетень: прутья вплетены волной, верх неровный.
            float weave = sin(v * 70.0 + 3.14159 * floor(u * 3.0));
            base *= 0.84 + 0.18 * weave * fine;
            if (v > 0.93 + 0.07 * hashUnit(hash3(int(floor(u * 20.0)), 2, 9u))) {
                discard;
            }
        } else if (style == 2) {
            // Дощатый: доски разного тона, щели между ними.
            float board = fract(u / 0.16);
            base *= (0.88 + 0.12 * hashUnit(hash3(int(floor(u / 0.16)), 3, 9u))) *
                    mix(1.0, 0.7, (1.0 - smoothstep(0.0, 0.06, board)) * fine);
        } else {
            // Жерди: три перекладины.
            if (!((v > 0.30 && v < 0.40) || (v > 0.62 && v < 0.72) || v > 0.90)) {
                discard;
            }
        }
    }
    bool plant = part == PART_GRASS || part == PART_FLOWER || part == PART_CROP || part == PART_REED;
    if (plant) {
        n = normalize(mix(n, vec3(0.0, 1.0, 0.0), 0.65));
    }
#ifdef SHADOW_PASS
    finalColor = vec4(1.0);
#else
    if (!plant && snow > 0.0) {
        base = mix(base, vec3(0.90, 0.92, 0.96), 0.9 * snow * smoothstep(0.35, 0.75, n.y));
    }
    vec3 color = base * lighting(n, shadowAt(fragPosition, n));
    finalColor = vec4(withFog(color, fragPosition), 1.0);
#endif
}
)";

// Случайные числа предмета: из хеша клетки.
class Random {
public:
    explicit Random(std::uint32_t seed) : state_(seed * 2654435761u + 0x9E3779B9u) {}
    float next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(state_ >> 8) / static_cast<float>(1u << 24);
    }
    float signedNext() { return next() * 2.0f - 1.0f; }
    float range(float a, float b) { return a + (b - a) * next(); }
    std::uint32_t bits() {
        next();
        return state_;
    }

private:
    std::uint32_t state_;
};

std::uint32_t cellSeed(Cell cell, std::uint32_t salt) {
    return fieldHash(cell.x, cell.y, salt + 2000u);
}

Color vary(Color color, Random& random, float amount) {
    const float k = 1.0f + amount * random.signedNext();
    const auto channel = [k](unsigned char value) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(value) * k, 0.0f, 255.0f));
    };
    return {channel(color.r), channel(color.g), channel(color.b), color.a};
}

// Сетки мелочи: вершина — место, опора предмета (в нормали: от неё — растворение
// по расстоянию), часть и параметр (texcoord), доля высоты и координата вдоль
// (texcoord2), цвет. Индексы 16-битные: полная сетка уходит в список и
// начинается новая.
class PropBuilder {
public:
    int items = 0; // предметов в сетках

    int count() const { return static_cast<int>(positions_.size() / 3); }
    void setAnchor(Vector3 anchor) { anchor_ = anchor; }
    // Место под предмет из vertices вершин: не хватает — начать новую сетку.
    void reserve(int vertices) {
        if (count() + vertices > kMaxMeshVertices) {
            flush();
        }
    }
    int vertex(Vector3 p, float part, float param, float share, float u, Color color) {
        positions_.insert(positions_.end(), {p.x, p.y, p.z});
        anchors_.insert(anchors_.end(), {anchor_.x, anchor_.y, anchor_.z});
        texcoords_.insert(texcoords_.end(), {part, param});
        texcoords2_.insert(texcoords2_.end(), {share, u});
        colors_.insert(colors_.end(), {color.r, color.g, color.b, color.a});
        return count() - 1;
    }
    // Провод: в «опоре» — направление провода.
    int wireVertex(Vector3 p, Vector3 direction, float side) {
        positions_.insert(positions_.end(), {p.x, p.y, p.z});
        anchors_.insert(anchors_.end(), {direction.x, direction.y, direction.z});
        texcoords_.insert(texcoords_.end(), {kWire, 0.0f});
        texcoords2_.insert(texcoords2_.end(), {side, 0.0f});
        colors_.insert(colors_.end(), {kWireColor.r, kWireColor.g, kWireColor.b, 255});
        return count() - 1;
    }
    void triangle(int a, int b, int c) {
        indices_.insert(indices_.end(), {static_cast<unsigned short>(a), static_cast<unsigned short>(b),
                                         static_cast<unsigned short>(c)});
    }
    void quad(int a, int b, int c, int d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }

    // Коробка без дна: основание в center (y — низ), вдоль (dx, dz) — полудлина
    // halfLength, поперёк — полуширина halfWidth, высота height.
    void box(Vector3 center, float dx, float dz, float halfLength, float halfWidth, float height, Color color,
             float part = kSolid, float param = 0.0f, Color top = {0, 0, 0, 0}) {
        reserve(8);
        const float sx = -dz;
        const float sz = dx;
        int v[8];
        for (int k = 0; k < 8; ++k) {
            const float a = (k & 1) != 0 ? halfLength : -halfLength;
            const float b = (k & 2) != 0 ? halfWidth : -halfWidth;
            const float y = (k & 4) != 0 ? height : 0.0f;
            const Color c = (k & 4) != 0 && top.a != 0 ? top : color;
            v[k] = vertex({center.x + dx * a + sx * b, center.y + y, center.z + dz * a + sz * b}, part, param,
                          (k & 4) != 0 ? 1.0f : 0.0f, 0.0f, c);
        }
        quad(v[4], v[5], v[7], v[6]); // верх
        quad(v[0], v[1], v[5], v[4]);
        quad(v[2], v[6], v[7], v[3]);
        quad(v[0], v[4], v[6], v[2]);
        quad(v[1], v[3], v[7], v[5]);
    }

    // Вертикальная призма: кольцо радиуса r0 внизу и r1 вверху.
    void prism(Vector3 base, float r0, float r1, float height, int sides, Color color, float part = kSolid,
               float param = 0.0f) {
        reserve(2 * sides + 1);
        const int first = count();
        for (int k = 0; k < sides; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            vertex({base.x + r0 * std::cos(a), base.y, base.z + r0 * std::sin(a)}, part, param, 0.0f, 0.0f, color);
            vertex({base.x + r1 * std::cos(a), base.y + height, base.z + r1 * std::sin(a)}, part, param, 1.0f, 0.0f,
                   color);
        }
        const int cap = vertex({base.x, base.y + height, base.z}, part, param, 1.0f, 0.0f, color);
        for (int k = 0; k < sides; ++k) {
            const int a0 = first + 2 * k;
            const int a1 = first + 2 * ((k + 1) % sides);
            quad(a0, a1, a1 + 1, a0 + 1);
            triangle(a0 + 1, a1 + 1, cap);
        }
    }

    // Стог: купол из колец (радиус, высота) с верхушкой.
    void dome(Vector3 base, const float (*rings)[2], int ringCount, int sides, Color color, float part,
              float param, Random& random) {
        reserve(ringCount * sides + 1);
        const int first = count();
        for (int r = 0; r < ringCount; ++r) {
            for (int k = 0; k < sides; ++k) {
                const float a = 2.0f * kPi * (static_cast<float>(k) + 0.5f * static_cast<float>(r)) /
                                static_cast<float>(sides);
                const float radius = rings[r][0] * (1.0f + 0.06f * random.signedNext());
                vertex({base.x + radius * std::cos(a), base.y + rings[r][1], base.z + radius * std::sin(a)}, part,
                       param, static_cast<float>(r) / static_cast<float>(ringCount), 0.0f,
                       vary(color, random, 0.06f));
            }
        }
        const int apex = vertex({base.x, base.y + rings[ringCount - 1][1] + 0.4f, base.z}, part, param, 1.0f, 0.0f,
                                color);
        for (int r = 0; r + 1 < ringCount; ++r) {
            for (int k = 0; k < sides; ++k) {
                const int a0 = first + r * sides + k;
                const int a1 = first + r * sides + (k + 1) % sides;
                quad(a0, a1, a1 + sides, a0 + sides);
            }
        }
        for (int k = 0; k < sides; ++k) {
            triangle(first + (ringCount - 1) * sides + k, first + (ringCount - 1) * sides + (k + 1) % sides, apex);
        }
    }

    // Лежащий цилиндр (рулон соломы, покрышка): ось вдоль (dx, dz).
    void roll(Vector3 center, float dx, float dz, float radius, float halfLength, int sides, Color side, Color end,
              float part, float param) {
        reserve(4 * sides + 2);
        const float sx = -dz;
        const float sz = dx;
        const int first = count();
        for (int k = 0; k < sides; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float across = radius * std::cos(a);
            const float up = radius * std::sin(a);
            for (const float s : {-1.0f, 1.0f}) {
                vertex({center.x + dx * halfLength * s + sx * across, center.y + up,
                        center.z + dz * halfLength * s + sz * across},
                       part, param, 0.5f, 0.0f, side);
            }
        }
        const int ends = count();
        for (int k = 0; k < sides; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float across = radius * std::cos(a);
            const float up = radius * std::sin(a);
            for (const float s : {-1.0f, 1.0f}) {
                vertex({center.x + dx * halfLength * s + sx * across, center.y + up,
                        center.z + dz * halfLength * s + sz * across},
                       part, param, 0.5f, 0.0f, end);
            }
        }
        const int c0 = vertex({center.x - dx * halfLength, center.y, center.z - dz * halfLength}, part, param, 0.5f,
                              0.0f, end);
        const int c1 = vertex({center.x + dx * halfLength, center.y, center.z + dz * halfLength}, part, param, 0.5f,
                              0.0f, end);
        for (int k = 0; k < sides; ++k) {
            const int a = first + 2 * k;
            const int b = first + 2 * ((k + 1) % sides);
            quad(a, b, b + 1, a + 1);
            triangle(c0, ends + 2 * ((k + 1) % sides), ends + 2 * k);
            triangle(c1, ends + 2 * k + 1, ends + 2 * ((k + 1) % sides) + 1);
        }
    }

    // Валун: неровный восьмигранник, на треть в земле.
    void boulder(Vector3 base, float size, Random& random, float part = kSolid, float param = 0.0f,
                 Color color = kStone) {
        reserve(6);
        const float yaw = random.next() * 2.0f * kPi;
        int ring[4];
        for (int k = 0; k < 4; ++k) {
            const float a = yaw + 0.5f * kPi * static_cast<float>(k);
            const float r = size * random.range(0.75f, 1.15f);
            ring[k] = vertex({base.x + r * std::cos(a), base.y + size * random.range(0.05f, 0.25f),
                              base.z + r * std::sin(a)},
                             part, param, 0.3f, 0.0f, vary(color, random, 0.12f));
        }
        const int top = vertex({base.x + 0.2f * size * random.signedNext(), base.y + size * random.range(0.6f, 0.9f),
                                base.z + 0.2f * size * random.signedNext()},
                               part, param, 1.0f, 0.0f, vary(color, random, 0.1f));
        const int bottom = vertex({base.x, base.y - 0.5f * size, base.z}, part, param, 0.0f, 0.0f, color);
        for (int k = 0; k < 4; ++k) {
            triangle(ring[k], ring[(k + 1) % 4], top);
            triangle(ring[(k + 1) % 4], ring[k], bottom);
        }
    }

    std::vector<Mesh> finish() {
        flush();
        return std::move(meshes_);
    }

private:
    void flush() {
        if (indices_.empty()) {
            clear();
            return;
        }
        Mesh mesh{};
        mesh.vertexCount = count();
        mesh.triangleCount = static_cast<int>(indices_.size() / 3);
        const auto copy = [](const auto& source) {
            using T = typename std::decay_t<decltype(source)>::value_type;
            auto* target = static_cast<T*>(MemAlloc(static_cast<unsigned>(source.size() * sizeof(T))));
            std::copy(source.begin(), source.end(), target);
            return target;
        };
        mesh.vertices = copy(positions_);
        mesh.normals = copy(anchors_);
        mesh.texcoords = copy(texcoords_);
        mesh.texcoords2 = copy(texcoords2_);
        mesh.colors = copy(colors_);
        mesh.indices = copy(indices_);
        UploadMesh(&mesh, false);
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        MemFree(mesh.texcoords);
        MemFree(mesh.texcoords2);
        MemFree(mesh.colors);
        mesh.vertices = nullptr;
        mesh.normals = nullptr;
        mesh.texcoords = nullptr;
        mesh.texcoords2 = nullptr;
        mesh.colors = nullptr;
        meshes_.push_back(mesh);
        clear();
    }
    void clear() {
        positions_.clear();
        anchors_.clear();
        texcoords_.clear();
        texcoords2_.clear();
        colors_.clear();
        indices_.clear();
    }

    Vector3 anchor_{};
    std::vector<float> positions_;
    std::vector<float> anchors_;
    std::vector<float> texcoords_;
    std::vector<float> texcoords2_;
    std::vector<unsigned char> colors_;
    std::vector<unsigned short> indices_;
    std::vector<Mesh> meshes_;
};

float distanceToBox(Vector3 p, Vector3 min, Vector3 max) {
    const float dx = std::max({min.x - p.x, 0.0f, p.x - max.x});
    const float dy = std::max({min.y - p.y, 0.0f, p.y - max.y});
    const float dz = std::max({min.z - p.z, 0.0f, p.z - max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool freshWater(Water water) {
    return water == Water::River || water == Water::Lake;
}

// --- Мелкое: трава, цветы, посевы, камыш, грядки, мусор ----------------------

struct Context {
    const Map& map;
    const Terrain3D& terrain;
    const LandUse& land;
    PropBuilder& b;
};

// Пучок травы: травинки-треугольники веером. color.a — пятно луга (сухие пятна
// к концу лета), rgb — оттенок (×2 в шейдере).
void emitTuft(Context& c, float x, float z, float height, Color tint, float patch, Random& random,
              bool flowers) {
    const float y = c.terrain.groundAt(x, z);
    c.b.setAnchor({x, y, z});
    const int blades = 5;
    c.b.reserve(blades * 3 + 8);
    const float yaw = random.next() * 2.0f * kPi;
    const Color color = {tint.r, tint.g, tint.b, static_cast<unsigned char>(std::lround(patch * 255.0f))};
    for (int i = 0; i < blades; ++i) {
        const float a = yaw + 2.0f * kPi * static_cast<float>(i) / static_cast<float>(blades) + 0.4f * random.signedNext();
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        const float h = height * random.range(0.65f, 1.1f);
        const float lean = h * random.range(0.15f, 0.4f);
        const float w = random.range(0.03f, 0.05f);
        const float bx = x + 0.05f * ca;
        const float bz = z + 0.05f * sa;
        const int l = c.b.vertex({bx - sa * w, y, bz + ca * w}, kGrass, 0.0f, 0.0f, 0.0f, color);
        const int r = c.b.vertex({bx + sa * w, y, bz - ca * w}, kGrass, 0.0f, 0.0f, 0.0f, color);
        const int t = c.b.vertex({bx + ca * lean, y + h, bz + sa * lean}, kGrass, 0.0f, 1.0f, 0.0f, color);
        c.b.triangle(l, r, t);
    }
    if (flowers) {
        // Цветок: стебелёк и венчик над травой.
        constexpr Color kPetals[] = {{240, 240, 228, 255}, {240, 214, 60, 255}, {116, 140, 236, 255},
                                     {176, 104, 214, 255}, {226, 54, 40, 255}};
        const float season = random.next();
        const Color petals = kPetals[static_cast<int>(random.next() * 4.99f)];
        const float h = height * random.range(0.9f, 1.3f);
        const float fx = x + 0.1f * random.signedNext();
        const float fz = z + 0.1f * random.signedNext();
        const float s = 0.05f;
        const int a = c.b.vertex({fx - s, y + h, fz - s}, kFlower, season, 1.0f, 0.0f, petals);
        const int bb = c.b.vertex({fx + s, y + h + 0.01f, fz - s}, kFlower, season, 1.0f, 0.0f, petals);
        const int cc = c.b.vertex({fx + s, y + h, fz + s}, kFlower, season, 1.0f, 0.0f, petals);
        const int d = c.b.vertex({fx - s, y + h + 0.01f, fz + s}, kFlower, season, 1.0f, 0.0f, petals);
        c.b.quad(a, bb, cc, d);
        const Color stem = {70, 110, 50, 255};
        const int s0 = c.b.vertex({fx - 0.01f, y, fz}, kFlower, season, 0.0f, 0.0f, stem);
        const int s1 = c.b.vertex({fx + 0.01f, y, fz}, kFlower, season, 0.0f, 0.0f, stem);
        const int s2 = c.b.vertex({fx, y + h, fz}, kFlower, season, 1.0f, 0.0f, stem);
        c.b.triangle(s0, s1, s2);
    }
    c.b.items++;
}

// Камыш (высокий, с початками) или осока (ниже, гуще): пучок у воды.
void emitReeds(Context& c, float x, float z, bool cattails, Random& random) {
    const float y = c.terrain.groundAt(x, z);
    c.b.setAnchor({x, y, z});
    const int blades = cattails ? 9 : 7;
    c.b.reserve(blades * 3 + 32);
    const float height = cattails ? random.range(1.4f, 2.2f) : random.range(0.5f, 0.8f);
    const Color tint = cattails ? Color{128, 128, 128, 255} : Color{104, 118, 100, 255};
    for (int i = 0; i < blades; ++i) {
        const float ox = x + 0.35f * random.signedNext();
        const float oz = z + 0.35f * random.signedNext();
        const float a = random.next() * 2.0f * kPi;
        const float h = height * random.range(0.7f, 1.05f);
        const float lean = h * random.range(0.02f, 0.15f);
        const float w = cattails ? 0.025f : 0.035f;
        const int l = c.b.vertex({ox - std::sin(a) * w, y, oz + std::cos(a) * w}, kReed, 0.0f, 0.0f, 0.0f, tint);
        const int r = c.b.vertex({ox + std::sin(a) * w, y, oz - std::cos(a) * w}, kReed, 0.0f, 0.0f, 0.0f, tint);
        const int t = c.b.vertex({ox + std::cos(a) * lean, y + h, oz + std::sin(a) * lean}, kReed, 0.0f, 1.0f, 0.0f,
                                 tint);
        c.b.triangle(l, r, t);
    }
    if (cattails) {
        for (int i = 0; i < 2; ++i) {
            const float ox = x + 0.25f * random.signedNext();
            const float oz = z + 0.25f * random.signedNext();
            const float h = height * random.range(0.8f, 0.95f);
            c.b.box({ox, y + h - 0.2f, oz}, 1.0f, 0.0f, 0.025f, 0.025f, 0.2f, {86, 58, 36, 255});
        }
    }
    c.b.items++;
}

// Ряды посева в клетке поля: по одному полотну на ряд, кусками до 2 м; у
// границы лоскута (межа) и на чужом лоскуте рядов нет.
void emitCrops(Context& c, Cell cell) {
    const float x0 = static_cast<float>(cell.x) * Map::kCellSize;
    const float z0 = static_cast<float>(cell.y) * Map::kCellSize;
    const float x1 = x0 + Map::kCellSize;
    const float z1 = z0 + Map::kCellSize;
    // Лоскуты в клетке: по углам и середине.
    FieldPatch patches[5];
    int count = 0;
    const float samples[5][2] = {{x0 + 0.1f, z0 + 0.1f}, {x1 - 0.1f, z0 + 0.1f}, {x0 + 0.1f, z1 - 0.1f},
                                 {x1 - 0.1f, z1 - 0.1f}, {x0 + 2.0f, z0 + 2.0f}};
    bool uniform = true;
    for (const auto& sample : samples) {
        const FieldPatch patch = fieldPatchAt(sample[0], sample[1]);
        bool known = false;
        for (int i = 0; i < count; ++i) {
            known = known || patches[i].id == patch.id;
        }
        if (!known) {
            patches[count++] = patch;
        }
    }
    uniform = count == 1 && fieldPatchAt(x0 + 2.0f, z0 + 2.0f).edge > kBalkWidth + 3.0f;
    Random random(cellSeed(cell, 5u));
    for (int i = 0; i < count; ++i) {
        const FieldPatch& patch = patches[i];
        if (patch.crop == CropKind::Fallow) {
            continue;
        }
        const float spacing = rowSpacing(patch.crop);
        float nx = 0.0f;
        float nz = 0.0f;
        rowNormal(patch.angle, nx, nz);
        const float dx = nz; // вдоль ряда
        const float dz = -nx;
        float rmin = 1.0e9f;
        float rmax = -1.0e9f;
        for (const float cx : {x0, x1}) {
            for (const float cz : {z0, z1}) {
                const float r = (cx * nx + cz * nz) / spacing;
                rmin = std::min(rmin, r);
                rmax = std::max(rmax, r);
            }
        }
        for (int k = static_cast<int>(std::ceil(rmin - 0.5f)); k <= static_cast<int>(std::floor(rmax - 0.5f)); ++k) {
            const float offset = (static_cast<float>(k) + 0.5f) * spacing;
            const float px = nx * offset;
            const float pz = nz * offset;
            // Отрезок прямой p + d·t внутри квадрата клетки.
            float t0 = -1.0e9f;
            float t1 = 1.0e9f;
            const auto slab = [&](float p, float d, float lo, float hi) {
                if (std::abs(d) < 1e-6f) {
                    if (p < lo || p > hi) {
                        t1 = -1.0e9f;
                    }
                    return;
                }
                const float a = (lo - p) / d;
                const float b = (hi - p) / d;
                t0 = std::max(t0, std::min(a, b));
                t1 = std::min(t1, std::max(a, b));
            };
            slab(px, dx, x0, x1);
            slab(pz, dz, z0, z1);
            if (t1 - t0 < 0.05f) {
                continue;
            }
            const int pieces = std::max(1, static_cast<int>(std::ceil((t1 - t0) / 2.0f)));
            for (int piece = 0; piece < pieces; ++piece) {
                const float ta = t0 + (t1 - t0) * static_cast<float>(piece) / static_cast<float>(pieces);
                const float tb = t0 + (t1 - t0) * static_cast<float>(piece + 1) / static_cast<float>(pieces);
                const float mx = px + dx * 0.5f * (ta + tb);
                const float mz = pz + dz * 0.5f * (ta + tb);
                if (!uniform) {
                    const FieldPatch here = fieldPatchAt(mx, mz);
                    if (here.id != patch.id || here.edge < kBalkWidth) {
                        continue;
                    }
                }
                const Vector3 a = {px + dx * ta, 0.0f, pz + dz * ta};
                const Vector3 b = {px + dx * tb, 0.0f, pz + dz * tb};
                const float ya = c.terrain.groundAt(a.x, a.z);
                const float yb = c.terrain.groundAt(b.x, b.z);
                c.b.setAnchor({mx, 0.5f * (ya + yb), mz});
                c.b.reserve(4);
                Color tint = vary({128, 128, 128, 255}, random, 0.08f);
                tint.a = static_cast<unsigned char>(patch.crop);
                const int v0 = c.b.vertex({a.x, ya, a.z}, kCrop, patch.timing, 0.0f, ta, tint);
                const int v1 = c.b.vertex({b.x, yb, b.z}, kCrop, patch.timing, 0.0f, tb, tint);
                const int v2 = c.b.vertex({b.x, yb, b.z}, kCrop, patch.timing, 1.0f, tb, tint);
                const int v3 = c.b.vertex({a.x, ya, a.z}, kCrop, patch.timing, 1.0f, ta, tint);
                c.b.quad(v0, v1, v2, v3);
                c.b.items++;
            }
        }
    }
}

// Огород: грядки вдоль двора — земля горкой и ряд растений на каждой.
void emitGarden(Context& c, Cell cell) {
    const int lot = c.land.lotOf(cell);
    const std::uint32_t bits = lot > 0 ? c.land.lots()[static_cast<std::size_t>(lot - 1)].bits : 0u;
    const bool alongX = (bits & 1u) != 0u;
    const float timing = hashUnit(bits >> 13);
    const float x0 = static_cast<float>(cell.x) * Map::kCellSize;
    const float z0 = static_cast<float>(cell.y) * Map::kCellSize;
    for (int bed = 0; bed < 3; ++bed) {
        const float across = 0.75f + 1.25f * static_cast<float>(bed);
        const float cx = alongX ? x0 + 2.0f : x0 + across;
        const float cz = alongX ? z0 + across : z0 + 2.0f;
        const float dx = alongX ? 1.0f : 0.0f;
        const float dz = alongX ? 0.0f : 1.0f;
        const float y = c.terrain.groundAt(cx, cz);
        c.b.setAnchor({cx, y, cz});
        c.b.box({cx, y - 0.05f, cz}, dx, dz, 1.85f, 0.35f, 0.17f, kSoil);
        const float ax = cx - dx * 1.8f;
        const float az = cz - dz * 1.8f;
        const float bx = cx + dx * 1.8f;
        const float bz = cz + dz * 1.8f;
        c.b.reserve(4);
        Color tint = {128, 128, 128, static_cast<unsigned char>(CropKind::Garden)};
        const float u0 = alongX ? ax : az;
        const float u1 = alongX ? bx : bz;
        const int v0 = c.b.vertex({ax, y + 0.1f, az}, kCrop, timing, 0.0f, u0, tint);
        const int v1 = c.b.vertex({bx, y + 0.1f, bz}, kCrop, timing, 0.0f, u1, tint);
        const int v2 = c.b.vertex({bx, y + 0.1f, bz}, kCrop, timing, 1.0f, u1, tint);
        const int v3 = c.b.vertex({ax, y + 0.1f, az}, kCrop, timing, 1.0f, u0, tint);
        c.b.quad(v0, v1, v2, v3);
        c.b.items++;
    }
}

// Брошенное после коллапса: коробки, сумки, чемоданы, покрышки, бумаги.
// threshold — при каком запустении (0..1) вещь появляется.
void emitLitter(Context& c, float x, float z, Random& random) {
    const float y = c.terrain.groundAt(x, z);
    c.b.setAnchor({x, y, z});
    const float threshold = random.range(0.05f, 0.95f);
    const float a = random.next() * 2.0f * kPi;
    const float dx = std::cos(a);
    const float dz = std::sin(a);
    const int kind = static_cast<int>(random.next() * 4.99f);
    switch (kind) {
    case 0: { // коробка
        const float s = random.range(0.18f, 0.28f);
        c.b.box({x, y, z}, dx, dz, s, s * 0.8f, s * 1.4f, vary({158, 124, 82, 255}, random, 0.1f), kLitter,
                threshold);
        break;
    }
    case 1: { // сумка или мешок
        constexpr Color kBags[] = {{34, 34, 38, 255}, {52, 70, 110, 255}, {96, 46, 40, 255}};
        c.b.box({x, y, z}, dx, dz, 0.22f, 0.14f, 0.26f, kBags[static_cast<int>(random.next() * 2.99f)], kLitter,
                threshold);
        break;
    }
    case 2: { // чемодан
        constexpr Color kCases[] = {{120, 40, 36, 255}, {40, 60, 96, 255}, {60, 60, 60, 255}, {150, 120, 70, 255}};
        c.b.box({x, y, z}, dx, dz, 0.32f, 0.11f, 0.45f, kCases[static_cast<int>(random.next() * 3.99f)], kLitter,
                threshold);
        break;
    }
    case 3: // покрышка
        c.b.roll({x, y + 0.11f, z}, -dz, dx, 0.32f, 0.11f, 8, {30, 30, 32, 255}, {44, 44, 46, 255}, kLitter,
                 threshold);
        break;
    default: { // бумаги и тряпьё — плоско на земле
        const Color sheet = random.next() < 0.6f ? Color{226, 222, 210, 255} : vary({120, 96, 130, 255}, random, 0.3f);
        c.b.reserve(4);
        const float s = random.range(0.15f, 0.35f);
        const int v0 = c.b.vertex({x - dx * s + dz * s, y + 0.03f, z - dz * s - dx * s}, kLitter, threshold, 0.0f, 0.0f, sheet);
        const int v1 = c.b.vertex({x + dx * s + dz * s, y + 0.03f, z + dz * s - dx * s}, kLitter, threshold, 0.0f, 0.0f, sheet);
        const int v2 = c.b.vertex({x + dx * s - dz * s, y + 0.03f, z + dz * s + dx * s}, kLitter, threshold, 0.0f, 0.0f, sheet);
        const int v3 = c.b.vertex({x - dx * s - dz * s, y + 0.03f, z - dz * s + dx * s}, kLitter, threshold, 0.0f, 0.0f, sheet);
        c.b.quad(v0, v1, v2, v3);
        break;
    }
    }
    c.b.items++;
}

// Направление с клетки суши на соседнюю пресную воду; нет — false.
bool waterSide(const Map& map, Cell cell, int& dx, int& dy) {
    constexpr int kDx[] = {0, 0, -1, 1};
    constexpr int kDy[] = {-1, 1, 0, 0};
    for (int d = 0; d < 4; ++d) {
        const Cell next = {cell.x + kDx[d], cell.y + kDy[d]};
        if (map.contains(next) && freshWater(map.water.at(next.x, next.y))) {
            dx = kDx[d];
            dy = kDy[d];
            return true;
        }
    }
    return false;
}

void emitSmallCell(Context& c, Cell cell) {
    const Map& map = c.map;
    const float x0 = static_cast<float>(cell.x) * Map::kCellSize;
    const float z0 = static_cast<float>(cell.y) * Map::kCellSize;
    Random random(cellSeed(cell, 1u));
    const Water water = map.water.at(cell.x, cell.y);
    const Road road = map.road.at(cell.x, cell.y);
    const std::uint8_t flags = c.land.flags(cell);

    if (water != Water::None) {
        // Камыш на мелководье у берега.
        if (freshWater(water) && road == Road::None && map.depthMeters(cell) <= 0.6f) {
            int dx = 0;
            int dy = 0;
            bool shore = false;
            constexpr int kDx[] = {0, 0, -1, 1};
            constexpr int kDy[] = {-1, 1, 0, 0};
            for (int d = 0; d < 4 && !shore; ++d) {
                const Cell next = {cell.x + kDx[d], cell.y + kDy[d]};
                if (map.contains(next) && map.water.at(next.x, next.y) == Water::None) {
                    shore = true;
                    dx = kDx[d];
                    dy = kDy[d];
                }
            }
            if (shore && random.next() < 0.5f) {
                const float x = x0 + 2.0f + static_cast<float>(dx) * 1.3f + 0.6f * random.signedNext();
                const float z = z0 + 2.0f + static_cast<float>(dy) * 1.3f + 0.6f * random.signedNext();
                emitReeds(c, x, z, true, random);
            }
        }
        return;
    }
    if (road != Road::None) {
        // Мусор на улицах поселений и изредка на трассах.
        const float chance = (flags & LandUse::kSettled) != 0 ? 0.12f : 0.01f;
        if (random.next() < chance) {
            emitLitter(c, x0 + random.range(0.5f, 3.5f), z0 + random.range(0.5f, 3.5f), random);
        }
        return;
    }
    if (map.cover.at(cell.x, cell.y) == Cover::Forest || (flags & LandUse::kBuilding) != 0) {
        return;
    }
    const Ground ground = map.ground.at(cell.x, cell.y);
    if ((flags & LandUse::kGarden) != 0) {
        emitGarden(c, cell);
        return;
    }
    // Мусор у домов после коллапса.
    if ((flags & (LandUse::kYard | LandUse::kDoor)) != 0 && random.next() < 0.18f) {
        const int n = 1 + static_cast<int>(random.next() * 2.5f);
        for (int i = 0; i < n; ++i) {
            emitLitter(c, x0 + random.range(0.4f, 3.6f), z0 + random.range(0.4f, 3.6f), random);
        }
    }
    if (ground == Ground::Field) {
        emitCrops(c, cell);
        // На пару — трава, на меже — тоже.
        const FieldPatch patch = fieldPatchAt(x0 + 2.0f, z0 + 2.0f);
        const int tufts = patch.crop == CropKind::Fallow ? 4 : patch.edge < kBalkWidth + 2.0f ? 2 : 0;
        for (int i = 0; i < tufts; ++i) {
            const float x = x0 + random.range(0.2f, 3.8f);
            const float z = z0 + random.range(0.2f, 3.8f);
            if (patch.crop != CropKind::Fallow && fieldPatchAt(x, z).edge > kBalkWidth) {
                continue;
            }
            emitTuft(c, x, z, random.range(0.45f, 0.7f), vary({128, 128, 128, 255}, random, 0.1f), random.next(),
                     random, random.next() < 0.3f);
        }
        return;
    }
    if (ground == Ground::Rock) {
        return; // валуны — с крупным
    }

    // Луг, песок, двор, обочина, берег.
    const float wild = hashUnit(fieldHash(cell.x / 6, cell.y / 6, 3000u));
    int tufts = 3 + static_cast<int>(wild * 4.0f);
    float height = random.range(0.45f, 0.75f);
    float flowerChance = 0.2f + 0.2f * wild;
    Color tint = {128, 128, 128, 255};
    if ((flags & (LandUse::kYard | LandUse::kSettled)) != 0) {
        tufts = 2;
        height = random.range(0.18f, 0.3f); // у жилья трава вытоптана и скошена
        flowerChance = 0.1f;
    } else if ((flags & LandUse::kRoadside) != 0) {
        tufts = 5; // в канаве — высокая и сочная
        height = random.range(0.6f, 0.9f);
        tint = {116, 134, 116, 255};
    }
    if (ground == Ground::Sand) {
        tufts = 1;
        height = 0.35f;
        flowerChance = 0.0f;
        tint = {140, 140, 120, 255};
    }
    for (int i = 0; i < tufts; ++i) {
        emitTuft(c, x0 + random.range(0.2f, 3.8f), z0 + random.range(0.2f, 3.8f), height,
                 vary(tint, random, 0.1f), wild, random, random.next() < flowerChance);
    }
    // Камыш и осока на берегу — у края, ближе к воде.
    int dx = 0;
    int dy = 0;
    if ((flags & LandUse::kShore) != 0 && waterSide(map, cell, dx, dy)) {
        const int clumps = 1 + static_cast<int>(random.next() * 2.5f);
        for (int i = 0; i < clumps; ++i) {
            const float along = random.range(0.5f, 3.5f);
            const float x = dx != 0 ? x0 + 2.0f + static_cast<float>(dx) * random.range(1.0f, 2.2f) : x0 + along;
            const float z = dy != 0 ? z0 + 2.0f + static_cast<float>(dy) * random.range(1.0f, 2.2f) : z0 + along;
            emitReeds(c, x, z, random.next() < 0.5f, random);
        }
    }
    // Камешек в траве.
    if (random.next() < 0.012f) {
        const float x = x0 + random.range(0.5f, 3.5f);
        const float z = z0 + random.range(0.5f, 3.5f);
        c.b.setAnchor({x, c.terrain.groundAt(x, z), z});
        c.b.boulder({x, c.terrain.groundAt(x, z), z}, random.range(0.15f, 0.4f), random);
        c.b.items++;
    }
}

// --- Крупное: заборы, колодцы, скамейки, поленницы, стога, столбы, мостки ----

constexpr float fenceHeight(LandUse::FenceStyle style) {
    switch (style) {
    case LandUse::FenceStyle::Picket:
        return 1.2f;
    case LandUse::FenceStyle::Wattle:
        return 1.0f;
    case LandUse::FenceStyle::Boards:
        return 1.7f;
    default:
        return 1.25f;
    }
}

Color fenceColor(LandUse::FenceStyle style, std::uint32_t bits) {
    switch (style) {
    case LandUse::FenceStyle::Picket: {
        // Штакетник бывает крашеным.
        constexpr Color kPaint[] = {{126, 114, 98, 255}, {150, 136, 112, 255}, {92, 122, 92, 255},
                                    {104, 120, 150, 255}, {196, 192, 180, 255}};
        return kPaint[(bits >> 4) % 5];
    }
    case LandUse::FenceStyle::Wattle:
        return {116, 98, 72, 255};
    case LandUse::FenceStyle::Boards:
        return {110, 92, 70, 255};
    default:
        return kGreyWood;
    }
}

// Полотно забора от a до b (по земле) высотой h, вид — param; u — вдоль.
void fenceSpan(Context& c, Vector2 a, Vector2 b, float h, LandUse::FenceStyle style, Color color) {
    const float ya = c.terrain.groundAt(a.x, a.y);
    const float yb = c.terrain.groundAt(b.x, b.y);
    c.b.setAnchor({0.5f * (a.x + b.x), 0.5f * (ya + yb), 0.5f * (a.y + b.y)});
    c.b.reserve(4);
    const float param = static_cast<float>(style);
    // Узор — по мировой координате вдоль: соседние пролёты стыкуются.
    const float u0 = std::abs(b.x - a.x) > std::abs(b.y - a.y) ? a.x : a.y;
    const float u1 = u0 + std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    const int v0 = c.b.vertex({a.x, ya, a.y}, kFence, param, 0.0f, u0, color);
    const int v1 = c.b.vertex({b.x, yb, b.y}, kFence, param, 0.0f, u1, color);
    const int v2 = c.b.vertex({b.x, yb + h, b.y}, kFence, param, 1.0f, u1, color);
    const int v3 = c.b.vertex({a.x, ya + h, a.y}, kFence, param, 1.0f, u0, color);
    c.b.quad(v0, v1, v2, v3);
}

void post(Context& c, float x, float z, float h, float size, Color color) {
    const float y = c.terrain.groundAt(x, z);
    c.b.setAnchor({x, y, z});
    c.b.box({x, y - 0.2f, z}, 1.0f, 0.0f, size, size, h + 0.2f, color);
}

void emitFence(Context& c, const LandUse::Thing& t) {
    const float h = fenceHeight(t.style);
    const Color color = fenceColor(t.style, t.bits);
    const Color postColor = t.style == LandUse::FenceStyle::Picket ? kDarkWood : color;
    const auto at = [&](float s) { return Vector2{t.x + t.dx * s, t.z + t.dz * s}; };
    if (t.kind == LandUse::ThingKind::Gate) {
        // Калитка: столбы повыше, створка — короткий пролёт, рядом проход.
        fenceSpan(c, at(0.0f), at(0.9f), h, t.style, color);
        fenceSpan(c, at(1.0f), at(2.0f), h * 0.95f, t.style, color);
        fenceSpan(c, at(3.1f), at(4.0f), h, t.style, color);
        post(c, at(0.95f).x, at(0.95f).y, h + 0.25f, 0.07f, kDarkWood);
        post(c, at(3.05f).x, at(3.05f).y, h + 0.25f, 0.07f, kDarkWood);
    } else {
        fenceSpan(c, at(0.0f), at(t.length), h, t.style, color);
    }
    post(c, t.x, t.z, h + (t.style == LandUse::FenceStyle::Wattle ? 0.2f : 0.05f), 0.05f, postColor);
    if (t.style == LandUse::FenceStyle::Wattle) {
        // Колья плетня торчат над ним.
        for (const float s : {1.0f, 2.0f, 3.0f}) {
            post(c, at(s).x, at(s).y, h + 0.15f, 0.03f, kDarkWood);
        }
    }
    c.b.items++;
}

void emitWell(Context& c, const LandUse::Thing& t, Random& random) {
    const float y = c.terrain.groundAt(t.x, t.z);
    c.b.setAnchor({t.x, y, t.z});
    const float dx = t.dx;
    const float dz = t.dz;
    // Сруб, тёмная вода внутри, два столба, крыша домиком и ворот.
    c.b.box({t.x, y - 0.2f, t.z}, dx, dz, 0.6f, 0.6f, 1.0f, vary(kGreyWood, random, 0.08f));
    c.b.box({t.x, y + 0.8f, t.z}, dx, dz, 0.45f, 0.45f, 0.02f, {28, 30, 32, 255});
    const float sx = -dz;
    const float sz = dx;
    for (const float s : {-0.55f, 0.55f}) {
        c.b.box({t.x + sx * s, y, t.z + sz * s}, dx, dz, 0.06f, 0.06f, 2.0f, kDarkWood);
    }
    c.b.box({t.x, y + 1.35f, t.z}, sx, sz, 0.6f, 0.05f, 0.1f, kDarkWood);
    c.b.reserve(6);
    const Color roof = vary({96, 84, 72, 255}, random, 0.1f);
    const float ry = y + 2.0f;
    const auto corner = [&](float a, float b, float up) {
        return Vector3{t.x + sx * a + dx * b, ry + up, t.z + sz * a + dz * b};
    };
    const int r0 = c.b.vertex(corner(-0.75f, -0.75f, 0.0f), kSolid, 0.0f, 0.0f, 0.0f, roof);
    const int r1 = c.b.vertex(corner(0.75f, -0.75f, 0.0f), kSolid, 0.0f, 0.0f, 0.0f, roof);
    const int r2 = c.b.vertex(corner(0.75f, 0.0f, 0.55f), kSolid, 0.0f, 1.0f, 0.0f, roof);
    const int r3 = c.b.vertex(corner(-0.75f, 0.0f, 0.55f), kSolid, 0.0f, 1.0f, 0.0f, roof);
    const int r4 = c.b.vertex(corner(0.75f, 0.75f, 0.0f), kSolid, 0.0f, 0.0f, 0.0f, roof);
    const int r5 = c.b.vertex(corner(-0.75f, 0.75f, 0.0f), kSolid, 0.0f, 0.0f, 0.0f, roof);
    c.b.quad(r0, r1, r2, r3);
    c.b.quad(r3, r2, r4, r5);
    c.b.items++;
}

void emitBench(Context& c, const LandUse::Thing& t) {
    const float y = c.terrain.groundAt(t.x, t.z);
    c.b.setAnchor({t.x, y, t.z});
    const float sx = -t.dz; // вдоль скамейки
    const float sz = t.dx;
    const float half = 0.5f * t.length;
    c.b.box({t.x, y + 0.42f, t.z}, sx, sz, half, 0.18f, 0.05f, kWood);
    for (const float s : {-half + 0.12f, half - 0.12f}) {
        c.b.box({t.x + sx * s, y, t.z + sz * s}, sx, sz, 0.04f, 0.15f, 0.42f, kDarkWood);
    }
    // Спинка — со стороны стены.
    c.b.box({t.x - t.dx * 0.17f, y + 0.55f, t.z - t.dz * 0.17f}, sx, sz, half, 0.025f, 0.3f, kWood);
    c.b.items++;
}

void emitWoodpile(Context& c, const LandUse::Thing& t, Random& random) {
    const float y = c.terrain.groundAt(t.x, t.z);
    c.b.setAnchor({t.x, y, t.z});
    const float half = 0.5f * t.length;
    const float height = random.range(0.9f, 1.4f);
    // Торцы поленьев светлые, сверху — доска от дождя.
    c.b.box({t.x, y, t.z}, t.dx, t.dz, half, 0.35f, height, vary({168, 132, 92, 255}, random, 0.08f));
    c.b.box({t.x, y + height, t.z}, t.dx, t.dz, half + 0.15f, 0.45f, 0.05f, kDarkWood);
    c.b.items++;
}

void emitHaystack(Context& c, float x, float z, float param, Random& random, float scale) {
    const float y = c.terrain.groundAt(x, z);
    c.b.setAnchor({x, y, z});
    const float rings[4][2] = {{1.5f * scale, -0.1f},
                               {1.55f * scale, 0.9f * scale},
                               {1.15f * scale, 1.9f * scale},
                               {0.45f * scale, 2.6f * scale}};
    c.b.dome({x, y, z}, rings, 4, 9, vary(kHay, random, 0.08f), kBale, param, random);
    c.b.box({x, y + 2.8f * scale, z}, 1.0f, 0.0f, 0.04f, 0.04f, 0.7f, kDarkWood, kBale, param);
    c.b.items++;
}

void emitBale(Context& c, float x, float z, float param, Random& random) {
    const float radius = 0.7f;
    const float y = c.terrain.groundAt(x, z);
    c.b.setAnchor({x, y, z});
    const float a = random.next() * 2.0f * kPi;
    c.b.roll({x, y + radius - 0.05f, z}, std::cos(a), std::sin(a), radius, 0.6f, 10, vary(kStraw, random, 0.06f),
             vary({200, 182, 120, 255}, random, 0.05f), kBale, param);
    c.b.items++;
}

// Где на столбе крепятся провода: концы траверсы (поперёк дороги) и середина.
Vector3 wireAttach(const LandUse::Thing& pole, float ground, int index) {
    const float across = static_cast<float>(index - 1) * 0.6f;
    return {pole.x - pole.dz * across, ground + 7.55f, pole.z + pole.dx * across};
}

void emitPole(Context& c, const LandUse::Thing& t, const std::vector<LandUse::Thing>& things, int self) {
    const float y = c.terrain.groundAt(t.x, t.z);
    c.b.setAnchor({t.x, y, t.z});
    c.b.prism({t.x, y - 0.3f, t.z}, 0.13f, 0.1f, 8.0f, 6, kDarkWood);
    c.b.box({t.x, y + 7.4f, t.z}, -t.dz, t.dx, 0.75f, 0.05f, 0.1f, kDarkWood);
    for (int i = 0; i < 3; ++i) {
        const Vector3 p = wireAttach(t, y, i);
        c.b.box({p.x, p.y - 0.05f, p.z}, 1.0f, 0.0f, 0.03f, 0.03f, 0.1f, kInsulator);
    }
    // Провода — к соседям с большим номером (каждый пролёт — один раз), с провисом.
    for (const int link : t.links) {
        if (link <= self) {
            continue;
        }
        const LandUse::Thing& other = things[static_cast<std::size_t>(link)];
        const float oy = c.terrain.groundAt(other.x, other.z);
        // Траверсы могут смотреть в разные стороны: концы — по одной стороне дороги.
        const bool flip = (-t.dz) * (-other.dz) + t.dx * other.dx < 0.0f;
        const float span = std::sqrt((other.x - t.x) * (other.x - t.x) + (other.z - t.z) * (other.z - t.z));
        const float sag = 0.15f + 0.5f * (span / 30.0f) * (span / 30.0f);
        for (int i = 0; i < 3; ++i) {
            const Vector3 a = wireAttach(t, y, i);
            const Vector3 b = wireAttach(other, oy, flip ? 2 - i : i);
            constexpr int kSegments = 8;
            c.b.reserve(2 * (kSegments + 1));
            const Vector3 direction = Vector3Normalize(Vector3Subtract(b, a));
            const int first = c.b.count();
            for (int k = 0; k <= kSegments; ++k) {
                const float s = static_cast<float>(k) / static_cast<float>(kSegments);
                Vector3 p = Vector3Lerp(a, b, s);
                p.y -= 4.0f * sag * s * (1.0f - s);
                c.b.wireVertex(p, direction, -1.0f);
                c.b.wireVertex(p, direction, 1.0f);
            }
            for (int k = 0; k < kSegments; ++k) {
                const int v = first + 2 * k;
                c.b.quad(v, v + 2, v + 3, v + 1);
            }
        }
    }
    c.b.items++;
}

void emitPier(Context& c, const LandUse::Thing& t) {
    const float endX = t.x + t.dx * t.length;
    const float endZ = t.z + t.dz * t.length;
    const float water = c.terrain.surfaceAt(endX, endZ);
    const float deck = std::max(water + 0.45f, c.terrain.groundAt(t.x, t.z) + 0.15f);
    const float midX = 0.5f * (t.x + endX);
    const float midZ = 0.5f * (t.z + endZ);
    c.b.setAnchor({midX, deck, midZ});
    c.b.box({midX, deck - 0.08f, midZ}, t.dx, t.dz, 0.5f * t.length, 0.65f, 0.08f, {128, 108, 84, 255});
    const float sx = -t.dz;
    const float sz = t.dx;
    for (float s = 0.4f; s < t.length; s += 1.6f) {
        for (const float side : {-0.55f, 0.55f}) {
            const float px = t.x + t.dx * s + sx * side;
            const float pz = t.z + t.dz * s + sz * side;
            const float bottom = c.terrain.groundAt(px, pz) - 0.4f;
            if (deck - 0.08f > bottom) {
                c.b.box({px, bottom, pz}, 1.0f, 0.0f, 0.06f, 0.06f, deck - 0.08f - bottom, kDarkWood);
            }
        }
    }
    c.b.items++;
}

} // namespace

Props3D::Props3D(const Map& map, const Terrain3D& terrain, const LandUse& land)
    : map_(map), terrain_(terrain), land_(land) {
    const int chunksX = (map.width() + kChunkCells - 1) / kChunkCells;
    const int chunksY = (map.height() + kChunkCells - 1) / kChunkCells;
    chunks_.resize(static_cast<std::size_t>(chunksX) * static_cast<std::size_t>(chunksY));
    for (int cy = 0; cy < chunksY; ++cy) {
        for (int cx = 0; cx < chunksX; ++cx) {
            Chunk& chunk = chunks_[static_cast<std::size_t>(cy * chunksX + cx)];
            chunk.left = cx * kChunkCells;
            chunk.top = cy * kChunkCells;
            chunk.width = std::min(kChunkCells, map.width() - chunk.left);
            chunk.height = std::min(kChunkCells, map.height() - chunk.top);
            float low = 1.0e9f;
            float high = -1.0e9f;
            for (int y = chunk.top; y < chunk.top + chunk.height; ++y) {
                for (int x = chunk.left; x < chunk.left + chunk.width; ++x) {
                    low = std::min(low, map.elevationMeters({x, y}));
                    high = std::max(high, map.elevationMeters({x, y}) + map.depthMeters({x, y}));
                }
            }
            constexpr float kReach = 6.0f; // провода и мостки выходят за чанк
            chunk.min = {static_cast<float>(chunk.left) * Map::kCellSize - kReach, low - 2.0f,
                         static_cast<float>(chunk.top) * Map::kCellSize - kReach};
            chunk.max = {static_cast<float>(chunk.left + chunk.width) * Map::kCellSize + kReach, high + kTallest,
                         static_cast<float>(chunk.top + chunk.height) * Map::kCellSize + kReach};
        }
    }

    const std::string vertex = std::string("#version 330\n") + kFieldGlsl + kPropsVs;
    const std::string fragment = std::string(kFieldGlsl) + kPropsFs;
    shader_ = SceneShading::loadShader(vertex.c_str(), fragment.c_str());
    const std::string shadowVertex = std::string("#version 330\n#define SHADOW_PASS\n") + kFieldGlsl + kPropsVs;
    const std::string shadowFragment = std::string("#define SHADOW_PASS\n") + fragment;
    shadowShader_ = SceneShading::loadShader(shadowVertex.c_str(), shadowFragment.c_str());
    rangeLoc_ = GetShaderLocation(shader_, "range");
    shadowRangeLoc_ = GetShaderLocation(shadowShader_, "range");
    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    shadowMaterial_ = LoadMaterialDefault();
    shadowMaterial_.shader = shadowShader_;
}

Props3D::~Props3D() {
    for (Chunk& chunk : chunks_) {
        unload(chunk.small);
        unload(chunk.large);
    }
    UnloadShader(shader_);
    UnloadShader(shadowShader_);
    MemFree(material_.maps);
    MemFree(shadowMaterial_.maps);
}

void Props3D::reset() {
    for (Chunk& chunk : chunks_) {
        unload(chunk.small);
        unload(chunk.large);
    }
}

void Props3D::unload(Part& part) {
    if (!part.built) {
        return;
    }
    for (Mesh& mesh : part.meshes) {
        UnloadMesh(mesh);
    }
    part = Part{};
    --meshesLoaded_;
}

void Props3D::buildSmall(Chunk& chunk) {
    const double start = GetTime();
    PropBuilder builder;
    Context context{map_, terrain_, land_, builder};
    for (int y = chunk.top; y < chunk.top + chunk.height; ++y) {
        for (int x = chunk.left; x < chunk.left + chunk.width; ++x) {
            emitSmallCell(context, {x, y});
        }
    }
    chunk.small.items = builder.items;
    cpuTime_ += GetTime() - start;
    chunk.small.meshes = builder.finish();
    chunk.small.built = true;
    ++meshesLoaded_;
}

void Props3D::buildLarge(Chunk& chunk) {
    const double start = GetTime();
    PropBuilder builder;
    Context context{map_, terrain_, land_, builder};
    const std::vector<LandUse::Thing>& things = land_.things();
    for (const int index : land_.thingsIn(chunk.left / LandUse::kBucketCells, chunk.top / LandUse::kBucketCells)) {
        const LandUse::Thing& thing = things[static_cast<std::size_t>(index)];
        Random random(thing.bits);
        switch (thing.kind) {
        case LandUse::ThingKind::Fence:
        case LandUse::ThingKind::Gate:
            emitFence(context, thing);
            break;
        case LandUse::ThingKind::Well:
            emitWell(context, thing, random);
            break;
        case LandUse::ThingKind::Bench:
            emitBench(context, thing);
            break;
        case LandUse::ThingKind::Woodpile:
            emitWoodpile(context, thing, random);
            break;
        case LandUse::ThingKind::Haystack:
            emitHaystack(context, thing.x, thing.z, kBarnStackParam, random, 1.0f);
            break;
        case LandUse::ThingKind::Pole:
            emitPole(context, thing, things, index);
            break;
        case LandUse::ThingKind::Pier:
            emitPier(context, thing);
            break;
        }
    }
    // По клеткам: валуны на камне, тюки на убранных полях, стога на пару.
    for (int y = chunk.top; y < chunk.top + chunk.height; ++y) {
        for (int x = chunk.left; x < chunk.left + chunk.width; ++x) {
            const Cell cell = {x, y};
            if (map_.water.at(x, y) != Water::None || map_.road.at(x, y) != Road::None ||
                map_.cover.at(x, y) != Cover::None || land_.flags(cell) != 0) {
                continue;
            }
            Random random(cellSeed(cell, 2u));
            const float x0 = static_cast<float>(x) * Map::kCellSize;
            const float z0 = static_cast<float>(y) * Map::kCellSize;
            const Ground ground = map_.ground.at(x, y);
            if (ground == Ground::Rock) {
                const int n = static_cast<int>(random.next() * 3.0f);
                for (int i = 0; i < n; ++i) {
                    const float bx = x0 + random.range(0.6f, 3.4f);
                    const float bz = z0 + random.range(0.6f, 3.4f);
                    const float by = terrain_.groundAt(bx, bz);
                    builder.setAnchor({bx, by, bz});
                    builder.boulder({bx, by, bz}, random.range(0.4f, 1.3f), random);
                    builder.items++;
                }
            } else if (ground == Ground::Field) {
                const float roll = random.next();
                const float bx = x0 + random.range(1.0f, 3.0f);
                const float bz = z0 + random.range(1.0f, 3.0f);
                const FieldPatch patch = fieldPatchAt(bx, bz);
                if (patch.edge < kBalkWidth + 1.0f) {
                    continue;
                }
                const bool grain = patch.crop == CropKind::Wheat || patch.crop == CropKind::Rye ||
                                   patch.crop == CropKind::Barley || patch.crop == CropKind::Rapeseed;
                if (grain && roll < 0.03f) {
                    emitBale(context, bx, bz, static_cast<float>(patch.crop), random);
                } else if (patch.crop == CropKind::Fallow && roll < 0.015f) {
                    emitHaystack(context, bx, bz, kHaystackParam, random, random.range(0.8f, 1.1f));
                }
            }
        }
    }
    chunk.large.items = builder.items;
    cpuTime_ += GetTime() - start;
    chunk.large.meshes = builder.finish();
    chunk.large.built = true;
    ++meshesLoaded_;
}

void Props3D::update(const Camera3D& camera, float smallRange, float largeRange) {
    ++frame_;
    smallRange_ = std::min(smallRange, largeRange);
    largeRange_ = largeRange;
    pixelAngle_ = 2.0f * std::tan(camera.fovy * 0.5f * DEG2RAD) / static_cast<float>(std::max(GetScreenHeight(), 1));
    const Vector3 eye = camera.position;
    struct Wanted {
        float distance = 0.0f;
        Chunk* chunk = nullptr;
        bool small = false;
    };
    std::vector<Wanted> missing;
    for (Chunk& chunk : chunks_) {
        const float distance = distanceToBox(eye, chunk.min, chunk.max);
        chunk.drawSmall = distance < smallRange_;
        chunk.drawLarge = distance < largeRange_;
        if (chunk.drawSmall) {
            chunk.small.lastUsed = frame_;
            if (!chunk.small.built) {
                missing.push_back({distance, &chunk, true});
            }
        }
        if (chunk.drawLarge) {
            chunk.large.lastUsed = frame_;
            if (!chunk.large.built) {
                missing.push_back({distance, &chunk, false});
            }
        }
    }
    std::sort(missing.begin(), missing.end(), [](const Wanted& a, const Wanted& b) { return a.distance < b.distance; });
    cpuTime_ = 0.0;
    pending_ = static_cast<int>(missing.size());
    for (const Wanted& wanted : missing) {
        --pending_;
        if (wanted.small) {
            buildSmall(*wanted.chunk);
        } else {
            buildLarge(*wanted.chunk);
        }
        if (cpuTime_ > kBuildBudget) {
            break;
        }
    }
    for (Chunk& chunk : chunks_) {
        if (chunk.small.built && frame_ - chunk.small.lastUsed > kEvictFrames) {
            unload(chunk.small);
        }
        if (chunk.large.built && frame_ - chunk.large.lastUsed > kEvictFrames) {
            unload(chunk.large);
        }
    }
}

void Props3D::setUniforms(const Shader& shader, const Look& look) const {
    const auto set = [&shader](const char* name, const void* value, int type) {
        SetShaderValue(shader, GetShaderLocation(shader, name), value, type);
    };
    set("year", &look.year, SHADER_UNIFORM_FLOAT);
    set("snow", &look.snow, SHADER_UNIFORM_FLOAT);
    set("time", &look.time, SHADER_UNIFORM_FLOAT);
    set("decay", &look.decay, SHADER_UNIFORM_FLOAT);
    set("wind", &look.wind, SHADER_UNIFORM_VEC2);
    set("pixelAngle", &pixelAngle_, SHADER_UNIFORM_FLOAT);
}

void Props3D::drawPart(const Shader& shader, Material& material, const Part& part, float range, bool count) {
    if (!part.built || part.lastUsed != frame_) {
        return;
    }
    SetShaderValue(shader, &shader == &shader_ ? rangeLoc_ : shadowRangeLoc_, &range, SHADER_UNIFORM_FLOAT);
    for (const Mesh& mesh : part.meshes) {
        DrawMesh(mesh, material, MatrixIdentity());
    }
    if (count) {
        itemsDrawn_ += part.items;
    }
}

void Props3D::draw(const SceneShading& shading, const Look& look) {
    const Matrix viewProjection = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    shading.apply(shader_);
    setUniforms(shader_, look);
    itemsDrawn_ = 0;
    // Травинки, полотна заборов и посевов видны с обеих сторон.
    rlDisableBackfaceCulling();
    for (Chunk& chunk : chunks_) {
        if ((!chunk.drawSmall && !chunk.drawLarge) || outsideFrustum(viewProjection, chunk.min, chunk.max)) {
            continue;
        }
        if (chunk.drawSmall) {
            drawPart(shader_, material_, chunk.small, smallRange_, true);
        }
        if (chunk.drawLarge) {
            drawPart(shader_, material_, chunk.large, largeRange_, true);
        }
    }
    rlEnableBackfaceCulling();
}

void Props3D::drawShadows(const SceneShading& shading, const Look& look) {
    shading.apply(shadowShader_);
    setUniforms(shadowShader_, look);
    rlDisableBackfaceCulling();
    for (Chunk& chunk : chunks_) {
        if (!chunk.drawLarge) {
            continue;
        }
        const Vector3 center = Vector3Scale(Vector3Add(chunk.min, chunk.max), 0.5f);
        if (!shading.inShadowArea(center, 0.5f * std::max(chunk.max.x - chunk.min.x, chunk.max.z - chunk.min.z))) {
            continue;
        }
        drawPart(shadowShader_, shadowMaterial_, chunk.large, largeRange_, false);
    }
    rlEnableBackfaceCulling();
}

} // namespace lw
