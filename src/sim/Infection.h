#pragma once

#include "sim/Components.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

namespace lw {

class World;

// Вспышка и заражение (этап 7.5). Вирус один, путей два: укус зомби (этап 5.5)
// и больной рядом. Зомби — бывшие жители: каждый заражённый, погибнув от
// чего угодно, встаёт (updateBodies).
//
// Течение. Заражение (Wounds::infection) растёт само от затравки до предела
// (смерть) за infectionHours: после укуса — быстро (kInfectionHours, sim/Death.h),
// от больного — медленнее (kContactInfectionHours): скрытый период длиннее, и
// заразившийся успевает сходить на работу, в школу и к родне. С kFeverStart —
// лихорадка: человек слёг (Utility AI: дома в постели, на работу почти не
// выходят, гулять не тянет), здоровье уходит (NeedsSystem).
//
// Заразность. Заразен больной с kContagiousFrom: в конце скрытого периода —
// слабо (kLatentShedding), в лихорадке — в полную силу; заразно и тело
// заражённого, пока не встало. Передача — в InfectionSystem: раз в игровую
// минуту каждый незаражённый рядом с заразным (ближе kContactRadius)
// заражается с вероятностью по тому, как они рядом (InfectionRoute): под одной
// крышей, в больнице (врач и больной), лицом к лицу (разговор, встреча,
// обмен), просто рядом, у тела. Подробности и числа — docs/architecture.md,
// «Вспышка и заражение».

// Затравка заражения от больного и у нулевого пациента: дальше растёт само.
inline constexpr float kInfectionSeed = 0.01f;
// Часы от затравки до предела после заражения от больного.
inline constexpr float kContactInfectionHours = 40.0f;
// С этого уровня — лихорадка (этап 5.5).
inline constexpr float kFeverStart = 0.5f;
// С этого уровня больной заразен; до лихорадки — с такой силой.
inline constexpr float kContagiousFrom = 0.3f;
inline constexpr float kLatentShedding = 0.35f;
// Тело заражённого (пока не встало) заразно с такой силой — тому, кто стоит
// у него (обыскивает, оплакивает), а не проходит мимо.
inline constexpr float kBodyShedding = 0.5f;
// Рядом — ближе этого, метры: в одной клетке (у двери дома, на месте работы,
// в разговоре).
inline constexpr float kContactRadius = 3.0f;

// Заражён ли.
inline bool isInfected(const Wounds& wounds) {
    return wounds.infection > 0.0f;
}
bool isInfected(const entt::registry& registry, entt::entity entity);
// Сколько игровых часов заражение идёт от затравки до предела: после укуса
// быстрее.
float infectionHours(const Wounds& wounds);
// Лихорадка: 0 — нет, дальше растёт к 1 у предела.
float feverOf(const Wounds& wounds);
inline bool isFeverish(const Wounds& wounds) {
    return wounds.infection >= kFeverStart;
}
// Насколько больной заразен, 0..1: 0 — не заразен.
float contagiousness(const Wounds& wounds);
// Заметно ли заражение окружающим: укус или лихорадка. Скрытый период не
// замечает никто, даже сам больной (этап 7.6 — слухи и страх).
inline bool isVisiblyIll(const Wounds& wounds) {
    return isInfected(wounds) && (wounds.bites > 0 || isFeverish(wounds));
}
// Через сколько игровых часов начнётся лихорадка (0 — уже) и наступит предел.
float hoursToFever(const Wounds& wounds);
float hoursToEnd(const Wounds& wounds);

// Заразить человека, если он ещё не заражён: затравка, путь, от кого, когда.
// Летопись: от больного или тела — Infected; укус пишет CombatSystem (Bitten),
// нулевого пациента (Origin) — infectPatientZero (PatientZero). false — уже
// заражён или не человек.
bool infect(World& world, entt::entity person, InfectionRoute route, EntityId source);

// Как заразился человек id: у живого — из ран, у погибшего и обратившегося —
// по летописи (первый укус, заражение или «нулевой пациент»). None — не
// заражался.
InfectionRoute infectionRouteOf(const World& world, EntityId id);

// Короткое английское имя пути — для lw_headless.
const char* infectionRouteName(InfectionRoute route);

} // namespace lw
