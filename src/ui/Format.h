#pragma once

#include "sim/Components.h"
#include "sim/Id.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace lw {
class World;
}

namespace lw::ui {

// Подписи и мелкие виджеты, общие для панелей.

const char* buildingName(BuildingKind kind);
// Профессия: «продавец», «без работы».
const char* professionName(Profession profession);
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
// Ивент: «падение припасов».
const char* eventKindName(EventKind kind);
// Сценарий и фаза мира (этап 7.4): «зомби-вирус», «паника».
const char* scenarioKindName(ScenarioKind kind);
const char* phaseName(WorldPhase phase);
// Предмет: «бинт», «патроны к винтовке».
const char* itemName(ItemKind kind);
// Строка «еда 3, вода 2, бинт 1» — непустые позиции инвентаря; «пусто».
void formatInventory(char* buffer, std::size_t size, const Inventory& inventory);

// Полоска-индикатор 0..1 с подписью.
void meter(const char* label, float value);

// Ссылка на объект: кнопка «#id», у людей — «John Miller #id»; true — по ней
// щёлкнули. Имена берутся из мира, заданного setNameSource (раз за кадр).
bool idLink(EntityId id);
void setNameSource(const World* world);
// Подпись объекта: «John Miller #1745» или «#1745».
std::string idLabel(EntityId id);

// Длительность в тиках — как игровое время: «3 сут 4 ч», «2 ч 05 мин», «35 мин».
void formatGameDuration(char* buffer, std::size_t size, std::uint64_t ticks);

// Когда наступит следующая фаза мира (этап 7.4): «паника — при 3 пострадавших
// (сейчас 1), иначе через 2 сут 4 ч». Пусто — фаза последняя.
void formatNextPhase(char* buffer, std::size_t size, const World& world, const Scenario& scenario);

} // namespace lw::ui
