#pragma once

#include "sim/Components.h"

namespace lw {

// Свойства оружия и естественных атак (этап 5.4). Урон — доля здоровья цели
// (у всех существ здоровье 0..1). Время — в секундах симуляции: бой — физика.
// Числа и их смысл — в docs/architecture.md, раздел «Бой».
struct AttackSpec {
    bool ranged = false;
    // Ближний бой — досягаемость; стрельба — дистанция уверенного выстрела
    // (дальше шанс попасть падает до maxRange), метры.
    float reach = 1.4f;
    float maxRange = 1.4f;
    float interval = 1.0f;  // секунд между ударами или выстрелами
    float accuracy = 0.7f;  // шанс попасть в лучших условиях
    float damage = 0.1f;    // средний урон, доля здоровья; разброс ±30 %
    float critChance = 0.0f; // попадание в голову: цель погибает сразу
    float bleeding = 0.0f;  // кровотечение раненого человека, здоровья в игровой час
    float loudness = 0.0f;  // выстрел слышен в стольких метрах; 0 — бесшумно
};

// Как бьёт человек с этим оружием. Ружьё без патронов — дубина.
const AttackSpec& weaponSpec(WeaponKind kind);
const AttackSpec& attackSpec(const Weapon& weapon);
// Огнестрельное ли оружие.
bool isFirearm(WeaponKind kind);

// Естественные атаки: зомби — укус и когти, кабан — клыки.
const AttackSpec& zombieAttack();
const AttackSpec& boarAttack();
// Шанс, что удар зомби — укус (иначе царапина).
inline constexpr float kBiteChance = 0.08f;
inline constexpr float kBiteBleeding = 0.25f; // здоровья в игровой час
// Укус заражает (этап 5.5): столько к заражению; дальше оно растёт само (NeedsSystem).
inline constexpr float kBiteInfection = 0.05f;

// Насколько человек с таким оружием уверен в бою с зомби, 0..1: для оценки
// действия «Драться» в Utility AI.
float weaponPower(const Weapon& weapon);

} // namespace lw
