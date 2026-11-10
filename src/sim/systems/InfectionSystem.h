#pragma once

#include "sim/Components.h"
#include "sim/Rng.h"

namespace lw {

class World;

// Заражение от больных (этап 7.5). Правила, течение и заразность —
// sim/Infection.h; подробности и числа — docs/architecture.md, «Вспышка и
// заражение».

// Сколько заражений в игровой час рядом с заразным в полную силу (contagiousness
// = 1) — по тому, как они рядом. Вероятность за минуту — это / 60 × заразность.
inline constexpr float kHouseholdRate = 0.05f; // под одной крышей: ночь, ужин, уход
inline constexpr float kCareRate = 0.35f;      // в больнице: осмотр, перевязка, очередь
inline constexpr float kContactRate = 0.25f;   // лицом к лицу: разговор, встреча, обмен
inline constexpr float kNearbyRate = 0.04f;    // просто рядом: на работе, в магазине
inline constexpr float kBodyRate = 0.3f;       // у тела: обыскивает, оплакивает
// Осторожность (этап 7.6): какую долю частоты пути снимает тревога у того, кто
// ещё здоров (alarmWorry — от 0 до «странной болезни» до 1 у ужаса). Дома
// больного не бросишь, поэтому меньше всего.
inline constexpr float kHouseholdCaution = 0.4f;
inline constexpr float kCareCaution = 0.6f;
inline constexpr float kContactCaution = 0.6f;
inline constexpr float kNearbyCaution = 0.7f;
inline constexpr float kBodyCaution = 0.6f;

// Раз в игровую минуту: кто заразился рядом с больными и телами заражённых.
// Порядок — по id заразных, рядом с каждым — по id; рандом — поток
// RngStream::Outbreak.
void tickInfection(World& world);

// Нулевой пациент (начало вспышки, ScenarioSystem) или новый завозной случай
// (imported): незаражённый взрослый житель с домом, где живёт не один
// (живущий один — только если других нет), чаще тот, у кого больше людей
// вокруг, — работает, живёт с большой семьёй, в городе. Заражается (Origin)
// раньше, чем это заметно: заражение сразу kPatientZeroInfection — через
// несколько часов он заразен, к вечеру слёг. В летописи — PatientZero с
// объектом scenario (a = 1 — завозной). Возвращает его id; kInvalidId —
// подходящих жителей нет.
inline constexpr float kPatientZeroInfection = 0.2f;
EntityId infectPatientZero(World& world, EntityId scenario, Rng& rng, bool imported = false);

} // namespace lw
