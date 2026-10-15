#pragma once

#include <cstdint>

// Масштаб времени. В мире два времени:
// - время симуляции — секунды, в которых считается физика: скорости (м/с), бой, звук.
//   Тик — kTickDt секунд симуляции.
// - игровое время — часы и календарь: сутки, сезоны, нужды, сон, свет, погода.
//   Идёт в kGameTimeScale раз быстрее времени симуляции.
// Человек проходит 1,4 м/с, то есть около 0,4 км за игровой час: через карту — сутки пути,
// до соседней деревни — несколько часов. Так карта в 8 км ощущается большой.
// Всё игровое время считается в целых тиках, без float: календарь точен и детерминирован.
namespace lw {

inline constexpr int kTicksPerSecond = 20;
inline constexpr float kTickDt = 1.0f / static_cast<float>(kTicksPerSecond);

// Игровых секунд в секунде симуляции.
inline constexpr int kGameTimeScale = 12;

inline constexpr std::uint64_t kTicksPerGameMinute = 60 * kTicksPerSecond / kGameTimeScale;
inline constexpr std::uint64_t kTicksPerGameHour = 60 * kTicksPerGameMinute;
inline constexpr std::uint64_t kTicksPerGameDay = 24 * kTicksPerGameHour;

// Год — четыре сезона по 15 суток: весна, лето, осень, зима.
inline constexpr int kGameDaysPerSeason = 15;
inline constexpr int kSeasonsPerYear = 4;
inline constexpr int kGameDaysPerYear = kGameDaysPerSeason * kSeasonsPerYear;
inline constexpr std::uint64_t kTicksPerGameYear = kGameDaysPerYear * kTicksPerGameDay;

// Игровая минута должна быть целым числом тиков.
static_assert(60 * kTicksPerSecond % kGameTimeScale == 0);
static_assert(kTicksPerGameMinute == 100);
static_assert(kTicksPerGameDay == 144'000);       // 2 ч симуляции
static_assert(kTicksPerGameYear == 8'640'000);    // 120 ч симуляции

} // namespace lw
