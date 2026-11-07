#pragma once

#include "sim/Components.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <optional>

namespace lw {

class Rng;
class World;

// Реакция общества (этап 7.6). Катастрофу встречают люди, которые сначала
// ничего не знают. Что они знают и во что верят — тревога (Alarm): растёт от
// увиденного (больной в жару, укушенный, тело, зомби), от рассказов (слух
// слабеет с каждым пересказом) и от официальных передач по радио и
// телевизору. От тревоги — страх, недоверие к чужим, осторожность рядом с
// больными (меньше заражений), скупка впрок, эвакуация, «бегут к своим».
//
// Пока власть работает (вспышка и паника), она отвечает: объявляет карантин в
// поселениях, где есть больные и зомби (Quarantine), с паникой открывает в
// городах пункты эвакуации (EvacuationPoint), куда приходят солдаты и откуда
// дважды в день уходит колонна. С коллапсом армия уходит — часть солдат
// остаётся. Система — SocietySystem; подробности и числа — docs/architecture.md,
// раздел «Реакция общества».

// --- Тревога ------------------------------------------------------------------

// Стадии тревоги: по ним пишется летопись (Alarmed, a) и говорят титры.
enum class AlarmStage : std::uint8_t {
    Calm,      // ничего не знает
    Uneasy,    // ходит странная болезнь
    Alarmed,   // больные нападают, это заразно и опасно
    Terrified, // мёртвые встают
};
inline constexpr float kUneasyLevel = 0.25f;
inline constexpr float kAlarmedLevel = 0.5f;
inline constexpr float kTerrifiedLevel = 0.8f;

AlarmStage alarmStage(float level);
// Тревога человека, 0..1; нет компонента — 0.
float alarmOf(const entt::registry& registry, entt::entity entity);
// Насколько тревога тревожит: 0 до «странной болезни», к ужасу — 1.
float alarmWorry(float level);

// Что видно своими глазами — уровень тревоги, до которого поднимает.
float clueLevel(AlarmClue clue);

// Поднять тревогу человека до level (ниже нынешней — ничего): откуда узнал и
// кто рассказал (кого видел). Новая стадия — запись в летописи (Alarmed).
// false — тревога не выросла или у человека нет Alarm.
bool raiseAlarm(World& world, entt::entity person, float level, AlarmClue clue, EntityId source);

// Насколько listener поверит рассказу teller о беде: роднёй, товарищем и
// другом — kCloseCredibility, знакомым — kKnownCredibility, чужим — меньше.
inline constexpr float kCloseCredibility = 0.85f;
inline constexpr float kKnownCredibility = 0.7f;
inline constexpr float kStrangerCredibility = 0.55f;
float credibility(const World& world, entt::entity listener, EntityId teller);

// До какого уровня teller поднимет тревогу listener рассказом; 0 — рассказывать
// нечего (сам ничего не знает или тот знает не меньше). Рассказывают все, кто
// знает: это не тайна, а новость.
float alarmToShare(const World& world, entt::entity teller, entt::entity listener);

// Официальные сообщения по радио и телевизору (Scenario::broadcast, летопись
// Broadcast, a). Значения попадают в хеш: новые добавлять только в конец.
enum class BroadcastKind : std::uint8_t {
    None,
    Epidemic,   // эпидемия неизвестной болезни: оставайтесь дома, карантин
    Evacuation, // больные нападают на людей: эвакуация из городов
    Farewell,   // последняя передача: армия уходит, спасайтесь сами
    Count
};
// До какого уровня тревоги поднимает сообщение тех, кто его услышал.
float broadcastLevel(BroadcastKind kind);
// Передать сообщение (если оно новее прежнего): Scenario::broadcast и летопись.
void broadcast(World& world, BroadcastKind kind, EntityId settlement = kInvalidId);

// --- Карантин -----------------------------------------------------------------

// Власть ещё работает: вспышка или паника, и хоть в одном полицейском участке
// или больнице кто-то выходит на работу.
bool authorityPresent(const World& world);

// Поселение на карантине, внутри кордона которого точка; kInvalidId — нет.
// Кордон — круг поселения и ещё kCordonMargin метров.
inline constexpr float kCordonMargin = 40.0f;
EntityId quarantineAt(const World& world, float x, float y);
inline EntityId quarantineAt(const World& world, Cell cell) {
    return quarantineAt(world, Map::cellCenter(cell.x), Map::cellCenter(cell.y));
}

// Случаи беды в поселении: заметно больные жители, зомби и тела, которые
// встанут, внутри его круга.
int quarantineCases(const World& world, EntityId settlement);

// Клетки дорог у кордона поселения — где стоят полицейские: по возрастанию
// (y, x). Пусто — дорог нет.
std::vector<Cell> cordonExits(const World& world, EntityId settlement);

// --- Эвакуация ----------------------------------------------------------------

// Колонна уходит из пункта в эти часы; мест в ней — kConvoySeats.
inline constexpr int kConvoyHours[] = {10, 16};
inline constexpr int kConvoySeats = 20;
// Солдат в пункте.
inline constexpr int kSoldiersPerPoint = 4;

// Ближайший к точке пункт эвакуации не дальше range метров; nullopt — нет.
struct EvacuationSite {
    EntityId id = kInvalidId; // здание
    Cell door;
    float distance = 0.0f;
};
std::optional<EvacuationSite> nearestEvacuation(const World& world, float x, float y, float range);
// Есть ли хоть один пункт.
bool evacuationOpen(const World& world);
// Тик ближайшей колонны после tick (сегодня или завтра).
std::uint64_t nextConvoyTick(std::uint64_t tick);

// Солдат ли (по профессии).
bool isSoldier(const entt::registry& registry, entt::entity entity);
// Чувство долга солдата: смелость и лидерство (дисциплина).
float soldierDiscipline(const Traits& traits);

// Увезти человека из края: летопись Evacuated, поведение и позиция снимаются,
// остаются имя, возраст, родня, работа и черты (Evacuated). Вызывать только
// вне обхода Utility AI (SocietySystem).
void evacuate(World& world, entt::entity person, EntityId point, bool soldier);

} // namespace lw
