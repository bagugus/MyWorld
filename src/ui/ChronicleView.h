#pragma once

#include "sim/Chronicle.h"
#include "sim/Id.h"

namespace lw {

class World;

namespace ui {

class Inspector;

// Строка летописи: «день 3, 14:05  #2100 обыскал здание #812 — еды 2, воды 1».
// Участник self (тот, чью биографию смотрят) по имени не называется. Номера —
// ссылки; возвращает id, по которому щёлкнули, иначе kInvalidId.
EntityId drawChronicleEvent(const ChronicleEvent& event, EntityId self);

// Окно «Летопись»: события всего мира, новые сверху. Ссылки выбирают объект
// в инспекторе.
void drawChroniclePanel(const World& world, Inspector& inspector);

} // namespace ui
} // namespace lw
