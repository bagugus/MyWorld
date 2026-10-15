#pragma once

#include "sim/Components.h"

#include <entt/entt.hpp>

#include <array>
#include <cstddef>

namespace lw {

class World;

// Utility AI людей: каждое действие получает оценку от нужд, черт характера,
// времени суток и расстояния до места, человек выбирает лучшее.
//
// Действие — две фазы: дойти до места (Travel) и выполнить (Perform). Раз в
// 10 игровых минут (во сне — в 30) решение пересматривается: текущее действие
// получает надбавку за начатое дело, поэтому человек не мечется, но бросает
// его, если что-то стало заметно важнее (жажда посреди прогулки). Выбор между
// почти равными действиями решает небольшой случайный разброс оценок (поток
// RngStream::Behavior). Пути прокладываются только для выбранного действия;
// оценки считают расстояние по прямой.
//
// Ночь у каждого своя — по хронотипу (Traits::chronotype). Подробности и все
// формулы оценок — в docs/architecture.md, раздел «Utility AI».

using ActionScores = std::array<float, static_cast<std::size_t>(Action::Count)>;

// Оценки всех действий для человека сейчас, без случайного разброса и без
// надбавки текущему действию. Ноль — действие невозможно. Для отладки и тестов.
ActionScores evaluateActions(const World& world, entt::entity person);

// Ночь человека с таким хронотипом в эту минуту суток: жаворонок спит с 21:00
// до 5:30, сова — на 2,5 ч позже.
bool isPersonalNight(float chronotype, int minuteOfDay);

void updateUtilityAi(World& world);

} // namespace lw
