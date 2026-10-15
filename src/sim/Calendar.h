#pragma once

#include "sim/TimeScale.h"

#include <cstdint>

namespace lw {

// Значения попадают в хеш и сохранения: новые добавлять только в конец.
enum class Season : std::uint8_t { Spring, Summer, Autumn, Winter };

// Мир начинается в 08:00 первого дня весны первого года.
// Первый день весны соответствует 1 марта: так привязаны солнце и климат (sim/Climate.h).
inline constexpr std::uint64_t kStartGameTick = 8 * kTicksPerGameHour;

// Игровая дата и время. Всё считается из номера тика целочисленно, поэтому точно.
struct CalendarTime {
    std::uint64_t gameTick = 0; // тиков от 00:00 первого дня первого года
    int year = 1;               // с 1
    Season season = Season::Spring;
    int dayOfSeason = 0;        // 0 .. kGameDaysPerSeason-1
    int dayOfYear = 0;          // 0 .. kGameDaysPerYear-1
    int day = 1;                // номер дня с начала календаря, с 1
    int hour = 0;
    int minute = 0;
    int minuteOfDay = 0;        // 0 .. 1439
    double yearFraction = 0.0;  // доля прошедшего года, [0, 1), с точностью до тика
};

CalendarTime calendarAt(std::uint64_t worldTick);

// Тик мира, на котором наступит ближайшее (строго после worldTick) время hour:minute.
std::uint64_t nextTimeOfDay(std::uint64_t worldTick, int hour, int minute);

} // namespace lw
