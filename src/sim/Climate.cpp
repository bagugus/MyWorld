#include "sim/Climate.h"

#include "sim/World.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Доли года от 1 марта (первого дня весны).
constexpr double kSummerSolstice = 0.302; // 21 июня
constexpr double kWarmestDay = 0.39;      // конец июля

// Солнце.
constexpr float kNoonMinute = 750.0f;          // 12:30 — солнечный полдень
constexpr float kDayLengthMean = 720.0f;       // минут, в равноденствие
constexpr float kDayLengthAmplitude = 270.0f;  // летом 16,5 ч, зимой 7,5 ч
constexpr float kNoonElevationMean = 39.0f;    // градусы: 90 − широта
constexpr float kNoonElevationAmplitude = 23.4f;
constexpr float kTwilightDegreesPerMinute = 6.0f / 45.0f; // гражданские сумерки — 45 мин

// Луна: цикл короче сезона, поэтому фазы с сезонами не совпадают.
constexpr int kMoonCycleDays = 12;

// Освещённость.
constexpr float kNightLight = 0.015f;   // безлунная ночь
constexpr float kMoonLight = 0.1f;      // добавка от полной луны
constexpr float kHorizonLight = 0.25f;  // солнце на горизонте
constexpr float kDarkElevation = -12.0f; // ниже — ночь
constexpr float kBrightElevation = 25.0f; // выше — полный свет

// Температура.
constexpr float kMeanTemperature = 7.0f;
constexpr float kSeasonAmplitude = 13.0f;
constexpr float kDailyAmplitude = 4.5f;
constexpr float kWarmestMinute = 900.0f; // 15:00
constexpr float kLapseRate = 0.0065f;    // °C на метр
constexpr float kSnowTemperature = 0.5f;

float smoothstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

} // namespace

float smoothCycle(double t) {
    const double phase = t - std::floor(t);
    // Расстояние до середины периода: 1 на пиках, 0 во впадине.
    const auto x = static_cast<float>(std::abs(2.0 * phase - 1.0));
    return 2.0f * smoothstep(x) - 1.0f;
}

SunState sunAt(const CalendarTime& time) {
    const float season = smoothCycle(time.yearFraction - kSummerSolstice);
    const float dayLength = kDayLengthMean + kDayLengthAmplitude * season;
    const float noonElevation = kNoonElevationMean + kNoonElevationAmplitude * season;

    SunState sun;
    const float half = dayLength * 0.5f;
    sun.sunriseMinute = static_cast<int>(kNoonMinute - half);
    sun.sunsetMinute = static_cast<int>(kNoonMinute + half);

    // Днём высота — парабола с вершиной в полдень, после заката опускается
    // с постоянной скоростью: так сумерки длятся одинаково весь год.
    const float fromNoon = std::abs(static_cast<float>(time.minuteOfDay) - kNoonMinute);
    if (fromNoon <= half) {
        const float x = fromNoon / half;
        sun.elevation = noonElevation * (1.0f - x * x);
    } else {
        sun.elevation = -(fromNoon - half) * kTwilightDegreesPerMinute;
    }

    const int moonDay = time.day % kMoonCycleDays;
    const double moonPhase = (static_cast<double>(moonDay) + time.minuteOfDay / 1440.0) / kMoonCycleDays;
    sun.moonFullness = 0.5f - 0.5f * smoothCycle(moonPhase);
    return sun;
}

float lightLevel(const SunState& sun, const Weather& weather) {
    float day = 0.0f;
    if (sun.elevation > 0.0f) {
        day = kHorizonLight + (1.0f - kHorizonLight) * smoothstep(sun.elevation / kBrightElevation);
    } else {
        day = kHorizonLight * smoothstep((sun.elevation - kDarkElevation) / -kDarkElevation);
    }
    day *= (1.0f - 0.5f * weather.cloudCover) * (1.0f - 0.3f * weather.fog) *
           (1.0f - 0.15f * weather.precipitation);

    const float moon = kNightLight + kMoonLight * sun.moonFullness * (1.0f - 0.85f * weather.cloudCover);
    return std::max(day, moon);
}

float seaLevelTemperature(const CalendarTime& time, const Weather& weather) {
    const float seasonal =
        kMeanTemperature + kSeasonAmplitude * smoothCycle(time.yearFraction - kWarmestDay);
    // Облака и туман сглаживают суточный ход: днём не так жарко, ночью не так холодно.
    const float dailyAmplitude =
        kDailyAmplitude * (1.0f - 0.6f * weather.cloudCover) * (1.0f - 0.4f * weather.fog);
    const float daily =
        dailyAmplitude * smoothCycle((static_cast<float>(time.minuteOfDay) - kWarmestMinute) / 1440.0);
    return seasonal + daily + weather.anomaly - 2.0f * weather.precipitation;
}

float temperatureAtElevation(float seaLevel, float elevationMeters) {
    return seaLevel - kLapseRate * std::max(0.0f, elevationMeters);
}

bool isSnowing(float temperature) {
    return temperature < kSnowTemperature;
}

Environment environmentAt(const World& world) {
    Environment environment;
    environment.time = calendarAt(world.tick());
    environment.sun = sunAt(environment.time);
    environment.light = lightLevel(environment.sun, world.weather());
    environment.temperature = seaLevelTemperature(environment.time, world.weather());
    return environment;
}

} // namespace lw
