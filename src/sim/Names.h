#pragma once

#include "sim/Id.h"
#include "sim/Rng.h"

#include <entt/entity/registry.hpp>

#include <string>
#include <string_view>

namespace lw {

// Имя человека (английское). Отдельный компонент, а не поле Person: имя
// остаётся у тела и у вставшего зомби — «узнал в зомби Джона Миллера».
struct Name {
    std::string first;
    std::string last;
};

// Случайные имя и фамилия из списков.
std::string_view randomFirstName(Rng& rng);
// Мужское или женское имя (этап 7.3: пол задаёт состав семьи).
std::string_view randomFirstName(Rng& rng, bool female);
std::string_view randomLastName(Rng& rng);
// Женское ли имя (по списку имён): для окончаний в подписях («погиб» —
// «погибла»). Неизвестное имя — мужское.
bool isFemaleName(std::string_view first);

// Подпись сущности для интерфейса и сводок: «John Miller #1745», если у неё
// есть имя, иначе «#1745».
std::string labelOf(const entt::registry& registry, entt::entity entity, EntityId id);

} // namespace lw
