#pragma once

#include "sim/Components.h"
#include "sim/Id.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lw {

class World;

namespace ui {

// Подписи и титры режиссёра (этап 12а.6): тексты о конкретных людях из их
// действия, памяти, нужд и летописи. Только читают мир.

// Женщина ли (по имени): для окончаний «погиб» — «погибла».
bool isFemale(const World& world, EntityId id);

// Что человек хочет сейчас и почему, одной строкой без имени: «идёт мстить
// Peter Green #40 за то, что он обокрал его дом», «бежит от зомби», «спит под
// открытым небом». Тело — «погибла», зомби — «стал зомби». Пусто — не о ком.
std::string intentLine(const World& world, EntityId id);

// Положение человека одной строкой: вожак группы, укушен и в лихорадке,
// истекает кровью, ранен. Пусто — ничего особенного.
std::string statusLine(const World& world, EntityId id);

// Что человек делал, когда за ним следили, — для итога эпизода: при гибели
// важно, защищал ли он дом, убегал или мстил.
struct Witnessed {
    Action action = Action::Idle;
    bool nearHome = false; // у своего дома или в нём
};
// Что делает человек сейчас; false — не человек (тело, зомби, нет в мире).
bool observe(const World& world, EntityId id, Witnessed& witnessed);

// Итог эпизода одной строкой: самое значимое, что случилось с id с тика
// since, — «Mary Cole #12 погибла, защищая дом», «John Miller #3 отомстил
// Peter Green #40». Пусто — ничего значимого.
std::string episodeSummary(const World& world, EntityId id, std::uint64_t since,
                           const Witnessed& witnessed);

// Значимые события биографии id (номера в Chronicle::events()) с тиками в
// [from, to), не больше count самых весомых, по порядку времени. Для «ранее в
// сериях» и «пока нас не было».
std::vector<std::uint32_t> notableEvents(const World& world, EntityId id, std::uint64_t from,
                                         std::uint64_t to, std::size_t count);

} // namespace ui
} // namespace lw
