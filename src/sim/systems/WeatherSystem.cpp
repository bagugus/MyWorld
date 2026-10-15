#include "sim/systems/WeatherSystem.h"

#include "sim/Calendar.h"
#include "sim/Climate.h"
#include "sim/World.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lw {

namespace {

constexpr int kKinds = static_cast<int>(WeatherKind::Count);

// К чему тянется погода каждого типа.
struct KindProfile {
    float cloudCover;
    float precipitation;
    float fog;
    float wind;     // м/с, среднее
    int minHours;   // сколько длится
    int maxHours;
};

constexpr std::array<KindProfile, kKinds> kProfiles = {{
    {0.1f, 0.0f, 0.0f, 3.0f, 6, 30},  // Clear
    {0.5f, 0.0f, 0.0f, 4.0f, 3, 12},  // Cloudy
    {0.9f, 0.0f, 0.0f, 5.0f, 4, 24},  // Overcast
    {0.7f, 0.0f, 0.9f, 1.0f, 2, 6},   // Fog
    {0.95f, 0.5f, 0.1f, 6.0f, 2, 10}, // Rain
    {1.0f, 1.0f, 0.05f, 12.0f, 1, 3}, // Storm
}};

// Насколько тип вероятен в сезоне (весна, лето, осень, зима).
constexpr float kSeasonWeights[4][kKinds] = {
    // Clear Cloudy Overcast Fog  Rain  Storm
    {3.0f, 3.0f, 2.0f, 1.5f, 2.5f, 0.5f}, // весна
    {5.0f, 3.0f, 1.0f, 0.5f, 1.5f, 1.5f}, // лето
    {2.0f, 3.0f, 3.5f, 2.5f, 3.0f, 0.3f}, // осень
    {2.0f, 2.5f, 4.0f, 1.5f, 3.0f, 0.1f}, // зима
};

// Из какого типа в какой можно перейти: погода меняется через промежуточные
// состояния — из ясного неба сразу не льёт.
constexpr bool kAllowed[kKinds][kKinds] = {
    // to: Clear  Cloudy Overcast Fog   Rain   Storm
    {false, true, false, true, false, false},  // Clear
    {true, false, true, true, true, true},     // Cloudy
    {false, true, false, true, true, false},   // Overcast
    {true, true, true, false, false, false},   // Fog
    {false, true, true, false, false, true},   // Rain
    {false, true, false, false, true, false},  // Storm
};

// Скорость приближения к целевым значениям: доля разницы за игровую минуту.
// Смена облачности и осадков занимает около часа.
constexpr float kBlendPerMinute = 1.0f / 45.0f;
constexpr float kWindBlendPerMinute = 1.0f / 30.0f;

// Аномалия температуры: случайное блуждание раз в час с возвратом к нулю,
// разброс около ±2,5 °C.
constexpr float kAnomalyStep = 1.2f;
constexpr float kAnomalyReturn = 0.04f;

// Снег: полный покров за 3 часа сильного снегопада, при +5 °C тает за 8 часов.
constexpr float kSnowPerMinute = 1.0f / 180.0f;
constexpr float kMeltPerDegreeMinute = 0.0004f;
constexpr float kRainMeltPerMinute = 0.002f;
constexpr float kInitialSnow = 0.3f; // начало марта

float approach(float value, float target, float rate) {
    return value + (target - value) * rate;
}

float kindWeight(WeatherKind from, WeatherKind to, Season season, int hour) {
    const int a = static_cast<int>(from);
    const int b = static_cast<int>(to);
    if (!kAllowed[a][b]) {
        return 0.0f;
    }
    float weight = kSeasonWeights[static_cast<int>(season)][b];
    // Туман ложится ночью и утром, днём рассеивается.
    if (to == WeatherKind::Fog) {
        if (hour >= 2 && hour < 9) {
            weight *= 3.0f;
        } else if (hour >= 11 && hour < 19) {
            weight *= 0.3f;
        }
    }
    return weight;
}

WeatherKind pickKind(Rng& rng, WeatherKind from, Season season, int hour) {
    float total = 0.0f;
    std::array<float, kKinds> weights{};
    for (int i = 0; i < kKinds; ++i) {
        weights[static_cast<std::size_t>(i)] = kindWeight(from, static_cast<WeatherKind>(i), season, hour);
        total += weights[static_cast<std::size_t>(i)];
    }
    float roll = rng.unit() * total;
    for (int i = 0; i < kKinds; ++i) {
        roll -= weights[static_cast<std::size_t>(i)];
        if (roll < 0.0f && weights[static_cast<std::size_t>(i)] > 0.0f) {
            return static_cast<WeatherKind>(i);
        }
    }
    // Погрешность округления: последний разрешённый тип.
    for (int i = kKinds - 1; i >= 0; --i) {
        if (weights[static_cast<std::size_t>(i)] > 0.0f) {
            return static_cast<WeatherKind>(i);
        }
    }
    return from;
}

// Новый тип погоды: длительность, ветер. Направление ветра при смене поворачивает.
void startKind(Weather& weather, WeatherKind kind, Rng& rng) {
    const KindProfile& profile = kProfiles[static_cast<std::size_t>(kind)];
    weather.kind = kind;
    weather.minutesLeft = static_cast<std::uint32_t>(rng.rangeInt(profile.minHours * 60, profile.maxHours * 60));
    weather.windTarget = profile.wind * rng.range(0.6f, 1.4f);
    weather.windFrom = std::fmod(weather.windFrom + rng.range(-60.0f, 60.0f) + 360.0f, 360.0f);
}

} // namespace

void initWeather(World& world) {
    Rng& rng = world.rng(RngStream::Weather);
    const CalendarTime time = calendarAt(world.tick());
    Weather& weather = world.weather();

    float total = 0.0f;
    for (const float weight : kSeasonWeights[static_cast<int>(time.season)]) {
        total += weight;
    }
    float roll = rng.unit() * total;
    int kind = 0;
    for (; kind < kKinds - 1; ++kind) {
        roll -= kSeasonWeights[static_cast<int>(time.season)][kind];
        if (roll < 0.0f) {
            break;
        }
    }

    weather.windFrom = rng.range(0.0f, 360.0f);
    startKind(weather, static_cast<WeatherKind>(kind), rng);
    // Погода уже установилась: сразу берём целевые значения.
    const KindProfile& profile = kProfiles[static_cast<std::size_t>(kind)];
    weather.cloudCover = profile.cloudCover;
    weather.precipitation = profile.precipitation;
    weather.fog = profile.fog;
    weather.windSpeed = weather.windTarget;
    weather.anomaly = rng.range(-2.0f, 2.0f);
    weather.snowCover = kInitialSnow;
}

void updateWeather(World& world) {
    const std::uint64_t gameTick = world.tick() + kStartGameTick;
    if (gameTick % kTicksPerGameMinute != 0) {
        return;
    }

    Rng& rng = world.rng(RngStream::Weather);
    const CalendarTime time = calendarAt(world.tick());
    Weather& weather = world.weather();

    if (weather.minutesLeft > 0) {
        --weather.minutesLeft;
    } else {
        startKind(weather, pickKind(rng, weather.kind, time.season, time.hour), rng);
    }

    const KindProfile& profile = kProfiles[static_cast<std::size_t>(weather.kind)];
    weather.cloudCover = approach(weather.cloudCover, profile.cloudCover, kBlendPerMinute);
    // Дождь не начинается, пока не затянуло небо.
    const float precipitationTarget = weather.cloudCover > 0.8f ? profile.precipitation : 0.0f;
    weather.precipitation = approach(weather.precipitation, precipitationTarget, kBlendPerMinute);
    weather.fog = approach(weather.fog, profile.fog, kBlendPerMinute);
    weather.windSpeed = approach(weather.windSpeed, weather.windTarget, kWindBlendPerMinute);

    if (time.minute == 0) {
        weather.anomaly += rng.range(-kAnomalyStep, kAnomalyStep) - kAnomalyReturn * weather.anomaly;
        weather.windFrom = std::fmod(weather.windFrom + rng.range(-10.0f, 10.0f) + 360.0f, 360.0f);
    }

    const float temperature = seaLevelTemperature(time, weather);
    if (weather.precipitation > 0.05f && isSnowing(temperature)) {
        weather.snowCover += weather.precipitation * kSnowPerMinute;
    } else if (temperature > 0.0f) {
        const float rain = weather.precipitation > 0.05f ? weather.precipitation : 0.0f;
        weather.snowCover -= temperature * kMeltPerDegreeMinute + rain * kRainMeltPerMinute;
    }
    weather.snowCover = std::clamp(weather.snowCover, 0.0f, 1.0f);
}

} // namespace lw
