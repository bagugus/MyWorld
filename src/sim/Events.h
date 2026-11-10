#pragma once

#include "sim/Components.h"
#include "sim/Sound.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <optional>

namespace lw {

class Rng;
class World;

// Ивенты (этап 7.1): свойства видов и вести о них. Ивент ничего не решает за
// людей — он ставит в мир груз, шум и знак; кто узнал, кто пошёл, кто успел —
// решают системы. Подробности и числа — docs/architecture.md, раздел «Ивенты».

// Как человек узнал об ивенте (летопись LearnedOfEvent, a).
enum class EventClue : std::uint8_t {
    Saw,   // увидел знак: дым над местом
    Heard, // услышал шум ивента: гул самолёта
    Told,  // рассказали
};

struct EventSpec {
    const char* name = "";       // короткое английское имя — для lw_headless
    // Разовый звук в начале: на него идут зомби (угроза). loudness — звук мира
    // (World::emitSound), слышен в стольких метрах в тихую погоду.
    SoundKind sound = SoundKind::None;
    float loudness = 0.0f;
    // Люди узнают об ивенте по шуму дальше, чем его слышат зомби: гул самолёта
    // в небе разносится далеко, но не зовёт к земле. И из дома тоже.
    float heardRange = 0.0f;
    // Знак: сколько игровых минут виден и с какого расстояния днём в ясную
    // погоду (ночью, в туман и дождь — меньше, как у зрения).
    int signalMinutes = 0;
    float signalRange = 0.0f;
    int cachesMin = 1;           // грузов
    int cachesMax = 1;
    // Экипаж (этап 7.7): солдаты — люди с именами, как у пункта эвакуации.
    // Каждый выживает с вероятностью crewSurvive (раненым — кровотечение,
    // перевязаться есть чем) и укушен с вероятностью crewBitten: погибший
    // укушенный встанет у места ивента, выживший — заражён и придёт к людям.
    int crewMin = 0;
    int crewMax = 0;
    float crewSurvive = 0.0f;
    float crewBitten = 0.0f;
    // Зомби у места: те, от кого отбивалась колонна (этап 7.7).
    int zombiesMin = 0;
    int zombiesMax = 0;
};

const EventSpec& eventSpec(EventKind kind);
// Вид по имени из eventSpec; nullopt — такого нет.
std::optional<EventKind> eventKindByName(const char* name);

// Весть устаревает за сутки: чем она старше, тем вернее груз уже растащили.
inline constexpr std::uint64_t kLeadTicks = kTicksPerGameDay;

inline bool leadFresh(const Lead& lead, std::uint64_t tick) {
    return tick < lead.since + kLeadTicks;
}
// Свежесть вести: 1 — только что, к kLeadTicks — 0.
inline float leadFreshness(const Lead& lead, std::uint64_t tick) {
    if (!leadFresh(lead, tick)) {
        return 0.0f;
    }
    return 1.0f - static_cast<float>(tick - lead.since) / static_cast<float>(kLeadTicks);
}

// Наполнить груз ивента: что лежит в ящике.
void fillCache(Inventory& inventory, EventKind kind, Rng& rng);

// Узнать о месте ивента: весть в Leads (новая вытесняет самую старую) и запись
// в летописи. source — кто рассказал (EventClue::Told), since — когда узнал
// он сам (у него и весть того же возраста); без source — сейчас. false — уже
// знал или у человека нет Leads.
bool learnOf(World& world, entt::entity person, EntityId event, Cell site, EventClue clue,
             EntityId source = kInvalidId, std::uint64_t since = 0);

// Отметить весть: побывал там; empty — и там ничего не осталось.
void markVisited(Leads& leads, EntityId event, bool empty);
// Узнал, что там пусто: не пойдёт. false — о месте не знал или уже знал, что пусто.
bool hearEmpty(Leads& leads, EntityId event);

// Какую весть teller расскажет listener в разговоре. Сначала — что там пусто,
// если тот туда собирается (Lead::empty; это скажет любой, кроме недруга).
// Иначе — свежую, о месте, где сам ещё не был, которой тот не знает и которую
// он заметил сам (пересказ из вторых рук дальше не идёт), — если хочет
// делиться. Товарищу по группе и другу (симпатия не ниже kShareLeadAffinity) —
// всегда; остальным — только нежадный (жадность ниже kKeepLeadGreed): груза на
// всех не хватит. nullptr — нечего или не хочет.
inline constexpr float kShareLeadAffinity = 0.3f;
inline constexpr float kKeepLeadGreed = 0.45f;
const Lead* leadToShare(const World& world, entt::entity teller, entt::entity listener);

} // namespace lw
