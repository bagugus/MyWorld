#pragma once

#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

#include <cstdint>

namespace lw {

class Rng;
class World;

// Животные и птицы (этап 5.3). Подробности и числа — в docs/architecture.md,
// раздел «Животные и птицы».
//
// Животное решает раз в kAnimalThinkTicks — в тот же тик, когда обновилось
// его восприятие, — по тому, что заметило (Awareness):
// - человек или зомби ближе дистанции бегства (у оленя она больше, у зайца
//   меньше) — убегает прочь бегом;
// - иначе пасётся на месте или переходит к соседнему месту у родных мест:
//   олени и кабаны держатся леса, зайцы — полей и лугов.
// Зомби животных не замечают и не трогают. Раненое животное бежит от обидчика,
// даже не видя его, а кабан, которого загнали (ближе 6 м) или ранили, может
// броситься на человека (Charge) и бить клыками (Combat, CombatSystem).
//
// Стая птиц сидит на земле. Кто-то подошёл вплотную или проходит рядом —
// она взлетает с криком (разовый звук мира SoundKind::Birds), который слышно
// далеко: зомби идут на него, как на любой шум. Посидев несколько часов,
// стая перелетает сама, негромко, — к ближайшему телу, если оно есть (этап 5.5).

inline constexpr std::uint64_t kAnimalThinkTicks = 20;

// Свойства вида.
struct AnimalSpec {
    float runSpeed;    // бегом, м/с: Walker::speed
    float walkSpeed;   // пасётся, переходит, м/с
    float sight;       // дальность зрения, м
    float hearing;     // острота слуха
    float fleeSeen;    // увидел человека или зомби ближе этого — бежит, м
    float fleeHeard;   // услышал ближе этого — бежит, м
    float fleeDistance; // убегает на столько, м
    bool forest;       // держится леса (иначе — открытых мест)
};
const AnimalSpec& animalSpec(AnimalKind kind);

// Создать животное в клетке: так их селит мир и ставят тесты.
entt::entity createAnimal(World& world, AnimalKind kind, Cell cell, Rng& rng);
// Создать стаю птиц в клетке.
entt::entity createFlock(World& world, Cell cell, int birds, Rng& rng);

void updateAnimals(World& world);

} // namespace lw
