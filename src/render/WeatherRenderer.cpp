#include "render/WeatherRenderer.h"

#include "sim/Climate.h"
#include "sim/Weather.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lw {

namespace {

constexpr int kMaxRainDrops = 900;
constexpr int kMaxSnowFlakes = 700;
constexpr float kRainSpeed = 1100.0f; // пикселей в секунду
constexpr float kRainLength = 16.0f;
constexpr float kSnowSpeed = 70.0f;
constexpr float kMaxDarkness = 0.86f; // доля затемнения в безлунную ночь
// Затемнение растёт нелинейно: пасмурный день (свет около 0,4) не должен выглядеть сумерками.
constexpr float kDarknessCurve = 2.5f;
constexpr double kLightningSlot = 0.125; // секунд: шаг, на котором решается, будет ли вспышка

constexpr Color kNightColor = {6, 10, 32, 255};
constexpr Color kDuskColor = {255, 120, 50, 255};
constexpr Color kFogColor = {196, 200, 206, 255};

// Детерминированное псевдослучайное число 0..1 по номеру частицы (только для картинки).
float hash01(std::uint32_t n) {
    n ^= n >> 16;
    n *= 0x7FEB352Du;
    n ^= n >> 15;
    n *= 0x846CA68Bu;
    n ^= n >> 16;
    return static_cast<float>(n & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

float wrap(float value, float size) {
    const float result = std::fmod(value, size);
    return result < 0.0f ? result + size : result;
}

Color withAlpha(Color color, float alpha) {
    color.a = static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return color;
}

void fillScreen(Color color) {
    if (color.a > 0) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), color);
    }
}

} // namespace

void WeatherRenderer::update(float realDt, bool running) {
    if (running) {
        time_ += realDt;
    }
}

void WeatherRenderer::draw(const Environment& environment, const Weather& weather) const {
    const float light = environment.light;

    if (weather.precipitation > 0.02f) {
        drawPrecipitation(weather, isSnowing(environment.temperature));
    }

    // Туман светлый днём и почти не виден в темноте.
    if (weather.fog > 0.01f) {
        const float brightness = std::min(1.0f, 0.1f + 1.6f * light);
        const Color fog = {static_cast<unsigned char>(kFogColor.r * brightness),
                           static_cast<unsigned char>(kFogColor.g * brightness),
                           static_cast<unsigned char>(kFogColor.b * brightness), 255};
        fillScreen(withAlpha(fog, weather.fog * 0.6f));
    }

    fillScreen(withAlpha(kNightColor, std::pow(1.0f - light, kDarknessCurve) * kMaxDarkness));

    // Тёплый оттенок, пока солнце низко: от −6° до +8°, сильнее всего у горизонта.
    const float elevation = environment.sun.elevation;
    const float dusk = std::max(0.0f, 1.0f - std::abs(elevation - 1.0f) / 7.0f);
    fillScreen(withAlpha(kDuskColor, dusk * 0.16f * (1.0f - 0.8f * weather.cloudCover)));

    // Молнии: в сильную грозу иногда вспышка, гаснущая за долю секунды.
    if (weather.kind == WeatherKind::Storm && weather.precipitation > 0.6f) {
        const auto slot = static_cast<std::uint32_t>(time_ / kLightningSlot);
        if (hash01(slot * 2654435761u + 17u) < 0.012f) {
            const auto progress = static_cast<float>(std::fmod(time_, kLightningSlot) / kLightningSlot);
            fillScreen(withAlpha(WHITE, 0.55f * (1.0f - progress)));
        }
    }
}

void WeatherRenderer::drawPrecipitation(const Weather& weather, bool snow) const {
    const auto width = static_cast<float>(GetScreenWidth());
    const auto height = static_cast<float>(GetScreenHeight());
    const auto t = static_cast<float>(time_);

    // Ветер сносит осадки туда, куда дует: от windFrom. На экране север — вверх.
    constexpr float kDegToRad = 3.14159265f / 180.0f;
    const float blowX = -std::sin(weather.windFrom * kDegToRad);
    const float blowY = std::cos(weather.windFrom * kDegToRad);
    const float drift = weather.windSpeed * 12.0f; // пикселей в секунду на м/с

    const int maxCount = snow ? kMaxSnowFlakes : kMaxRainDrops;
    const int count = static_cast<int>(static_cast<float>(maxCount) * std::min(weather.precipitation, 1.0f));
    const float fallSpeed = snow ? kSnowSpeed : kRainSpeed;
    // Вид сверху: «падение» — это движение вниз по экрану плюс снос ветром.
    const float vx = blowX * drift;
    const float vy = fallSpeed + blowY * drift;
    const float speed = std::sqrt(vx * vx + vy * vy);
    const float dirX = vx / speed;
    const float dirY = vy / speed;

    for (int i = 0; i < count; ++i) {
        const auto n = static_cast<std::uint32_t>(i);
        const float speedFactor = 0.7f + 0.6f * hash01(n * 3u + 2u);
        float x = hash01(n * 3u) * width + vx * speedFactor * t;
        float y = hash01(n * 3u + 1u) * height + vy * speedFactor * t;
        if (snow) {
            x += 6.0f * std::sin(t * 1.7f + static_cast<float>(i));
        }
        x = wrap(x, width + 40.0f) - 20.0f;
        y = wrap(y, height + 40.0f) - 20.0f;

        if (snow) {
            DrawCircleV({x, y}, 1.2f + 1.3f * hash01(n * 7u + 5u), {240, 244, 250, 200});
        } else {
            DrawLineV({x, y}, {x - dirX * kRainLength, y - dirY * kRainLength}, {180, 198, 230, 170});
        }
    }
}

} // namespace lw
