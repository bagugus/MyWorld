#pragma once

#include <cstdint>

namespace lw {

class World;

// ВРЕМЕННО (этап 4.3): распорядок дня людей без нужд и характера.
// Днём человек отдыхает, потом идёт к случайному зданию своего или соседнего
// поселения; на ночь возвращается домой и спит до утра. Нужен, чтобы проверить
// поиск пути и движение на живых маршрутах. На этапе 4.4 его заменит Utility AI.

enum class Activity : std::uint8_t { Rest, Walk };

struct Routine {
    Activity activity = Activity::Rest;
    // Тик мира, когда кончается отдых.
    std::uint64_t restUntil = 0;
    // Ночь человека: с bedtime до wakeTime (минуты суток). У каждого свои — по
    // черте chronotype, — поэтому люди не прокладывают пути все в один тик.
    std::int32_t bedtime = 22 * 60;
    std::int32_t wakeTime = 7 * 60;
};

// Поселить count человек в жилые дома (дома и многоэтажки), у дверей.
// Если жилья на карте нет — на случайные проходимые клетки, без дома.
void spawnPeople(World& world, int count);
void updateRoutine(World& world);

} // namespace lw
