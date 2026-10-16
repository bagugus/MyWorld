#pragma once

#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/TimeScale.h"

#include <cstdint>
#include <optional>

namespace lw {

class Rng;
class World;

// Ивенты (этап 7.1). Ивент — «камень в воду»: startEvent ставит в мир грузы
// (ценность), издаёт громкий звук (угроза: на него идут зомби, его слышат
// люди) и зажигает знак (дым), который видно издалека. Дальше — системы:
// заметивший получает весть (Leads), рассказывает её в разговоре
// (SocialSystem), идёт к месту (Utility AI, Action::Salvage) и забирает из
// груза что унесёт. Подробности и числа — docs/architecture.md, раздел «Ивенты».
//
// updateEvents — раз в игровую минуту: кто заметил знак, какие ивенты
// кончились; раз в час — временное расписание (WorldConfig::events), пока
// ивенты не начинает рассказчик (этап 7.4).

// Знак замечают с такой вероятностью за минуту: в небо смотрят не всё время;
// из дома — в окно, в столько раз реже.
inline constexpr float kNoticeChance = 0.2f;
inline constexpr float kIndoorsNotice = 0.25f;
// Ивент кончается, когда знак погас и грузы пусты, но не позже этого: дальше
// непустые грузы остаются в мире сами по себе.
inline constexpr std::uint64_t kEventMaxTicks = 4 * kTicksPerGameDay;
// Временное расписание: столько ивентов в сутки в среднем, днём с 7 до 19 ч.
inline constexpr float kEventsPerDay = 0.4f;
inline constexpr int kEventFirstHour = 7;
inline constexpr int kEventLastHour = 19;

// Где случится ивент этого вида: для падения припасов — открытое место
// (не лес, не вода, не дорога) в 300–1500 м от края поселения, чтобы дым
// видели люди. nullopt — места нет.
std::optional<Cell> pickEventSite(const World& world, EventKind kind, Rng& rng);

// Начать ивент. near — где (ближайшая проходимая клетка); без него место
// выбирает pickEventSite. Возвращает id ивента; kInvalidId — места нет.
// Рандом — поток RngStream::Events.
EntityId startEvent(World& world, EventKind kind, std::optional<Cell> near = std::nullopt);

void updateEvents(World& world);

} // namespace lw
