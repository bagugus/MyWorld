#pragma once

#include "sim/Calendar.h"
#include "sim/Weather.h"

namespace lw {

class World;

// Климат: солнце, луна, освещённость и температура. Чистые функции от календаря и погоды.
// Модель — широта около 51° (как Чернарусь): летом день 16,5 ч, зимой 7,5 ч,
// летом днём около +20 °C, зимой около −6 °C.
// Без sin/cos: периодические кривые — полиномиальное приближение (smoothCycle).

// Приближение cos(2π·t): период 1, максимум 1 при t = 0, минимум −1 при t = 0,5.
// Отклонение от косинуса — не больше 0,03.
float smoothCycle(double t);

struct SunState {
    int sunriseMinute = 0; // минуты от полуночи
    int sunsetMinute = 0;
    float elevation = 0.0f; // градусы над горизонтом, ночью отрицательная
    float moonFullness = 0.0f; // 0 — новолуние, 1 — полнолуние
};

SunState sunAt(const CalendarTime& time);

// Освещённость 0..1: 1 — ясный летний полдень, около 0,02 — безлунная ночь.
// Учитывает облака, туман и осадки.
float lightLevel(const SunState& sun, const Weather& weather);

// Температура воздуха на уровне моря, °C.
float seaLevelTemperature(const CalendarTime& time, const Weather& weather);

// Температура на высоте: −6,5 °C на километр.
float temperatureAtElevation(float seaLevel, float elevationMeters);

// Осадки при такой температуре выпадают снегом.
bool isSnowing(float temperature);

// Всё, что нужно знать о текущей обстановке, одним вызовом.
struct Environment {
    CalendarTime time;
    SunState sun;
    float light = 0.0f;
    float temperature = 0.0f; // на уровне моря
};

Environment environmentAt(const World& world);

} // namespace lw
