#include "sim/Calendar.h"

namespace lw {

CalendarTime calendarAt(std::uint64_t worldTick) {
    CalendarTime time;
    time.gameTick = worldTick + kStartGameTick;

    const std::uint64_t dayIndex = time.gameTick / kTicksPerGameDay;
    const std::uint64_t tickOfDay = time.gameTick % kTicksPerGameDay;
    const std::uint64_t tickOfYear = time.gameTick % kTicksPerGameYear;

    time.year = static_cast<int>(dayIndex / kGameDaysPerYear) + 1;
    time.dayOfYear = static_cast<int>(dayIndex % kGameDaysPerYear);
    time.season = static_cast<Season>(time.dayOfYear / kGameDaysPerSeason);
    time.dayOfSeason = time.dayOfYear % kGameDaysPerSeason;
    time.day = static_cast<int>(dayIndex) + 1;
    time.minuteOfDay = static_cast<int>(tickOfDay / kTicksPerGameMinute);
    time.hour = time.minuteOfDay / 60;
    time.minute = time.minuteOfDay % 60;
    time.yearFraction = static_cast<double>(tickOfYear) / static_cast<double>(kTicksPerGameYear);
    return time;
}

std::uint64_t nextTimeOfDay(std::uint64_t worldTick, int hour, int minute) {
    const std::uint64_t gameTick = worldTick + kStartGameTick;
    const std::uint64_t target = static_cast<std::uint64_t>(hour * 60 + minute) * kTicksPerGameMinute;
    const std::uint64_t dayStart = gameTick / kTicksPerGameDay * kTicksPerGameDay;
    std::uint64_t next = dayStart + target;
    if (next <= gameTick) {
        next += kTicksPerGameDay;
    }
    return next - kStartGameTick;
}

} // namespace lw
