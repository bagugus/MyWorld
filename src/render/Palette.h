#pragma once

#include "sim/Components.h"

#include <entt/entity/registry.hpp>
#include <raylib.h>

namespace lw {

// Цвета мира и мелкие детали облика, общие для вида сверху и 3D: объект
// узнаётся в обоих видах.

// Зомби: спокойные — болотные, идущие на шум — жёлто-зелёные, в погоне — красные.
inline constexpr Color kZombieColor = {92, 128, 64, 255};
inline constexpr Color kZombieAlertColor = {170, 190, 60, 255};
inline constexpr Color kZombieChaseColor = {214, 58, 46, 255};
// Птицы — чёрные точки.
inline constexpr Color kBirdColor = {24, 24, 28, 255};
// Ивенты (этап 7.1): груз — ящик защитного цвета (пустой — тёмный), знак —
// красный дым, сносимый ветром.
inline constexpr Color kCacheColor = {176, 160, 92, 255};
inline constexpr Color kEmptyCacheColor = {88, 82, 60, 255};
inline constexpr Color kCacheOutline = {40, 38, 24, 255};
inline constexpr Color kSmokeColor = {222, 92, 80, 255};
inline constexpr Color kBuildingOutline = {40, 34, 30, 255};
// Небо в 3D: фон и то, что отражает вода. Ночью его затемняет погода.
inline constexpr Color kSkyColor = {132, 166, 200, 255};

Color buildingColor(BuildingKind kind);
// Тела — тёмные: человек — бурый, зомби — тёмно-болотный, туша — тёмно-коричневая.
Color corpseColor(CorpseKind kind);
// Люди в пути — жёлтые (бегущие — белые), на месте — по занятию; зомби — по
// состоянию; животные — по виду, бегущие светлее.
Color creatureColor(const entt::registry& registry, entt::entity entity);

// Место птицы bird в стае flock: смещение от центра стаи, каждая ось −1..1.
// Из хеша id — у каждой стаи своя россыпь, одинаковая от кадра к кадру.
Vector2 birdOffset(EntityId flock, int bird);

} // namespace lw
