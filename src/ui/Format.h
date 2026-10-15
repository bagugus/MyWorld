#pragma once

#include "sim/Components.h"
#include "sim/Id.h"

#include <cstddef>
#include <cstdint>

namespace lw::ui {

// Подписи и мелкие виджеты, общие для панелей.

const char* buildingName(BuildingKind kind);
const char* settlementKindName(SettlementKind kind);
// Занятие человека: «спит», «ищет припасы».
const char* actionName(Action action);
// Что слышно: «шаги», «обыск».
const char* soundName(SoundKind kind);
// Что делает зомби: «бредёт», «преследует».
const char* zombieStateName(ZombieState state);

// Полоска-индикатор 0..1 с подписью.
void meter(const char* label, float value);

// Ссылка на объект: кнопка «#id»; true — по ней щёлкнули.
bool idLink(EntityId id);

// Длительность в тиках — как игровое время: «2 ч 05 мин», «35 мин».
void formatGameDuration(char* buffer, std::size_t size, std::uint64_t ticks);

} // namespace lw::ui
