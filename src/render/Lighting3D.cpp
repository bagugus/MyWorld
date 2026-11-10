#include "render/Lighting3D.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lw {

namespace {

// Карта теней: 4096² на квадрат до 2 × 1600 м — вблизи тексель в сантиметры.
constexpr int kShadowSize = 4096;
// Шаг направления света для карты теней, градусов.
constexpr float kShadowDirStep = 0.2f;
// Детальная текстура: шум четырёх видов в каналах, повторяется без швов.
constexpr int kDetailSize = 256;

// Общий кусок шейдеров 3D-вида: свет, тень и туман.
constexpr const char* kCommonGlsl = R"(
uniform vec3 viewPos;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambientSky;
uniform vec3 ambientGround;
uniform vec3 fogColor;
uniform float fogDensity;
uniform sampler2D shadowMap;
uniform sampler2D detailMap;
uniform mat4 lightVP;
uniform float shadowStrength;
uniform float shadowTexel; // метров на тексель карты теней
uniform float shadowRange; // глубина проекции света, метров
// Цвет кадра по настроению (кино, этап 12а.12): rgb — добавка к единице по
// каналам, a — доля серого; x — контраст, y — виньетка, zw — размер экрана в
// пикселях (0 — без цвета кадра).
uniform vec4 gradeTint;
uniform vec4 gradeFrame;

// 1 — точка на свету, меньше — в тени. n — нормаль поверхности.
float shadowAt(vec3 p, vec3 n) {
    if (shadowStrength <= 0.0) {
        return 1.0;
    }
    float cosine = clamp(dot(n, lightDir), 0.0, 1.0);
    // Сдвиг вдоль нормали и наклон — против «угрей» на освещённых гранях.
    vec4 clip = lightVP * vec4(p + n * (1.5 * shadowTexel), 1.0);
    vec3 s = clip.xyz / clip.w * 0.5 + 0.5;
    if (s.x <= 0.0 || s.y <= 0.0 || s.x >= 1.0 || s.y >= 1.0 || s.z >= 1.0) {
        return 1.0;
    }
    float slope = sqrt(1.0 - cosine * cosine) / max(cosine, 0.15);
    float bias = (0.05 + shadowTexel * min(slope, 6.0)) / shadowRange;
    // PCF с билинейными весами: девять выборок 2×2, сдвинутых на тексель, —
    // это 4×4 текселя, крайние с весом по дробной части. Край тени сдвигается
    // плавно вместе с точкой, а не ступенькой в тексель: не мерцает, когда
    // камера или солнце двигаются.
    ivec2 size = textureSize(shadowMap, 0);
    vec2 t = s.xy * vec2(size) - 0.5;
    vec2 cell = floor(t);
    vec2 f = t - cell;
    float reference = s.z - bias;
    float lit = 0.0;
    for (int y = -1; y <= 2; ++y) {
        float wy = y == -1 ? 1.0 - f.y : (y == 2 ? f.y : 1.0);
        for (int x = -1; x <= 2; ++x) {
            float wx = x == -1 ? 1.0 - f.x : (x == 2 ? f.x : 1.0);
            ivec2 texel = clamp(ivec2(cell) + ivec2(x, y), ivec2(0), size - 1);
            lit += wx * wy * step(reference, texelFetch(shadowMap, texel, 0).r);
        }
    }
    lit /= 9.0;
    // К краю карты теней тень тает, а не обрывается.
    float edge = smoothstep(0.0, 0.08, min(min(s.x, 1.0 - s.x), min(s.y, 1.0 - s.y)));
    return 1.0 - shadowStrength * (1.0 - lit) * edge;
}

// Во сколько раз освещена поверхность с нормалью n: рассеянный свет неба
// сверху и земли снизу плюс прямой.
vec3 lighting(vec3 n, float shadow) {
    vec3 ambient = mix(ambientGround, ambientSky, n.y * 0.5 + 0.5);
    return ambient + lightColor * max(dot(n, lightDir), 0.0) * shadow;
}

// Цвет кадра: выцветание, оттенок, контраст (S-кривая) и тёмные углы.
vec3 graded(vec3 c) {
    if (gradeFrame.z <= 0.0) {
        return c;
    }
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(c, vec3(luma), gradeTint.a);
    c *= vec3(1.0) + gradeTint.rgb;
    vec3 s = clamp(c, 0.0, 1.0);
    c = mix(c, s * s * (3.0 - 2.0 * s), gradeFrame.x);
    float aspect = gradeFrame.z / max(gradeFrame.w, 1.0);
    vec2 uv = (gl_FragCoord.xy / gradeFrame.zw - 0.5) * vec2(aspect, 1.0);
    float r = length(uv) / length(vec2(0.5 * aspect, 0.5));
    c *= 1.0 - gradeFrame.y * smoothstep(0.35, 1.05, r);
    return max(c, vec3(0.0));
}

// Дымка и туман: дальнее тонет в цвете горизонта.
vec3 withFog(vec3 color, vec3 p) {
    float distance = length(p - viewPos);
    return graded(mix(color, fogColor, 1.0 - exp(-fogDensity * distance)));
}
)";

// Модели: цвет вершины, нормаль грани; texcoord.x = 1 — светится сам (окна ночью),
// texcoord.y — доля растворения у края дальности прорисовки (пиксели выпадают
// по матрице Байера 4×4: модель тает, а не исчезает разом).
constexpr const char* kModelVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
out vec3 fragPosition;
out vec3 fragNormal;
out vec4 fragColor;
out float fragGlow;
out float fragDissolve;
void main() {
    fragPosition = vertexPosition;
    fragNormal = vertexNormal;
    fragColor = vertexColor;
    fragGlow = vertexTexCoord.x;
    fragDissolve = vertexTexCoord.y;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kModelFs = R"(
in vec3 fragPosition;
in vec3 fragNormal;
in vec4 fragColor;
in float fragGlow;
in float fragDissolve;
out vec4 finalColor;

// Внутри помещения (этап 12а.14): рассеянный свет неба приглушён (texcoord.x = 2),
// лампы — точечный тёплый свет с мягким спадом.
uniform float indoorAmbient;
uniform int lampCount;
uniform vec4 lampPos[4];   // xyz и радиус, метры
uniform vec3 lampColor[4];

// bounce — доля отражённого света: в комнате стены и потолок подсвечивают
// друг друга, поэтому и потолок над абажуром не чёрный.
vec3 lamps(vec3 p, vec3 n, float bounce) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < lampCount; ++i) {
        vec3 d = lampPos[i].xyz - p;
        float distance = length(d);
        float fall = clamp(1.0 - distance / lampPos[i].w, 0.0, 1.0);
        float wrap = dot(n, d / max(distance, 1e-3)) * 0.6 + 0.4;
        float fill = clamp(1.0 - distance / (1.4 * lampPos[i].w), 0.0, 1.0);
        sum += lampColor[i] * (fall * fall * max(wrap, 0.0) + bounce * fill);
    }
    return sum;
}

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main() {
    if (fragDissolve > 0.0) {
        ivec2 cell = ivec2(mod(gl_FragCoord.xy, 4.0));
        if ((kBayer[cell.y * 4 + cell.x] + 0.5) / 16.0 < fragDissolve) {
            discard;
        }
    }
    vec3 color;
    if (fragGlow > 0.5 && fragGlow < 1.5) {
        color = fragColor.rgb;
    } else {
        vec3 n = normalize(fragNormal);
        float shadow = shadowAt(fragPosition, n);
        vec3 light;
        float bounce = 0.0;
        if (fragGlow > 1.5) {
            // Внутри небо видно только в окна: рассеянный свет слабее и без
            // синевы снизу (пол и потолок освещены одинаково).
            // Днём солнце, отражённое от пола и стен, светит через окна во всю комнату.
            vec3 sky = mix(ambientGround, ambientSky, 0.6) * indoorAmbient + lightColor * 0.22 +
                       vec3(0.07, 0.08, 0.12); // ночью в комнате различимо, как в кино
            light = sky * (0.8 + 0.2 * n.y) + lightColor * max(dot(n, lightDir), 0.0) * shadow;
            bounce = 0.22;
        } else {
            light = lighting(n, shadow);
        }
        color = fragColor.rgb * (light + lamps(fragPosition, n, bounce));
    }
    finalColor = vec4(withFog(color, fragPosition), fragColor.a);
}
)";

// Небо: направление взгляда по пикселю — из обратной матрицы камеры.
constexpr const char* kSkyVs = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
void main() {
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kSkyFs = R"(
uniform mat4 invViewProj;
uniform vec2 screenSize;
uniform vec3 zenith;
uniform vec3 horizon;
uniform vec3 sunDir;
uniform vec3 sunColor;
uniform vec3 moonDir;
uniform float moonFullness;
uniform float night;
uniform float cloudCover;
uniform vec2 cloudShift;
out vec4 finalColor;

float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void main() {
    vec2 ndc = gl_FragCoord.xy / screenSize * 2.0 - 1.0;
    vec4 far = invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - viewPos);
    float up = max(dir.y, 0.0);
    vec3 sky = mix(horizon, zenith, pow(up, 0.45));

    // Ореол солнца — и под облаками светлее.
    float sunUp = smoothstep(-0.12, 0.02, sunDir.y);
    float toward = max(dot(dir, sunDir), 0.0);
    sky += sunColor * (0.22 * pow(toward, 6.0) + 0.3 * pow(toward, 80.0)) * sunUp * (1.0 - 0.6 * cloudCover);

    // Звёзды — ночью в ясном небе: в редких ячейках сетки направлений — точка
    // в случайном месте ячейки.
    if (night > 0.0) {
        vec3 grid = dir * 420.0;
        vec3 cell = floor(grid);
        vec3 center = cell + 0.25 + 0.5 * vec3(hash(cell + 1.0), hash(cell + 2.0), hash(cell + 3.0));
        // Звезда не мельче пикселя: иначе при повороте камеры она то попадает
        // в пиксель, то нет, и небо мигает. Шире — тусклее: яркость та же.
        float pixel = length(fwidth(grid));
        float radius = max(0.3, 0.9 * pixel);
        float point = (1.0 - smoothstep(0.08, radius, length(grid - center))) * min(1.0, 0.3 / radius);
        float star = step(0.993, hash(cell)) * point * (0.35 + 0.65 * hash(cell + 7.0));
        sky += vec3(star * night * (1.0 - cloudCover) * smoothstep(0.0, 0.1, dir.y));
    }

    // Диски солнца и луны за облаками.
    float sunDisc = smoothstep(0.99955, 0.99975, toward) * sunUp;
    float moonDisc = smoothstep(0.99965, 0.9998, dot(dir, moonDir)) * smoothstep(-0.02, 0.03, moonDir.y);

    // Облака — слой на высоте: шум детальной текстуры, плывёт по ветру.
    float cloud = 0.0;
    if (dir.y > 0.0 && cloudCover > 0.02) {
        vec2 p = dir.xz / max(dir.y, 0.02) * 1500.0 + cloudShift;
        float n = 0.6 * texture(detailMap, p / 24000.0).g + 0.4 * texture(detailMap, p / 7000.0).r;
        cloud = smoothstep(1.05 - cloudCover, 1.3 - cloudCover, n + 0.25) * smoothstep(0.0, 0.03, dir.y);
    }
    vec3 lit = ambientSky * 1.25 + lightColor * 0.55 * (0.6 + 0.4 * toward);
    vec3 cloudColor = mix(lit, lit * 0.62, cloudCover * cloudCover);
    sky = mix(sky, sunColor * 1.6, sunDisc * (1.0 - cloud));
    sky = mix(sky, vec3(0.86, 0.88, 0.94) * (0.35 + 0.65 * moonFullness), moonDisc * (1.0 - cloud) * night);
    sky = mix(sky, cloudColor, cloud * 0.92);

    // Дымка и туман у горизонта гуще: путь сквозь нижний слой воздуха длиннее.
    float fog = 1.0 - exp(-fogDensity * 1000.0 / max(dir.y + 0.08, 0.08));
    finalColor = vec4(graded(mix(sky, fogColor, clamp(fog, 0.0, 1.0))), 1.0);
}
)";

// Тайлящийся шум значений: решётка cells × cells, повторяется по краям.
std::vector<float> valueNoise(int size, int cells, int octaves, std::uint32_t seed) {
    const auto lattice = [seed](int x, int y, int period, int octave) {
        x = ((x % period) + period) % period;
        y = ((y % period) + period) % period;
        std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u +
                          seed * 2246822519u + static_cast<std::uint32_t>(octave) * 3266489917u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFu) / 65535.0f;
    };
    std::vector<float> result(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0.0f);
    float amplitude = 1.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        const int period = cells << octave;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float gx = static_cast<float>(x) * static_cast<float>(period) / static_cast<float>(size);
                const float gy = static_cast<float>(y) * static_cast<float>(period) / static_cast<float>(size);
                const int ix = static_cast<int>(gx);
                const int iy = static_cast<int>(gy);
                float u = gx - static_cast<float>(ix);
                float v = gy - static_cast<float>(iy);
                u = u * u * (3.0f - 2.0f * u);
                v = v * v * (3.0f - 2.0f * v);
                const float a = lattice(ix, iy, period, octave);
                const float b = lattice(ix + 1, iy, period, octave);
                const float c = lattice(ix, iy + 1, period, octave);
                const float d = lattice(ix + 1, iy + 1, period, octave);
                result[static_cast<std::size_t>(y * size + x)] +=
                    amplitude * ((a + (b - a) * u) * (1.0f - v) + (c + (d - c) * u) * v);
            }
        }
        amplitude *= 0.5f;
    }
    // В 0..1.
    const auto [low, high] = std::minmax_element(result.begin(), result.end());
    const float min = *low;
    const float span = std::max(*high - min, 1e-6f);
    for (float& value : result) {
        value = (value - min) / span;
    }
    return result;
}

Texture2D makeDetailTexture() {
    // R — мелкая зернистость, G — крупные пятна, B и A — независимый шум для
    // рваных краёв клеток.
    const std::vector<float> channels[4] = {
        valueNoise(kDetailSize, 8, 5, 1u),
        valueNoise(kDetailSize, 4, 4, 2u),
        valueNoise(kDetailSize, 16, 2, 3u),
        valueNoise(kDetailSize, 16, 2, 4u),
    };
    std::vector<Color> pixels(channels[0].size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const auto byte = [](float value) { return static_cast<unsigned char>(std::lround(value * 255.0f)); };
        pixels[i] = {byte(channels[0][i]), byte(channels[1][i]), byte(channels[2][i]), byte(channels[3][i])};
    }
    const Image image = {pixels.data(), kDetailSize, kDetailSize, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D texture = LoadTextureFromImage(image);
    GenTextureMipmaps(&texture);
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    rlTextureParameters(texture.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(texture.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_MIP_LINEAR);
    rlTextureParameters(texture.id, RL_TEXTURE_FILTER_ANISOTROPIC, 8);
    return texture;
}

void setVec3(const Shader& shader, const char* name, Vector3 value) {
    SetShaderValue(shader, GetShaderLocation(shader, name), &value, SHADER_UNIFORM_VEC3);
}

void setFloat(const Shader& shader, const char* name, float value) {
    SetShaderValue(shader, GetShaderLocation(shader, name), &value, SHADER_UNIFORM_FLOAT);
}

} // namespace

Shader SceneShading::loadShader(const char* vertex, const char* fragmentBody) {
    const std::string fragment = std::string("#version 330\n") + kCommonGlsl + fragmentBody;
    return LoadShaderFromMemory(vertex, fragment.c_str());
}

SceneShading::SceneShading() {
    modelShader_ = loadShader(kModelVs, kModelFs);
    lampCountLoc_ = GetShaderLocation(modelShader_, "lampCount");
    lampPosLoc_ = GetShaderLocation(modelShader_, "lampPos");
    lampColorLoc_ = GetShaderLocation(modelShader_, "lampColor");
    indoorAmbientLoc_ = GetShaderLocation(modelShader_, "indoorAmbient");
    skyShader_ = loadShader(kSkyVs, kSkyFs);
    detail_ = makeDetailTexture();

    // Карта теней — только глубина.
    shadowTarget_.id = rlLoadFramebuffer();
    shadowDepth_ = rlLoadTextureDepth(kShadowSize, kShadowSize, false);
    rlFramebufferAttach(shadowTarget_.id, shadowDepth_, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(shadowTarget_.id)) {
        TraceLog(LOG_WARNING, "SHADOW: framebuffer incomplete, shadows disabled");
        shadowDepth_ = 0;
    }
    shadowTarget_.texture.width = kShadowSize;
    shadowTarget_.texture.height = kShadowSize;
    shadowTarget_.depth.id = shadowDepth_;
    shadowTarget_.depth.width = kShadowSize;
    shadowTarget_.depth.height = kShadowSize;
}

SceneShading::~SceneShading() {
    UnloadShader(modelShader_);
    UnloadShader(skyShader_);
    UnloadTexture(detail_);
    rlUnloadFramebuffer(shadowTarget_.id);
    if (shadowDepth_ != 0) {
        rlUnloadTexture(shadowDepth_);
    }
}

void SceneShading::setLight(const SceneLight& light, Vector3 eye, float fogScale) {
    light_ = light;
    eye_ = eye;
    fogDensity_ = light.fogDensity * fogScale;
    shadowsOn_ = false;
}

const SceneShading::Uniforms& SceneShading::uniformsOf(const Shader& shader) const {
    const auto found = uniforms_.find(shader.id);
    if (found != uniforms_.end()) {
        return found->second;
    }
    Uniforms u;
    u.viewPos = GetShaderLocation(shader, "viewPos");
    u.lightDir = GetShaderLocation(shader, "lightDir");
    u.lightColor = GetShaderLocation(shader, "lightColor");
    u.ambientSky = GetShaderLocation(shader, "ambientSky");
    u.ambientGround = GetShaderLocation(shader, "ambientGround");
    u.fogColor = GetShaderLocation(shader, "fogColor");
    u.fogDensity = GetShaderLocation(shader, "fogDensity");
    u.shadowMap = GetShaderLocation(shader, "shadowMap");
    u.detailMap = GetShaderLocation(shader, "detailMap");
    u.shadowStrength = GetShaderLocation(shader, "shadowStrength");
    u.shadowTexel = GetShaderLocation(shader, "shadowTexel");
    u.shadowRange = GetShaderLocation(shader, "shadowRange");
    u.lightVP = GetShaderLocation(shader, "lightVP");
    u.gradeTint = GetShaderLocation(shader, "gradeTint");
    u.gradeFrame = GetShaderLocation(shader, "gradeFrame");
    return uniforms_.emplace(shader.id, u).first->second;
}

void SceneShading::apply(const Shader& shader) const {
    // Места униформ — один раз на шейдер: поиск по имени в драйвере дорог, а
    // apply зовётся несколько раз за кадр (этап 2а.2).
    const Uniforms& u = uniformsOf(shader);
    const float fogDensity = fogDensity_;
    const float shadowStrength = shadowsOn_ ? light_.shadowStrength : 0.0f;
    const float shadowTexel = 2.0f * shadowRadius_ / static_cast<float>(kShadowSize);
    const int shadowUnit = kShadowUnit;
    const int detailUnit = kDetailUnit;
    SetShaderValue(shader, u.viewPos, &eye_, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, u.lightDir, &light_.lightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, u.lightColor, &light_.lightColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, u.ambientSky, &light_.ambientSky, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, u.ambientGround, &light_.ambientGround, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, u.fogColor, &light_.fogColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, u.fogDensity, &fogDensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, u.shadowMap, &shadowUnit, SHADER_UNIFORM_INT);
    SetShaderValue(shader, u.detailMap, &detailUnit, SHADER_UNIFORM_INT);
    SetShaderValue(shader, u.shadowStrength, &shadowStrength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, u.shadowTexel, &shadowTexel, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, u.shadowRange, &shadowRange_, SHADER_UNIFORM_FLOAT);
    SetShaderValueMatrix(shader, u.lightVP, lightViewProj_);
    const float tint[4] = {grade_.tint.x, grade_.tint.y, grade_.tint.z, grade_.desaturate};
    const bool graded = grade_.desaturate > 0.0f || grade_.contrast > 0.0f || grade_.vignette > 0.0f ||
                        Vector3Length(grade_.tint) > 0.0f;
    const float frame[4] = {grade_.contrast, grade_.vignette,
                            graded ? static_cast<float>(GetRenderWidth()) : 0.0f,
                            graded ? static_cast<float>(GetRenderHeight()) : 0.0f};
    SetShaderValue(shader, u.gradeTint, tint, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader, u.gradeFrame, frame, SHADER_UNIFORM_VEC4);
}

bool SceneShading::beginShadowPass(Vector3 center, float radius) {
    // Направление света для теней меняется ступенями: солнце движется каждый
    // кадр, и карта теней, перерисованная под чуть другим углом, дрожала бы
    // краями. Шаг мал — тень переступает на сантиметры, глазу не видно.
    if (Vector3DotProduct(shadowDir_, light_.lightDir) < std::cos(kShadowDirStep * DEG2RAD)) {
        shadowDir_ = light_.lightDir;
    }
    const Vector3 toLight = shadowDir_;
    shadowsOn_ = shadowDepth_ != 0 && light_.shadowStrength > 0.01f && radius > 0.0f && toLight.y > 0.02f;
    if (!shadowsOn_) {
        return false;
    }
    // Оси взгляда света; центр сдвигается на целые тексели — тени не дрожат,
    // когда камера плывёт.
    const Vector3 forward = Vector3Negate(toLight);
    Vector3 right = Vector3CrossProduct(forward, {0.0f, 1.0f, 0.0f});
    right = Vector3Length(right) < 1e-3f ? Vector3{1.0f, 0.0f, 0.0f} : Vector3Normalize(right);
    const Vector3 up = Vector3CrossProduct(right, forward);
    const float texel = 2.0f * radius / static_cast<float>(kShadowSize);
    const float alongRight = Vector3DotProduct(center, right);
    const float alongUp = Vector3DotProduct(center, up);
    center = Vector3Add(center, Vector3Scale(right, std::round(alongRight / texel) * texel - alongRight));
    center = Vector3Add(center, Vector3Scale(up, std::round(alongUp / texel) * texel - alongUp));
    shadowCenter_ = center;
    shadowRadius_ = radius;

    // Свет — далеко за квадратом: тень отбрасывают и холмы, и крыши.
    const float distance = radius + 1500.0f;
    shadowRange_ = 2.0f * distance;
    const Vector3 eye = Vector3Add(center, Vector3Scale(toLight, distance));

    BeginTextureMode(shadowTarget_);
    rlClearScreenBuffers();
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlOrtho(-radius, radius, -radius, radius, 0.0, shadowRange_);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(MatrixLookAt(eye, center, up)));
    rlEnableDepthTest();
    // Задние грани в карту теней: освещённые передние не затеняют сами себя.
    rlSetCullFace(RL_CULL_FACE_FRONT);
    lightViewProj_ = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    return true;
}

void SceneShading::endShadowPass() {
    rlDrawRenderBatchActive();
    rlSetCullFace(RL_CULL_FACE_BACK);
    rlDisableDepthTest();
    EndTextureMode();
}

bool SceneShading::inShadowArea(Vector3 point, float size) const {
    return shadowsOn_ && std::abs(point.x - shadowCenter_.x) < shadowRadius_ + size &&
           std::abs(point.z - shadowCenter_.z) < shadowRadius_ + size;
}

void SceneShading::drawSky(const Camera3D& camera, float time) {
    const float width = static_cast<float>(GetRenderWidth());
    const float height = static_cast<float>(GetRenderHeight());
    const Matrix view = MatrixLookAt(camera.position, camera.target, camera.up);
    const Matrix projection =
        MatrixPerspective(camera.fovy * DEG2RAD, width / std::max(height, 1.0f), 1.0, 10000.0);
    const Matrix inverse = MatrixInvert(MatrixMultiply(view, projection));
    const Vector2 screen = {width, height};
    // Облака сносит ветром; шаг — в метрах на высоте слоя, ускорен для глаза.
    const Vector2 shift = {light_.windX * time * 6.0f, light_.windZ * time * 6.0f};

    apply(skyShader_);
    SetShaderValueMatrix(skyShader_, GetShaderLocation(skyShader_, "invViewProj"), inverse);
    SetShaderValue(skyShader_, GetShaderLocation(skyShader_, "screenSize"), &screen, SHADER_UNIFORM_VEC2);
    SetShaderValue(skyShader_, GetShaderLocation(skyShader_, "cloudShift"), &shift, SHADER_UNIFORM_VEC2);
    setVec3(skyShader_, "zenith", light_.zenith);
    setVec3(skyShader_, "horizon", light_.horizon);
    setVec3(skyShader_, "sunDir", light_.sunDir);
    setVec3(skyShader_, "sunColor", light_.sunColor);
    setVec3(skyShader_, "moonDir", light_.moonDir);
    setFloat(skyShader_, "moonFullness", light_.moonFullness);
    setFloat(skyShader_, "night", light_.night);
    setFloat(skyShader_, "cloudCover", light_.cloudCover);
    BeginShaderMode(skyShader_);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
    EndShaderMode();
}

void SceneShading::setLamps(const std::vector<Lamp>& lamps, float indoorAmbient) {
    lamps_.assign(lamps.begin(), lamps.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(lamps.size(), kMaxLamps)));
    indoorAmbient_ = indoorAmbient;
}

void SceneShading::beginModels() const {
    apply(modelShader_);
    const int count = static_cast<int>(lamps_.size());
    float positions[kMaxLamps * 4] = {};
    float colors[kMaxLamps * 3] = {};
    for (int i = 0; i < count; ++i) {
        const Lamp& lamp = lamps_[static_cast<std::size_t>(i)];
        positions[i * 4 + 0] = lamp.position.x;
        positions[i * 4 + 1] = lamp.position.y;
        positions[i * 4 + 2] = lamp.position.z;
        positions[i * 4 + 3] = lamp.radius;
        colors[i * 3 + 0] = lamp.color.x;
        colors[i * 3 + 1] = lamp.color.y;
        colors[i * 3 + 2] = lamp.color.z;
    }
    SetShaderValue(modelShader_, lampCountLoc_, &count, SHADER_UNIFORM_INT);
    SetShaderValueV(modelShader_, lampPosLoc_, positions, SHADER_UNIFORM_VEC4, kMaxLamps);
    SetShaderValueV(modelShader_, lampColorLoc_, colors, SHADER_UNIFORM_VEC3, kMaxLamps);
    SetShaderValue(modelShader_, indoorAmbientLoc_, &indoorAmbient_, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(modelShader_);
}

void SceneShading::endModels() const {
    EndShaderMode();
}

void SceneShading::bindTextures() const {
    rlActiveTextureSlot(kShadowUnit);
    rlEnableTexture(shadowDepth_);
    rlActiveTextureSlot(kDetailUnit);
    rlEnableTexture(detail_.id);
    rlActiveTextureSlot(0);
}

void SceneShading::unbindTextures() const {
    for (const int unit : {kShadowUnit, kDetailUnit}) {
        rlActiveTextureSlot(unit);
        rlDisableTexture();
    }
    rlActiveTextureSlot(0);
}

} // namespace lw
