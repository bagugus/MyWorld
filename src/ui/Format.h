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
// Животные: «олень», «пасётся».
const char* animalKindName(AnimalKind kind);
const char* animalStateName(AnimalState state);
// Оружие, причина смерти, чьё тело: «топор», «истёк кровью», «зомби».
const char* weaponName(WeaponKind kind);
const char* deathCauseName(DeathCause cause);
const char* corpseKindName(CorpseKind kind);
// Воспоминание о ком-то: «спас», «бросил в беде», «погиб».
const char* memoryKindName(MemoryKind kind);
// Предмет: «бинт», «патроны к винтовке».
const char* itemName(ItemKind kind);
// Строка «еда 3, вода 2, бинт 1» — непустые позиции инвентаря; «пусто».
void formatInventory(char* buffer, std::size_t size, const Inventory& inventory);

// Полоска-индикатор 0..1 с подписью.
void meter(const char* label, float value);

// Ссылка на объект: кнопка «#id»; true — по ней щёлкнули.
bool idLink(EntityId id);

// Длительность в тиках — как игровое время: «2 ч 05 мин», «35 мин».
void formatGameDuration(char* buffer, std::size_t size, std::uint64_t ticks);

} // namespace lw::ui
