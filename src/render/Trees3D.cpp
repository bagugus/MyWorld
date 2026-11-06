#include "render/Trees3D.h"

#include "render/Lighting3D.h"
#include "render/Terrain3D.h"
#include "render/TreeLayout.h"
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

// Сколько вершин может дать одно дерево: столько места должно оставаться в
// сетке перед следующим (индексы — 16 бит).
constexpr int kMaxTreeVertices = 128;
constexpr int kMaxMeshVertices = 65535;
// Время на постройку сеток за кадр, секунды: при быстром полёте деревья
// появляются чуть позже, зато кадр не замирает. Считается только работа
// процессора: первый вызов GL в кадре может ждать кадровую синхронизацию, и
// загрузка сетки в видеопамять выглядела бы долгой.
constexpr double kBuildBudget = 0.004;
// Сетка, не нужная столько кадров, выгружается.
constexpr std::uint32_t kEvictFrames = 600;
// Крона выходит за клетку, верхушка — выше земли: запас границ чанка, метры.
constexpr float kCrownReach = 6.0f;
constexpr float kTallest = 32.0f;

// Части дерева — texcoord.x вершины; шейдер красит их по-своему.
constexpr float kWood = 0.0f;        // ствол и ветви
constexpr float kNeedles = 1.0f;     // хвоя: круглый год
constexpr float kLeaves = 2.0f;      // листва лиственных и кустов
constexpr float kBirchLeaves = 3.0f; // листва берёзы: осенью жёлтая

constexpr Color kSpruceBark = {74, 58, 46, 255};
constexpr Color kPineBark = {128, 86, 58, 255};
constexpr Color kPineBarkLow = {88, 66, 50, 255};
constexpr Color kBroadleafBark = {86, 74, 62, 255};
constexpr Color kBirchBark = {214, 210, 198, 255};
constexpr Color kBirchBarkLow = {104, 96, 88, 255};
constexpr Color kSpruceNeedles = {36, 70, 44, 255};
constexpr Color kPineNeedles = {58, 88, 46, 255};
constexpr Color kBroadleafGreen = {64, 104, 46, 255};
constexpr Color kBirchGreen = {98, 132, 58, 255};
constexpr Color kBushGreen = {70, 106, 48, 255};

// Деревья: вершина — место (со сдвигом ветра), корень дерева (в нормали: от
// него — расстояние для детализации), часть и срок листвы (texcoord), цвет и
// доля высоты (color.a). Нормаль грани — по производным позиции.
constexpr const char* kTreeVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform vec3 viewPos;
uniform float year;
uniform float time;
uniform vec2 wind;
uniform float nearRange;
uniform float farRange;
uniform float lod;
out vec3 fragPosition;
out vec3 fragRest;
out vec4 fragColor;
flat out float fragPart;
flat out vec2 fragKeep;
flat out vec3 fragFoliage;
flat out float fragTiming;

void main() {
    vec3 root = vertexNormal;
    // Детализация — по расстоянию до корня: дерево меняется целиком.
    // Подробная тает к nearRange, упрощённая проявляется на её месте (дополняющие
    // доли матрицы Байера) и тает к farRange.
    float away = length(root - viewPos);
    float detail = 1.0 - smoothstep(0.85 * nearRange, nearRange, away);
    float shown = 1.0 - smoothstep(0.75 * farRange, farRange, away);
    fragKeep = lod < 0.5 ? vec2(0.0, min(detail, shown)) : vec2(detail, shown);

    // Листва — как foliageAt (render/TreeLayout.cpp).
    float t = vertexTexCoord.y;
    float onset = 0.02 + 0.08 * t;
    float grow = clamp((year - onset) / 0.05, 0.0, 1.0);
    float turnStart = 0.53 + 0.08 * t;
    float turn = clamp((year - turnStart) / 0.06, 0.0, 1.0);
    float fall = clamp((year - turnStart - 0.05) / 0.07, 0.0, 1.0);
    float fresh = 1.0 - clamp((year - onset - 0.05) / 0.12, 0.0, 1.0);
    fragFoliage = vec3(grow * (1.0 - fall), turn, fresh);
    fragTiming = t;
    fragPart = vertexTexCoord.x;

    // Ветер клонит и качает верхушку, деревья — вразнобой.
    float above = max(vertexPosition.y - root.y, 0.0);
    float h = vertexColor.a;
    float speed = length(wind);
    vec2 along = speed > 0.01 ? wind / speed : vec2(1.0, 0.0);
    float phase = dot(root.xz, vec2(0.071, 0.053));
    float amplitude = h * h * above * (0.004 + 0.0018 * speed);
    vec2 sway = along * amplitude * (0.6 + 0.4 * sin(time * 1.3 + phase)) +
                vec2(-along.y, along.x) * amplitude * 0.3 * sin(time * 2.1 + phase * 1.7);
    vec3 p = vertexPosition + vec3(sway.x, 0.0, sway.y);
    fragPosition = p;
    fragRest = vertexPosition;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(p, 1.0);
}
)";

constexpr const char* kTreeFs = R"(
in vec3 fragPosition;
in vec3 fragRest;
in vec4 fragColor;
flat in float fragPart;
flat in vec2 fragKeep;
flat in vec3 fragFoliage;
flat in float fragTiming;
uniform float lod;
uniform float snow;
out vec4 finalColor;

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Осенний цвет: берёза — жёлтая, остальные — от жёлтого до бурого.
vec3 autumnColor(float part, float timing) {
    if (part > 2.5) {
        return vec3(0.86, 0.72, 0.22);
    }
    float k = fract(timing * 7.31);
    return k < 0.4 ? vec3(0.82, 0.60, 0.16) : (k < 0.75 ? vec3(0.80, 0.42, 0.12) : vec3(0.56, 0.26, 0.12));
}

void main() {
    // Грань плоская: нормаль — по производным позиции, к камере.
    vec3 n = normalize(cross(dFdx(fragPosition), dFdy(fragPosition)));
    if (dot(n, viewPos - fragPosition) < 0.0) {
        n = -n;
    }
    ivec2 cell = ivec2(mod(gl_FragCoord.xy, 4.0));
    float threshold = (kBayer[cell.y * 4 + cell.x] + 0.5) / 16.0;
    if (threshold < fragKeep.x || threshold >= fragKeep.y) {
        discard;
    }
    // Листва — клочьями: распускается и облетает не вся сразу. Голая крона
    // вблизи — только ствол и сучья (клочья веток висели бы в воздухе
    // обломками), дальше — редкая серая дымка мелких веток, гуще с расстоянием.
    bool leafy = fragPart > 1.5;
    float clump = 0.0;
    if (leafy) {
        float clumpSize = lod < 0.5 ? 1.0 / 6.0 : 1.0;
        clump = hash(floor(fragRest / clumpSize));
        // Клочок мельче пары пикселей при движении камеры то попадал бы в
        // пиксель, то нет — крона искрилась. Вдали вместо клочьев — та же доля
        // пикселей по матрице Байера (сдвинутой, чтобы не совпасть с растворением):
        // узор стоит на экране и не мерцает.
        float clumpPixels = clumpSize / max(length(fwidth(fragRest)), 1e-4);
        float dithered = (kBayer[((cell.y + 2) & 3) * 4 + ((cell.x + 1) & 3)] + 0.5) / 16.0;
        clump = mix(dithered, clump, smoothstep(1.5, 4.0, clumpPixels));
        float twigs = lod < 0.5 ? 0.3 * smoothstep(40.0, 200.0, length(fragRest - viewPos)) : 0.45;
        if (clump >= max(fragFoliage.x, twigs)) {
            discard;
        }
    }
#ifdef SHADOW_PASS
    finalColor = vec4(1.0);
#else
    vec3 base = fragColor.rgb;
    bool bare = leafy && clump >= fragFoliage.x;
    if (bare) {
        base = vec3(0.34, 0.30, 0.26);
    } else if (leafy) {
        base = mix(base, vec3(0.50, 0.68, 0.26), 0.6 * fragFoliage.z);
        float spot = 0.85 + 0.3 * hash(floor(fragRest * 1.7) + 5.0);
        base = mix(base, autumnColor(fragPart, fragTiming) * spot, fragFoliage.y);
    }
    // Внизу и в глубине кроны темнее.
    if (fragPart > 0.5) {
        base *= 0.72 + 0.28 * fragColor.a;
    }
    // Снег — на верхних гранях хвои, ветвей и голых крон.
    if (snow > 0.0 && (!leafy || bare)) {
        base = mix(base, vec3(0.90, 0.92, 0.96), 0.9 * snow * smoothstep(-0.05, 0.4, n.y));
    }
    vec3 color = base * lighting(n, shadowAt(fragPosition, n));
    finalColor = vec4(withFog(color, fragPosition), 1.0);
#endif
}
)";

// Разброс облика дерева: из TreeSpec::bits.
class ShapeRandom {
public:
    explicit ShapeRandom(std::uint32_t seed) : state_(seed * 2654435761u + 1u) {}
    float next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(state_ >> 8) / static_cast<float>(1u << 24);
    }
    // −1..1
    float signedNext() { return next() * 2.0f - 1.0f; }

private:
    std::uint32_t state_;
};

Color vary(Color color, ShapeRandom& random, float amount) {
    const float bright = 1.0f + amount * random.signedNext();
    const float green = 1.0f + 0.5f * amount * random.signedNext();
    const auto channel = [](float value) { return static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f)); };
    return {channel(static_cast<float>(color.r) * bright), channel(static_cast<float>(color.g) * bright * green),
            channel(static_cast<float>(color.b) * bright), color.a};
}

Color darker(Color color, float factor) {
    const auto channel = [factor](unsigned char value) {
        return static_cast<unsigned char>(static_cast<float>(value) * factor);
    };
    return {channel(color.r), channel(color.g), channel(color.b), color.a};
}

// Сетка деревьев: вершины общие, грани обращены наружу (против часовой снаружи).
class TreeBuilder {
public:
    int vertexCount() const { return static_cast<int>(positions_.size() / 3); }
    bool hasRoom() const { return vertexCount() + kMaxTreeVertices <= kMaxMeshVertices; }
    bool empty() const { return indices_.empty(); }

    // Следующие вершины — этого дерева; ground — высота земли у корня.
    void begin(const TreeSpec& tree, float ground) {
        tree_ = tree;
        root_ = {tree.x, ground, tree.z};
        cosYaw_ = std::cos(tree.yaw);
        sinYaw_ = std::sin(tree.yaw);
        random_ = ShapeRandom(tree.bits);
    }
    ShapeRandom& random() { return random_; }

    // Точка в осях дерева (y — вверх от корня) — в мир: поворот и наклон.
    Vector3 toWorld(Vector3 p) const {
        const float x = p.x * cosYaw_ + p.z * sinYaw_;
        const float z = -p.x * sinYaw_ + p.z * cosYaw_;
        const float up = std::max(p.y, 0.0f);
        return {root_.x + x + tree_.tiltX * up, root_.y + p.y, root_.z + z + tree_.tiltZ * up};
    }

    int vertex(Vector3 local, float part, Color color) {
        const Vector3 p = toWorld(local);
        positions_.insert(positions_.end(), {p.x, p.y, p.z});
        roots_.insert(roots_.end(), {root_.x, root_.y, root_.z});
        texcoords_.insert(texcoords_.end(), {part, tree_.timing});
        // Соседние вершины чуть разного цвета: грани не одинаковые.
        const Color c = vary(color, random_, 0.07f);
        const float share = std::clamp(local.y / std::max(tree_.height, 0.1f), 0.0f, 1.0f);
        colors_.insert(colors_.end(), {c.r, c.g, c.b, static_cast<unsigned char>(std::lround(share * 255.0f))});
        return vertexCount() - 1;
    }

    // Треугольник наружу от точки inside (в осях дерева).
    void triangle(int a, int b, int c, Vector3 inside) {
        const Vector3 pa = position(a);
        const Vector3 normal = Vector3CrossProduct(Vector3Subtract(position(b), pa), Vector3Subtract(position(c), pa));
        const Vector3 center = Vector3Scale(Vector3Add(Vector3Add(pa, position(b)), position(c)), 1.0f / 3.0f);
        if (Vector3DotProduct(normal, Vector3Subtract(center, toWorld(inside))) < 0.0f) {
            std::swap(b, c);
        }
        indices_.insert(indices_.end(), {static_cast<unsigned short>(a), static_cast<unsigned short>(b),
                                         static_cast<unsigned short>(c)});
    }

    // Призма от a до b (радиусы ra и rb), без торцов.
    void prism(Vector3 a, Vector3 b, float ra, float rb, int sides, float part, Color colorA, Color colorB) {
        const Vector3 axis = Vector3Normalize(Vector3Subtract(b, a));
        const Vector3 helper = std::abs(axis.y) < 0.9f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
        const Vector3 u = Vector3Normalize(Vector3CrossProduct(axis, helper));
        const Vector3 v = Vector3CrossProduct(axis, u);
        const float twist = random_.next() * 2.0f * kPi;
        const int first = vertexCount();
        for (int k = 0; k < sides; ++k) {
            const float angle = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const Vector3 dir = Vector3Add(Vector3Scale(u, std::cos(angle)), Vector3Scale(v, std::sin(angle)));
            vertex(Vector3Add(a, Vector3Scale(dir, ra)), part, colorA);
            vertex(Vector3Add(b, Vector3Scale(dir, rb)), part, colorB);
        }
        const Vector3 inside = Vector3Scale(Vector3Add(a, b), 0.5f);
        for (int k = 0; k < sides; ++k) {
            const int a0 = first + 2 * k;
            const int b0 = a0 + 1;
            const int a1 = first + 2 * ((k + 1) % sides);
            const int b1 = a1 + 1;
            triangle(a0, a1, b1, inside);
            triangle(a0, b1, b0, inside);
        }
    }

    // Конус (ярус хвои, пирамида): кольцо в base радиуса radius, вершина выше
    // на height; jitter — неровность кольца; under — с дном, приподнятым внутрь.
    void cone(Vector3 base, float radius, float height, int sides, float jitter, float part, Color side,
              Color under, bool withBottom) {
        const float twist = random_.next() * 2.0f * kPi;
        const int first = vertexCount();
        for (int k = 0; k < sides; ++k) {
            const float angle = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float r = radius * (1.0f + jitter * random_.signedNext());
            // Края яруса чуть свисают.
            const float droop = radius * jitter * 0.6f * random_.next();
            vertex({base.x + r * std::cos(angle), base.y - droop, base.z + r * std::sin(angle)}, part, side);
        }
        const int apex = vertex({base.x, base.y + height, base.z}, part, side);
        const Vector3 inside = {base.x, base.y + height * 0.3f, base.z};
        for (int k = 0; k < sides; ++k) {
            triangle(first + k, first + (k + 1) % sides, apex, inside);
        }
        if (!withBottom) {
            return;
        }
        const int center = vertex({base.x, base.y + radius * 0.15f, base.z}, part, under);
        const Vector3 above = {base.x, base.y + height * 0.6f, base.z};
        for (int k = 0; k < sides; ++k) {
            triangle(first + (k + 1) % sides, first + k, center, above);
        }
    }

    // Клуб кроны: шестигранный «шар» радиуса r по горизонтали и ry по высоте.
    void blob(Vector3 center, float r, float ry, float part, Color color) {
        const float twist = random_.next() * 2.0f * kPi;
        const int top = vertex({center.x, center.y + ry, center.z}, part, color);
        int rings[2][6];
        for (int ring = 0; ring < 2; ++ring) {
            const float y = center.y + (ring == 0 ? 0.35f : -0.35f) * ry;
            for (int k = 0; k < 6; ++k) {
                const float angle = twist + kPi / 3.0f * (static_cast<float>(k) + 0.5f * static_cast<float>(ring));
                const float radius = r * (0.95f + 0.18f * random_.signedNext());
                rings[ring][k] = vertex({center.x + radius * std::cos(angle), y + 0.1f * ry * random_.signedNext(),
                                         center.z + radius * std::sin(angle)},
                                        part, color);
            }
        }
        const int bottom = vertex({center.x, center.y - 0.8f * ry, center.z}, part, darker(color, 0.8f));
        for (int k = 0; k < 6; ++k) {
            const int next = (k + 1) % 6;
            triangle(top, rings[0][k], rings[0][next], center);
            triangle(rings[0][k], rings[1][k], rings[0][next], center);
            triangle(rings[0][next], rings[1][k], rings[1][next], center);
            triangle(bottom, rings[1][next], rings[1][k], center);
        }
    }

    // Ромб кроны издали: четыре вершины по кругу, верх и низ.
    void diamond(Vector3 center, float r, float ry, float part, Color color) {
        const float twist = random_.next() * 2.0f * kPi;
        const int top = vertex({center.x, center.y + ry, center.z}, part, color);
        const int bottom = vertex({center.x, center.y - 0.8f * ry, center.z}, part, darker(color, 0.8f));
        int ring[4];
        for (int k = 0; k < 4; ++k) {
            const float angle = twist + 0.5f * kPi * static_cast<float>(k);
            ring[k] = vertex({center.x + r * std::cos(angle), center.y, center.z + r * std::sin(angle)}, part, color);
        }
        for (int k = 0; k < 4; ++k) {
            triangle(top, ring[k], ring[(k + 1) % 4], center);
            triangle(bottom, ring[(k + 1) % 4], ring[k], center);
        }
    }

    // В видеопамять; на процессоре остаются только индексы (по ним DrawMesh
    // понимает, что сетка индексная).
    Mesh upload() const {
        Mesh mesh{};
        mesh.vertexCount = vertexCount();
        mesh.triangleCount = static_cast<int>(indices_.size() / 3);
        const auto copy = [](const auto& source) {
            using T = typename std::decay_t<decltype(source)>::value_type;
            auto* target = static_cast<T*>(MemAlloc(static_cast<unsigned>(source.size() * sizeof(T))));
            std::copy(source.begin(), source.end(), target);
            return target;
        };
        mesh.vertices = copy(positions_);
        mesh.normals = copy(roots_);
        mesh.texcoords = copy(texcoords_);
        mesh.colors = copy(colors_);
        mesh.indices = copy(indices_);
        UploadMesh(&mesh, false);
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        MemFree(mesh.texcoords);
        MemFree(mesh.colors);
        mesh.vertices = nullptr;
        mesh.normals = nullptr;
        mesh.texcoords = nullptr;
        mesh.colors = nullptr;
        return mesh;
    }

private:
    Vector3 position(int index) const {
        const std::size_t i = static_cast<std::size_t>(index) * 3;
        return {positions_[i], positions_[i + 1], positions_[i + 2]};
    }

    std::vector<float> positions_;
    std::vector<float> roots_;
    std::vector<float> texcoords_;
    std::vector<unsigned char> colors_;
    std::vector<unsigned short> indices_;
    TreeSpec tree_{};
    Vector3 root_{};
    float cosYaw_ = 1.0f;
    float sinYaw_ = 0.0f;
    ShapeRandom random_{0};
};

// --- Подробные модели ---------------------------------------------------------

void emitSpruce(TreeBuilder& b, const TreeSpec& tree) {
    ShapeRandom& r = b.random();
    const float h = tree.height;
    const float radius = tree.crown;
    b.prism({0.0f, -0.4f, 0.0f}, {0.0f, 0.7f * h, 0.0f}, 0.011f * h + 0.06f, 0.06f, 5, kWood, kSpruceBark,
            kSpruceBark);
    // Ярусы почти от земли: сквозь ельник видно недалеко.
    const int tiers = h > 8.0f ? 4 : 3;
    const float base = h * (0.04f + 0.06f * r.next());
    const float span = h - base;
    const Color needles = vary(kSpruceNeedles, r, 0.12f);
    for (int i = 0; i < tiers; ++i) {
        const float share = static_cast<float>(i) / static_cast<float>(tiers);
        const float y = base + span * 0.9f * share;
        const float top = i + 1 == tiers ? h : y + span * 1.9f / static_cast<float>(tiers);
        b.cone({0.0f, y, 0.0f}, radius * (1.0f - 0.75f * share), top - y, 7, 0.18f, kNeedles, needles,
               darker(needles, 0.55f), true);
    }
}

void emitPine(TreeBuilder& b, const TreeSpec& tree) {
    ShapeRandom& r = b.random();
    const float h = tree.height;
    const float radius = tree.crown;
    // Высокий голый ствол, крона — зонтиком наверху.
    b.prism({0.0f, -0.4f, 0.0f}, {0.0f, 0.82f * h, 0.0f}, 0.009f * h + 0.08f, 0.07f, 5, kWood, kPineBarkLow,
            kPineBark);
    const Color needles = vary(kPineNeedles, r, 0.12f);
    const float low = h * (0.58f + 0.06f * r.next());
    b.cone({0.0f, low, 0.0f}, radius, 0.26f * h, 7, 0.22f, kNeedles, needles, darker(needles, 0.55f), true);
    const float high = low + 0.15f * h;
    b.cone({0.0f, high, 0.0f}, radius * 0.72f, h - high, 6, 0.22f, kNeedles, needles, darker(needles, 0.55f),
           true);
}

void emitBroadleaf(TreeBuilder& b, const TreeSpec& tree) {
    ShapeRandom& r = b.random();
    const float h = tree.height;
    const float radius = tree.crown;
    const float trunk = 0.014f * h + 0.08f;
    b.prism({0.0f, -0.4f, 0.0f}, {0.0f, 0.55f * h, 0.0f}, trunk, 0.6f * trunk, 6, kWood, kBroadleafBark,
            kBroadleafBark);
    // Клубы кроны по кругу на ветвях и один — на верхушке.
    const Color green = vary(kBroadleafGreen, r, 0.14f);
    const int clumps = 3 + static_cast<int>(tree.bits & 1u);
    const float crownY = 0.62f * h;
    for (int i = 0; i < clumps; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(clumps) + 0.5f * r.signedNext();
        const float out = radius * (0.45f + 0.15f * r.next());
        const Vector3 center = {out * std::cos(angle), crownY + 0.1f * h * r.signedNext(), out * std::sin(angle)};
        const float size = radius * (0.55f + 0.15f * r.next());
        const Vector3 fork = {0.0f, h * (0.42f + 0.06f * r.next()), 0.0f};
        b.prism(fork, Vector3Lerp(fork, center, 0.85f), 0.45f * trunk, 0.05f, 4, kWood, kBroadleafBark,
                kBroadleafBark);
        b.blob(center, size, size * (0.75f + 0.2f * r.next()), kLeaves, green);
    }
    b.prism({0.0f, 0.5f * h, 0.0f}, {0.0f, 0.84f * h, 0.0f}, 0.5f * trunk, 0.05f, 4, kWood, kBroadleafBark,
            kBroadleafBark);
    const float top = radius * 0.6f;
    b.blob({0.0f, h - top * 0.55f, 0.0f}, top, top * 0.55f, kLeaves, green);
}

void emitBirch(TreeBuilder& b, const TreeSpec& tree) {
    ShapeRandom& r = b.random();
    const float h = tree.height;
    const float radius = tree.crown;
    const float trunk = 0.008f * h + 0.05f;
    // Внизу кора тёмная, выше — белая.
    b.prism({0.0f, -0.4f, 0.0f}, {0.0f, 0.12f * h, 0.0f}, trunk, 0.95f * trunk, 5, kWood, kBirchBarkLow,
            kBirchBarkLow);
    b.prism({0.0f, 0.12f * h, 0.0f}, {0.0f, 0.9f * h, 0.0f}, 0.95f * trunk, 0.04f, 5, kWood, kBirchBark,
            kBirchBark);
    const Color green = vary(kBirchGreen, r, 0.12f);
    constexpr float kHeights[] = {0.55f, 0.72f, 0.87f};
    for (int i = 0; i < 3; ++i) {
        const float angle = 2.3f * static_cast<float>(i) + r.signedNext();
        const float out = radius * 0.3f * (i == 2 ? 0.3f : 1.0f);
        const Vector3 center = {out * std::cos(angle), kHeights[i] * h, out * std::sin(angle)};
        if (i < 2) {
            const Vector3 fork = {0.0f, center.y - 0.12f * h, 0.0f};
            b.prism(fork, center, 0.5f * trunk, 0.03f, 4, kWood, kBirchBark, kBirchBark);
        }
        const float size = radius * (0.8f + 0.15f * r.next()) * (i == 2 ? 0.7f : 1.0f);
        b.blob(center, size, 0.12f * h, kBirchLeaves, green);
    }
}

void emitBush(TreeBuilder& b, const TreeSpec& tree) {
    ShapeRandom& r = b.random();
    const float h = tree.height;
    const float radius = tree.crown;
    const Color green = vary(kBushGreen, r, 0.15f);
    b.blob({0.0f, 0.5f * h, 0.0f}, radius, 0.5f * h, kLeaves, green);
    if ((tree.bits & 2u) != 0u) {
        b.blob({0.5f * radius, 0.4f * h, 0.3f * radius}, 0.7f * radius, 0.4f * h, kLeaves, green);
    }
}

void emitNear(TreeBuilder& b, const TreeSpec& tree) {
    switch (tree.kind) {
    case TreeKind::Spruce:
        emitSpruce(b, tree);
        break;
    case TreeKind::Pine:
        emitPine(b, tree);
        break;
    case TreeKind::Broadleaf:
        emitBroadleaf(b, tree);
        break;
    case TreeKind::Birch:
        emitBirch(b, tree);
        break;
    case TreeKind::Bush:
        emitBush(b, tree);
        break;
    }
}

// --- Упрощённые: пирамида или ромб на стволе ---------------------------------

void emitFar(TreeBuilder& b, const TreeSpec& tree) {
    ShapeRandom& r = b.random();
    const float h = tree.height;
    const float radius = tree.crown;
    // Ствол вдали толще настоящего: иначе тоньше пикселя и мерцает.
    const float trunk = 0.03f * h + 0.1f;
    switch (tree.kind) {
    case TreeKind::Spruce: {
        const Color needles = vary(kSpruceNeedles, r, 0.12f);
        b.cone({0.0f, 0.05f * h, 0.0f}, radius, 0.95f * h, 4, 0.0f, kNeedles, needles, needles, false);
        break;
    }
    case TreeKind::Pine: {
        const Color needles = vary(kPineNeedles, r, 0.12f);
        b.prism({0.0f, -0.3f, 0.0f}, {0.0f, 0.65f * h, 0.0f}, trunk, trunk * 0.7f, 3, kWood, kPineBarkLow,
                kPineBark);
        b.cone({0.0f, 0.6f * h, 0.0f}, radius, 0.4f * h, 4, 0.0f, kNeedles, needles, darker(needles, 0.55f), true);
        break;
    }
    case TreeKind::Broadleaf: {
        const Color green = vary(kBroadleafGreen, r, 0.14f);
        b.prism({0.0f, -0.3f, 0.0f}, {0.0f, 0.5f * h, 0.0f}, trunk, trunk * 0.7f, 3, kWood, kBroadleafBark,
                kBroadleafBark);
        b.diamond({0.0f, 0.62f * h, 0.0f}, radius, 0.36f * h, kLeaves, green);
        break;
    }
    case TreeKind::Birch: {
        const Color green = vary(kBirchGreen, r, 0.12f);
        b.prism({0.0f, -0.3f, 0.0f}, {0.0f, 0.55f * h, 0.0f}, trunk, trunk * 0.7f, 3, kWood, kBirchBark,
                kBirchBark);
        b.diamond({0.0f, 0.7f * h, 0.0f}, radius, 0.28f * h, kBirchLeaves, green);
        break;
    }
    case TreeKind::Bush:
        break; // вдали кустов не видно
    }
}

float distanceToBox(Vector3 p, Vector3 min, Vector3 max) {
    const float dx = std::max({min.x - p.x, 0.0f, p.x - max.x});
    const float dy = std::max({min.y - p.y, 0.0f, p.y - max.y});
    const float dz = std::max({min.z - p.z, 0.0f, p.z - max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float farthestOfBox(Vector3 p, Vector3 min, Vector3 max) {
    const float dx = std::max(std::abs(p.x - min.x), std::abs(p.x - max.x));
    const float dy = std::max(std::abs(p.y - min.y), std::abs(p.y - max.y));
    const float dz = std::max(std::abs(p.z - min.z), std::abs(p.z - max.z));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

Trees3D::Trees3D(const Map& map, const Terrain3D& terrain) : map_(map), terrain_(terrain) {
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
                    if (map.cover.at(x, y) == Cover::Forest) {
                        chunk.forest = true;
                        low = std::min(low, map.elevationMeters({x, y}));
                        high = std::max(high, map.elevationMeters({x, y}));
                    }
                }
            }
            if (!chunk.forest) {
                continue;
            }
            chunk.min = {static_cast<float>(chunk.left) * Map::kCellSize - kCrownReach, low - 2.0f,
                         static_cast<float>(chunk.top) * Map::kCellSize - kCrownReach};
            chunk.max = {static_cast<float>(chunk.left + chunk.width) * Map::kCellSize + kCrownReach,
                         high + kTallest,
                         static_cast<float>(chunk.top + chunk.height) * Map::kCellSize + kCrownReach};
        }
    }

    shader_ = SceneShading::loadShader(kTreeVs, kTreeFs);
    const std::string shadowFs = std::string("#define SHADOW_PASS\n") + kTreeFs;
    shadowShader_ = SceneShading::loadShader(kTreeVs, shadowFs.c_str());
    lodLoc_ = GetShaderLocation(shader_, "lod");
    nearLoc_ = GetShaderLocation(shader_, "nearRange");
    shadowLodLoc_ = GetShaderLocation(shadowShader_, "lod");
    shadowNearLoc_ = GetShaderLocation(shadowShader_, "nearRange");
    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    shadowMaterial_ = LoadMaterialDefault();
    shadowMaterial_.shader = shadowShader_;
}

Trees3D::~Trees3D() {
    for (Chunk& chunk : chunks_) {
        unload(chunk.far);
        for (Part& part : chunk.near) {
            unload(part);
        }
    }
    UnloadShader(shader_);
    UnloadShader(shadowShader_);
    MemFree(material_.maps);
    MemFree(shadowMaterial_.maps);
}

void Trees3D::buildFar(Chunk& chunk) {
    const double start = GetTime();
    TreeBuilder builder;
    TreeSpec trees[kMaxTreesPerCell];
    int count = 0;
    for (int y = chunk.top; y < chunk.top + chunk.height && builder.hasRoom(); ++y) {
        for (int x = chunk.left; x < chunk.left + chunk.width && builder.hasRoom(); ++x) {
            const int n = treesInCell(map_, {x, y}, trees);
            for (int i = 0; i < n && builder.hasRoom(); ++i) {
                if (trees[i].kind == TreeKind::Bush) {
                    continue;
                }
                builder.begin(trees[i], terrain_.groundAt(trees[i].x, trees[i].z));
                emitFar(builder, trees[i]);
                ++count;
            }
        }
    }
    chunk.far.built = true;
    chunk.far.trees = count;
    cpuTime_ += GetTime() - start;
    if (!builder.empty()) {
        chunk.far.mesh = builder.upload();
    }
    ++meshesLoaded_;
}

void Trees3D::buildNear(Chunk& chunk, int block) {
    const double start = GetTime();
    constexpr int kPerRow = kChunkCells / kBlockCells;
    const int left = chunk.left + (block % kPerRow) * kBlockCells;
    const int top = chunk.top + (block / kPerRow) * kBlockCells;
    const int right = std::min(left + kBlockCells, chunk.left + chunk.width);
    const int bottom = std::min(top + kBlockCells, chunk.top + chunk.height);
    TreeBuilder builder;
    TreeSpec trees[kMaxTreesPerCell];
    int count = 0;
    for (int y = top; y < bottom && builder.hasRoom(); ++y) {
        for (int x = left; x < right && builder.hasRoom(); ++x) {
            const int n = treesInCell(map_, {x, y}, trees);
            for (int i = 0; i < n && builder.hasRoom(); ++i) {
                builder.begin(trees[i], terrain_.groundAt(trees[i].x, trees[i].z));
                emitNear(builder, trees[i]);
                ++count;
            }
        }
    }
    Part& part = chunk.near[static_cast<std::size_t>(block)];
    part.built = true;
    part.trees = count;
    cpuTime_ += GetTime() - start;
    if (!builder.empty()) {
        part.mesh = builder.upload();
    }
    ++meshesLoaded_;
}

void Trees3D::unload(Part& part) {
    if (!part.built) {
        return;
    }
    if (part.mesh.vertexCount > 0) {
        UnloadMesh(part.mesh);
    }
    part = Part{};
    --meshesLoaded_;
}

void Trees3D::update(const Camera3D& camera, float nearRange, float farRange) {
    ++frame_;
    farRange_ = farRange;
    // Подробные — не дальше середины дальности: иначе им негде смениться упрощёнными.
    nearRange_ = std::min(nearRange, 0.5f * farRange);
    const Vector3 eye = camera.position;
    constexpr int kPerRow = kChunkCells / kBlockCells;
    constexpr float kBlockSize = static_cast<float>(kBlockCells) * Map::kCellSize;

    // Чего не хватает: расстояние и что строить (блок −1 — упрощённая сетка).
    struct Wanted {
        float distance = 0.0f;
        Chunk* chunk = nullptr;
        int block = -1;
    };
    std::vector<Wanted> missing;
    for (Chunk& chunk : chunks_) {
        chunk.drawFar = false;
        chunk.drawNear = false;
        chunk.farOnly = false;
        if (!chunk.forest) {
            continue;
        }
        const float distance = distanceToBox(eye, chunk.min, chunk.max);
        if (distance >= farRange_) {
            continue;
        }
        bool wantNear = false;
        bool allNear = true;
        if (distance < nearRange_) {
            for (int block = 0; block < kBlocks; ++block) {
                const float x0 = static_cast<float>(chunk.left + (block % kPerRow) * kBlockCells) * Map::kCellSize;
                const float z0 = static_cast<float>(chunk.top + (block / kPerRow) * kBlockCells) * Map::kCellSize;
                const Vector3 min = {x0 - kCrownReach, chunk.min.y, z0 - kCrownReach};
                const Vector3 max = {x0 + kBlockSize + kCrownReach, chunk.max.y, z0 + kBlockSize + kCrownReach};
                const float near = distanceToBox(eye, min, max);
                if (near >= nearRange_) {
                    continue;
                }
                Part& part = chunk.near[static_cast<std::size_t>(block)];
                wantNear = true;
                part.lastUsed = frame_;
                if (!part.built) {
                    allNear = false;
                    missing.push_back({near, &chunk, block});
                }
            }
        }
        // Упрощённые нужны, если часть деревьев дальше подробных, и пока подробные не готовы.
        const bool wantFar = farthestOfBox(eye, chunk.min, chunk.max) > 0.85f * nearRange_ || !allNear;
        if (wantFar) {
            chunk.far.lastUsed = frame_;
            if (!chunk.far.built) {
                missing.push_back({distance, &chunk, -1});
            }
        }
        chunk.drawNear = wantNear && allNear;
        chunk.farOnly = wantNear && !allNear;
        chunk.drawFar = wantFar;
    }

    // Ближнее — первым, пока не кончится время кадра.
    std::sort(missing.begin(), missing.end(),
              [](const Wanted& a, const Wanted& b) { return a.distance < b.distance; });
    cpuTime_ = 0.0;
    pending_ = static_cast<int>(missing.size());
    for (const Wanted& wanted : missing) {
        --pending_;
        if (wanted.block < 0) {
            buildFar(*wanted.chunk);
        } else {
            buildNear(*wanted.chunk, wanted.block);
        }
        if (cpuTime_ > kBuildBudget) {
            break;
        }
    }
    // Достроенные в этом кадре подробные можно рисовать сразу.
    for (Chunk& chunk : chunks_) {
        if (!chunk.farOnly) {
            continue;
        }
        bool ready = true;
        for (const Part& part : chunk.near) {
            ready = ready && (part.lastUsed != frame_ || part.built);
        }
        if (ready) {
            chunk.farOnly = false;
            chunk.drawNear = true;
        }
    }

    for (Chunk& chunk : chunks_) {
        if (chunk.far.built && frame_ - chunk.far.lastUsed > kEvictFrames) {
            unload(chunk.far);
        }
        for (Part& part : chunk.near) {
            if (part.built && frame_ - part.lastUsed > kEvictFrames) {
                unload(part);
            }
        }
    }
}

void Trees3D::setUniforms(const Shader& shader, const Look& look) const {
    const auto set = [&shader](const char* name, const void* value, int type) {
        SetShaderValue(shader, GetShaderLocation(shader, name), value, type);
    };
    set("year", &look.year, SHADER_UNIFORM_FLOAT);
    set("time", &look.time, SHADER_UNIFORM_FLOAT);
    set("snow", &look.snow, SHADER_UNIFORM_FLOAT);
    set("wind", &look.wind, SHADER_UNIFORM_VEC2);
    set("farRange", &farRange_, SHADER_UNIFORM_FLOAT);
}

template <typename Inside>
void Trees3D::drawChunks(const Shader& shader, Material& material, Inside inside, bool count) {
    const int lodLoc = &shader == &shader_ ? lodLoc_ : shadowLodLoc_;
    const int nearLoc = &shader == &shader_ ? nearLoc_ : shadowNearLoc_;
    constexpr float kNear = 0.0f;
    constexpr float kFar = 1.0f;
    constexpr float kNoNear = 0.0f;
    if (count) {
        treesDrawn_ = 0;
        meshesDrawn_ = 0;
    }
    for (const Chunk& chunk : chunks_) {
        if ((!chunk.drawFar && !chunk.drawNear) || !inside(chunk.min, chunk.max)) {
            continue;
        }
        if (chunk.drawNear) {
            SetShaderValue(shader, lodLoc, &kNear, SHADER_UNIFORM_FLOAT);
            SetShaderValue(shader, nearLoc, &nearRange_, SHADER_UNIFORM_FLOAT);
            for (const Part& part : chunk.near) {
                if (part.built && part.lastUsed == frame_ && part.mesh.vertexCount > 0) {
                    DrawMesh(part.mesh, material, MatrixIdentity());
                    if (count) {
                        treesDrawn_ += part.trees;
                        ++meshesDrawn_;
                    }
                }
            }
        }
        if (chunk.drawFar && chunk.far.built && chunk.far.mesh.vertexCount > 0) {
            SetShaderValue(shader, lodLoc, &kFar, SHADER_UNIFORM_FLOAT);
            SetShaderValue(shader, nearLoc, chunk.farOnly ? &kNoNear : &nearRange_, SHADER_UNIFORM_FLOAT);
            DrawMesh(chunk.far.mesh, material, MatrixIdentity());
            if (count) {
                ++meshesDrawn_;
                treesDrawn_ += chunk.drawNear ? 0 : chunk.far.trees;
            }
        }
    }
}

void Trees3D::draw(const SceneShading& shading, const Look& look) {
    // Внутри BeginMode3D модельно-видовая матрица — это вид камеры.
    const Matrix viewProjection = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    shading.apply(shader_);
    setUniforms(shader_, look);
    drawChunks(shader_, material_,
               [&viewProjection](Vector3 min, Vector3 max) { return !outsideFrustum(viewProjection, min, max); },
               true);
}

void Trees3D::drawShadows(const SceneShading& shading, const Look& look) {
    shading.apply(shadowShader_);
    setUniforms(shadowShader_, look);
    drawChunks(shadowShader_, shadowMaterial_,
               [&shading](Vector3 min, Vector3 max) {
                   const Vector3 center = Vector3Scale(Vector3Add(min, max), 0.5f);
                   return shading.inShadowArea(center, 0.5f * std::max(max.x - min.x, max.z - min.z));
               },
               false);
}

} // namespace lw
