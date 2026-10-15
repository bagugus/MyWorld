#pragma once

#include "sim/Components.h"

namespace lw {

// Предметы и инвентарь (этап 5.6). Подробности — в docs/architecture.md,
// раздел «Инвентарь и лут».

// Вес одного предмета, кг.
float itemWeight(ItemKind kind);
// Вес всего инвентаря, кг (оружие в руках не считается).
float inventoryWeight(const Inventory& inventory);
// Сколько человек уносит, кг.
inline constexpr float kCarryCapacity = 15.0f;
// Сколько ещё предметов этого вида влезет в инвентарь до capacity кг.
int roomFor(const Inventory& inventory, ItemKind kind, float capacity = kCarryCapacity);

// Оружие как предмет и патроны к нему; ItemKind::Count — нет (кулаки, ближнее
// оружие без патронов).
ItemKind itemOf(WeaponKind weapon);
ItemKind ammoFor(WeaponKind weapon);
// Оружие, которое этот предмет; WeaponKind::Fists — предмет не оружие.
WeaponKind weaponOf(ItemKind item);

// Взять в руки самое сильное оружие из того, что в руках и в инвентаре, с
// учётом патронов (ружьё без патронов слабее топора); прежнее — в инвентарь.
// Патроны к оружию в руках перекладываются из инвентаря в Weapon::ammo.
// true — оружие сменилось.
bool equipBest(Weapon& weapon, Inventory& inventory);

// Съесть порцию из инвентаря: сначала мясо — оно портится. false — нечего.
bool eatFrom(Inventory& inventory);

} // namespace lw
