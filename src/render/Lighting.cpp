#include "render/Lighting.h"

#include "sim/Climate.h"
#include "sim/Weather.h"

#include <raymath.h>

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Солнце днём и у горизонта; луна — холодная и слабее солнца в десятки раз,
// но ночью глаз привыкает: ночь остаётся читаемой, как на виде сверху.
constexpr Vector3 kSunNoon = {1.0f, 0.96f, 0.88f};
constexpr Vector3 kSunLow = {1.0f, 0.52f, 0.26f};
constexpr float kSunIntensity = 0.9f;
constexpr Vector3 kMoonColor = {0.55f, 0.66f, 0.95f};
constexpr float kMoonIntensity = 0.22f;

// Рассеянный свет неба: ясный день, пасмурный день, сумерки, ночь.
constexpr Vector3 kAmbientClear = {0.34f, 0.39f, 0.48f};
constexpr Vector3 kAmbientOvercast = {0.62f, 0.64f, 0.67f};
constexpr Vector3 kAmbientDusk = {0.34f, 0.28f, 0.36f};
constexpr Vector3 kAmbientNight = {0.11f, 0.14f, 0.23f};
// Отражённый от земли свет — доля неба, теплее.
constexpr Vector3 kGroundBounce = {0.55f, 0.52f, 0.44f};

// Небо: зенит и горизонт.
constexpr Vector3 kZenithClear = {0.27f, 0.47f, 0.78f};
constexpr Vector3 kHorizonClear = {0.68f, 0.79f, 0.90f};
constexpr Vector3 kZenithOvercast = {0.50f, 0.53f, 0.57f};
constexpr Vector3 kHorizonOvercast = {0.66f, 0.68f, 0.70f};
constexpr Vector3 kZenithDusk = {0.22f, 0.27f, 0.48f};
constexpr Vector3 kHorizonDusk = {0.96f, 0.56f, 0.32f};
constexpr Vector3 kZenithNight = {0.012f, 0.018f, 0.045f};
constexpr Vector3 kHorizonNight = {0.045f, 0.06f, 0.11f};
constexpr Vector3 kFogDay = {0.74f, 0.76f, 0.78f};

// Дымка ясного дня: на 7 км скрыта примерно треть; туман и осадки — гуще.
constexpr float kHaze = 1.0f / 17000.0f;
constexpr float kFogDensity = 0.035f;   // при fog = 1 за 50 м скрыто больше 80 %
constexpr float kRainDensity = 0.0012f; // при сильном дожде за 1 км скрыто больше 70 %

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Vector3 mix(Vector3 a, Vector3 b, float t) {
    return Vector3Lerp(a, b, std::clamp(t, 0.0f, 1.0f));
}

// Азимут (градусы от севера по часовой) по времени: полдень — юг, ход по дуге
// тем шире, чем длиннее день: в равноденствие восход на востоке, летом — на
// северо-востоке, зимой — на юго-востоке. После заката — дальше по кругу.
float azimuthAt(int minuteOfDay, int sunrise, int sunset) {
    const float noon = 0.5f * static_cast<float>(sunrise + sunset);
    const float half = std::max(0.5f * static_cast<float>(sunset - sunrise), 1.0f);
    const float spread = 90.0f + (2.0f * half / 60.0f - 12.0f) * 9.5f;
    float fromNoon = static_cast<float>(minuteOfDay) - noon;
    if (fromNoon > 720.0f) {
        fromNoon -= 1440.0f;
    } else if (fromNoon < -720.0f) {
        fromNoon += 1440.0f;
    }
    return 180.0f + fromNoon / half * spread;
}

} // namespace

Vector3 sunDirection(float azimuthDegrees, float elevationDegrees) {
    const float azimuth = azimuthDegrees * DEG2RAD;
    const float elevation = elevationDegrees * DEG2RAD;
    // Азимут 0 — север (−Z), 90 — восток (+X).
    return {std::sin(azimuth) * std::cos(elevation), std::sin(elevation), -std::cos(azimuth) * std::cos(elevation)};
}

SceneLight sceneLightAt(const Environment& environment, const Weather& weather) {
    const SunState& sun = environment.sun;
    const float elevation = sun.elevation;
    const float cloud = std::clamp(weather.cloudCover, 0.0f, 1.0f);
    const float fog = std::clamp(weather.fog, 0.0f, 1.0f);
    const float rain = std::clamp(weather.precipitation, 0.0f, 1.0f);

    SceneLight light;
    light.cloudCover = cloud;
    light.moonFullness = sun.moonFullness;
    light.sunDir = sunDirection(azimuthAt(environment.time.minuteOfDay, sun.sunriseMinute, sun.sunsetMinute),
                                elevation);
    // Луна идёт по пути солнца с отставанием на полсуток — как полная луна;
    // фаза меняет только её яркость.
    CalendarTime moonTime = environment.time;
    moonTime.minuteOfDay = (moonTime.minuteOfDay + 720) % 1440;
    const SunState moon = sunAt(moonTime);
    light.moonDir = sunDirection(azimuthAt(moonTime.minuteOfDay, moon.sunriseMinute, moon.sunsetMinute),
                                 moon.elevation);

    // Доля дня: от гражданских сумерек до солнца на 10°; тёплый оттенок — пока солнце низко.
    const float day = smoothstep(-6.0f, 10.0f, elevation);
    const float low = 1.0f - smoothstep(2.0f, 28.0f, elevation);
    const float dusk = std::max(0.0f, 1.0f - std::abs(elevation - 1.0f) / 9.0f) * (1.0f - 0.7f * cloud);
    light.night = 1.0f - day;

    // Облака, туман и дождь закрывают прямой свет. Переменная облачность —
    // солнце то в просвете, то за облаком: гаснет оно только к сплошной.
    const float veil = (1.0f - 0.92f * smoothstep(0.3f, 1.0f, cloud)) * (1.0f - 0.7f * fog) * (1.0f - 0.4f * rain);
    light.sunColor = mix(kSunNoon, kSunLow, low);
    const float sunPower = kSunIntensity * smoothstep(-1.0f, 6.0f, elevation) * veil;
    const float moonPower = kMoonIntensity * sun.moonFullness * smoothstep(0.0f, 12.0f, moon.elevation) *
                            (1.0f - 0.9f * cloud) * (1.0f - 0.7f * fog) * light.night;
    if (sunPower >= moonPower) {
        light.lightDir = light.sunDir;
        light.lightColor = Vector3Scale(light.sunColor, sunPower);
    } else {
        light.lightDir = light.moonDir;
        light.moonlit = true;
        light.lightColor = Vector3Scale(kMoonColor, moonPower);
    }
    // Тень заметна, когда прямой свет сильнее рассеянного.
    light.shadowStrength = 0.85f * smoothstep(0.03f, 0.3f, std::max(sunPower, moonPower * 2.5f));

    // Рассеянный: днём — от неба (в пасмурный день светлее и серее), в сумерки —
    // сиреневый, ночью — синий, луна добавляет.
    Vector3 ambient = mix(kAmbientClear, kAmbientOvercast, smoothstep(0.2f, 1.0f, cloud));
    ambient = mix(ambient, kAmbientDusk, dusk * 0.6f);
    const Vector3 night = Vector3Scale(kAmbientNight, 1.0f + 0.6f * sun.moonFullness * (1.0f - cloud));
    ambient = mix(night, ambient, day);
    ambient = Vector3Scale(ambient, 1.0f - 0.25f * rain);
    light.ambientSky = ambient;
    light.ambientGround = Vector3Multiply(ambient, kGroundBounce);

    // Небо: ясное, пасмурное, закатное, ночное.
    Vector3 zenith = mix(kZenithClear, kZenithOvercast, cloud);
    Vector3 horizon = mix(kHorizonClear, kHorizonOvercast, cloud);
    zenith = mix(zenith, kZenithDusk, dusk);
    horizon = mix(horizon, kHorizonDusk, dusk);
    light.zenith = mix(kZenithNight, zenith, day);
    light.horizon = mix(kHorizonNight, horizon, day);

    // Туман светлый днём и тёмный ночью; дымка — цвета горизонта.
    const Vector3 fogTint = Vector3Scale(kFogDay, 0.06f + 0.94f * day);
    light.fogColor = mix(light.horizon, fogTint, std::max(fog, rain * 0.6f));
    light.fogDensity = kHaze * (1.0f + 2.0f * cloud) + kFogDensity * fog * fog + kRainDensity * rain;

    // Облака плывут туда, куда дует ветер (windFrom — откуда; ось Z — на юг).
    const float from = weather.windFrom * DEG2RAD;
    light.windX = -std::sin(from) * weather.windSpeed;
    light.windZ = std::cos(from) * weather.windSpeed;
    return light;
}

} // namespace lw
