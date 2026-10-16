#pragma once

#include "sim/Components.h"

#include <cstdint>

namespace lw {

class World;

// Отношения людей (этап 6.2): кто кого знает и как к нему относится.
// Связь (Bond) — взгляд одного человека на другого; меняет её только он сам
// (SocialSystem), поэтому порядок обхода на результат не влияет.
//
// Знакомство растёт от времени, проведённого рядом (в разговоре — быстрее) и
// без встреч понемногу забывается. Симпатия тянется к совместимости
// характеров (chemistry): одни сходятся, другие друг друга не выносят. Доверие
// растёт вслед за симпатией и знакомством.
//
// Долг и обида — от поступков, которые человек запомнил (sim/Memory.h):
// спас — долг, обокрал или бросил — обида. Сразу сдвигают симпатию и доверие,
// а пока не угасли, держат симпатию выше или ниже совместимости и не дают
// доверию вырасти обратно.

// Другом считает с такой симпатии и знакомства; перестаёт — ниже kFriendLost.
inline constexpr float kFriendAffinity = 0.5f;
inline constexpr float kFriendFamiliarity = 0.3f;
inline constexpr float kFriendLost = 0.3f;
// Невзлюбил — с такой симпатии и знакомства; отходит — выше kFoeLost.
inline constexpr float kFoeAffinity = -0.3f;
inline constexpr float kFoeFamiliarity = 0.2f;
inline constexpr float kFoeLost = -0.1f;
// Связь слабее этого вытесняется новым знакомством, когда мест нет.
inline constexpr float kForgettable = 0.15f;
// Соседям по дому совместимость выше: живут вместе, это почти семья.
inline constexpr float kHousemateChemistry = 0.3f;

// Совместимость: к какой симпатии self придёт к other, проведя с ним время.
// Общая часть — из хеша пары id (сошлись или нет), к ней — что self видит в
// характере other: доброта нравится, агрессия отталкивает мирных, а
// общительный ко всем теплее. От −1 до 1, в среднем около 0,2.
float chemistry(EntityId self, const Traits& mine, EntityId other, const Traits& theirs);

// Насколько связь держится в памяти: знакомство, сила чувства, долг и обида.
float bondStrength(const Bond& bond);

// Насколько другой близок: 0..1, знакомство × (0,5 + 0,5 × симпатия). Близкий —
// тот, чья смерть отзывается горем, а бегство — обидой.
float closeness(const Bond& bond);

// К чему тянется симпатия, когда они вместе: совместимость, долг поднимает её,
// обида опускает.
float affinityGoal(const Bond& bond);
// Выше этого доверие от общения не растёт: max(0, симпатия) × знакомство,
// обида срезает долю.
float trustCeiling(const Bond& bond);

// Стал обязан (долг растёт на amount) или затаил обиду (обида растёт на
// amount): сразу сдвигаются симпатия и доверие. Отметки «друг» и «недруг» —
// по updateBondFlags, её вызывает тот, кто пишет летопись.
void addDebt(Bond& bond, float amount);
void addGrudge(Bond& bond, float amount);

// Связь с other: найти или завести новую (знакомство и симпатия — ноль).
// Мест нет — вытесняется самая слабая, если она слабее kForgettable; иначе
// nullptr. created — заведена ли новая.
Bond* acquireBond(Relations& relations, EntityId other, float chemistry, std::uint64_t tick,
                  bool* created = nullptr);

// Поставить или снять отметки «друг» и «недруг» по порогам. Возвращает
// отметку, которая только что поставлена (Bond::kFriend, Bond::kFoe), или 0.
std::uint8_t updateBondFlags(Bond& bond);

// Знакомства с начала мира у всех людей с Relations: соседи по дому знают
// друг друга хорошо, а ещё каждый знает нескольких ближайших соседей по
// поселению. Существующие связи не меняются, поэтому можно звать повторно,
// когда в мире появляются новые люди. Рандома нет.
void introduceNeighbours(World& world);

} // namespace lw
