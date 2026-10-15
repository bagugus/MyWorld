#pragma once

#include <cstdint>

namespace lw {

// Тип погоды — «режим», к которому плавно тянутся облачность, осадки, туман и ветер.
// Осадки при морозе выпадают снегом: отдельного состояния для снега нет.
// Значения попадают в хеш и сохранения: новые добавлять только в конец (перед Count).
enum class WeatherKind : std::uint8_t { Clear, Cloudy, Overcast, Fog, Rain, Storm, Count };

// Погода одна на весь мир (карта 8 км — как один район). Меняет её WeatherSystem
// раз в игровую минуту; всё, кроме kind и minutesLeft, меняется плавно.
struct Weather {
    WeatherKind kind = WeatherKind::Clear;
    std::uint32_t minutesLeft = 0; // игровых минут до смены типа

    float cloudCover = 0.0f;    // 0..1
    float precipitation = 0.0f; // 0..1, интенсивность дождя или снега
    float fog = 0.0f;           // 0..1, 1 — видимость несколько десятков метров
    float windSpeed = 0.0f;     // м/с
    float windTarget = 0.0f;    // м/с, к этому значению тянется windSpeed
    float windFrom = 0.0f;      // градусы, откуда дует: 0 — север, 90 — восток
    float anomaly = 0.0f;       // °C, медленное случайное отклонение от климатической нормы
    float snowCover = 0.0f;     // 0..1, снег на земле
};

} // namespace lw
