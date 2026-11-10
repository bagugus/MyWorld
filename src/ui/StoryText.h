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

// Для титров и подписей (12а.10) — человеческим языком, без служебных слов
// (стадий линии, названий видов линий и предвестий, метров и минут). Имена —
// подписями интерфейса (idLabel): в кадре сериала номер срезает frame::clean.
// Линия: «Peter Green #40 предал John Miller #12», «John Miller #12 отомстил
// Peter Green #40», «Mary Cole #7 укушена», «Груз так никому и не достался».
std::string storyHeadline(const World& world, const Story& story);
// Предвестие: «John Miller #12 идёт мстить Peter Green #40 — он уже совсем
// близко», «Орда идёт к Old Village», «Тело Mary Cole #7 вот-вот встанет».
std::string omenHeadline(const World& world, const Foresight& omen);

} // namespace ui
} // namespace lw
