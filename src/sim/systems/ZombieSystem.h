#pragma once

#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace lw {

class Rng;
class World;

// Зомби и орды (этап 5.2). Подробности и числа — в docs/architecture.md,
// раздел «Зомби».
//
// Зомби решает раз в kZombieThinkTicks — в тот же тик, когда обновилось его
// восприятие, — по тому, что заметил (Awareness):
// - видит человека — преследует и созывает стоном других (разовый звук мира);
// - слышит человека или стон другого зомби, помнит, где потерял добычу, —
//   идёт туда и стоит там какое-то время;
// - иначе одиночка бродит по родным местам, а зомби из орды идёт за вожаком.
// Догнавший держится рядом и бьёт (Combat::target, удары — CombatSystem).
//
// Орда — сущность Horde: вожак ведёт её от поселения к поселению, остальные
// держатся вокруг него. Раз в kHordeUpdateTicks орды подбирают одиночек,
// сливаются друг с другом, а одиночки, сбежавшиеся на шум, сбиваются в новую.

// Решение — раз в секунду симуляции, вразнобой по id, в тик обновления восприятия.
inline constexpr std::uint64_t kZombieThinkTicks = 20;
// Орды обновляются раз в игровую минуту.
inline constexpr std::uint64_t kHordeUpdateTicks = kTicksPerGameMinute;

// Скорости зомби, м/с. Погоня — личная скорость Walker::speed (1,3–1,7 м/с):
// идущего человека догоняет, бегущего — нет.
inline constexpr float kZombieRoamSpeed = 0.4f;
inline constexpr float kHordeSpeed = 0.45f;
inline constexpr float kInvestigateSpeed = 0.9f;

// Угроза для человека — из того, что он сейчас замечает (Awareness).
struct Threat {
    float level = 0.0f;           // 0..1
    float awayX = 0.0f;           // куда бежать: единичный вектор от угроз
    float awayY = 0.0f;
    EntityId nearest = kInvalidId; // ближайший замеченный зомби
    float nearestX = 0.0f;         // где он
    float nearestY = 0.0f;
    int count = 0;                 // сколько зомби замечает
};
Threat threatOf(const World& world, entt::entity person);

// Создать зомби в клетке: так их селит мир и ставят тесты.
entt::entity createZombie(World& world, Cell cell, Rng& rng);
// Сделать зомби из существующей сущности (тело укушенного встаёт, этап 5.5):
// поведение зомби, родные места — anchor. Позиция у сущности уже есть.
void raiseZombie(World& world, entt::entity entity, Cell anchor, Rng& rng);
// Собрать орду из зомби (любой порядок; вожак — с меньшим id). gathered —
// сбежались на шум, иначе орда с начала мира. Возвращает id орды.
EntityId formHorde(World& world, std::vector<EntityId> members, bool gathered);

void updateZombies(World& world);

} // namespace lw
