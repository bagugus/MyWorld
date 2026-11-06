#pragma once

#include "sim/Cast.h"
#include "sim/Foresight.h"
#include "sim/Story.h"

#include <string>

namespace lw {

class World;

namespace ui {

// Тексты о сюжетных линиях и предвестиях для режиссёра (окно, подписи) и
// шоураннера (титры сериала). Только читают мир.

const char* heroStatusTitle(HeroStatus status);
// «Месть», «Орда», «Обращение», «Гонка за грузом», «Кража».
const char* omenTitle(ForesightKind kind);
// «Вражда», «Предательство», «Группа», «Заражение», «Гонка за грузом».
const char* kindTitle(const Story& story);
const char* stageTitle(StoryStage stage);
// «месть», «примирение», …; None — пусто.
const char* endingTitle(StoryEnding ending);
// «нарастание — чем кончится: месть?» или «развязка: месть».
std::string stateLine(const Story& story);
// «Вражда: John Miller #12 — Peter Green #40».
std::string titleOf(const Story& story);
// «John Miller #12 идёт мстить Peter Green #40 — 120 м, через ~2 мин».
std::string omenLine(const World& world, const Foresight& omen);

} // namespace ui
} // namespace lw
