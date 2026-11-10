#include "ui/Narrator.h"

#include "sim/Calendar.h"
#include "sim/Family.h"
#include "sim/Infection.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/Scenario.h"
#include "sim/Story.h"
#include "sim/Work.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/Captions.h"
#include "ui/Format.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace lw::ui {

namespace {

// Черта выражена, если дальше этого от середины (черты — колокол вокруг 0,5).
constexpr float kMarked = 0.18f;
// Реплика-характеристика: черта не слабее этого.
constexpr float kStrongTrait = 0.68f;
constexpr float kWeakTrait = 0.32f;
// Нужда, которую называют.
constexpr float kPressing = 0.6f;
// Ту же реплику о том же человеке — не чаще, реальных секунд.
constexpr float kRemarkRepeat = 900.0f;
// Ту же фразу — не чаще, реальных секунд.
constexpr float kRepeatLine = 600.0f;
// Фраза держится: основа плюс время на чтение, но в пределах.
constexpr float kLineBase = 2.0f;
constexpr float kCharsPerSecond = 12.0f; // зритель читает не спеша
constexpr float kLineMin = 4.0f;
constexpr float kLineMax = 15.0f;
constexpr float kLineFade = 0.5f;
constexpr float kLineGap = 0.6f;
// Воспоминание для знакомства — не слабее этого.
constexpr float kMemoryWorth = 0.25f;

// Мужская или женская форма.
const char* g(bool female, const char* male, const char* femaleForm) {
    return female ? femaleForm : male;
}

entt::entity entityOf(const World& world, EntityId id) {
    return id == kInvalidId ? entt::null : world.find(id);
}

bool female(const World& world, EntityId id) {
    return isFemale(world, id);
}

// Живой человек (не тело и не зомби).
bool alivePerson(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    return entity != entt::null && world.registry().all_of<Person>(entity) &&
           !world.registry().any_of<Corpse, Zombie>(entity);
}

bool named(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    return entity != entt::null && world.registry().all_of<Name>(entity);
}

// «John Miller»; безымянный — «зомби».
std::string fullName(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity != entt::null) {
        if (const auto* name = world.registry().try_get<Name>(entity)) {
            return name->first + " " + name->last;
        }
    }
    return "зомби";
}

std::string firstName(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity != entt::null) {
        if (const auto* name = world.registry().try_get<Name>(entity)) {
            return name->first;
        }
    }
    return "зомби";
}

// Местоимения: он/она, его/её, ему/ей, нём/ней.
const char* he(bool f) { return f ? "она" : "он"; }
const char* him(bool f) { return f ? "её" : "его"; }      // винительный и родительный
const char* toHim(bool f) { return f ? "ей" : "ему"; }    // дательный
const char* withHim(bool f) { return f ? "ней" : "ним"; } // творительный после предлога

// Первая буква — заглавная (латиница и кириллица в UTF-8).
std::string capitalize(std::string text) {
    if (text.empty()) {
        return text;
    }
    auto* bytes = reinterpret_cast<unsigned char*>(text.data());
    if (bytes[0] < 0x80) {
        if (bytes[0] >= 'a' && bytes[0] <= 'z') {
            bytes[0] = static_cast<unsigned char>(bytes[0] - 'a' + 'A');
        }
    } else if (text.size() >= 2) {
        if (bytes[0] == 0xD0 && bytes[1] >= 0xB0 && bytes[1] <= 0xBF) {
            bytes[1] = static_cast<unsigned char>(bytes[1] - 0x20); // а..п → А..П
        } else if (bytes[0] == 0xD1 && bytes[1] >= 0x80 && bytes[1] <= 0x8F) {
            bytes[0] = 0xD0; // р..я → Р..Я
            bytes[1] = static_cast<unsigned char>(bytes[1] + 0x20);
        } else if (bytes[0] == 0xD1 && bytes[1] == 0x91) {
            bytes[0] = 0xD0; // ё → Ё
            bytes[1] = 0x81;
        }
    }
    return text;
}

// Символов (не байтов) в строке UTF-8.
std::size_t glyphs(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

// «1 человек», «3 человека», «5 человек».
std::string people(int count) {
    const int tens = count % 100;
    const int ones = count % 10;
    const bool few = ones >= 2 && ones <= 4 && (tens < 12 || tens > 14);
    return std::to_string(count) + (few ? " человека" : " человек");
}

// «Mary», «Mary и Tom», «Mary, Tom и Ann».
std::string listNames(const std::vector<std::string>& names) {
    std::string line;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            line += i + 1 == names.size() ? " и " : ", ";
        }
        line += names[i];
    }
    return line;
}

// Поселение здания: «деревни Липки» (из …) и «в деревне Липки».
struct Town {
    std::string from;
    std::string in;
};

Town townOf(const World& world, EntityId building) {
    const entt::entity entity = entityOf(world, building);
    const auto* info = entity != entt::null ? world.registry().try_get<Building>(entity) : nullptr;
    const entt::entity place = info != nullptr ? entityOf(world, info->settlement) : entt::null;
    const auto* settlement = place != entt::null ? world.registry().try_get<Settlement>(place) : nullptr;
    if (settlement == nullptr) {
        return {};
    }
    const bool town = settlement->kind == SettlementKind::Town;
    return {std::string(town ? "города " : "деревни ") + settlement->name,
            std::string(town ? "в городе " : "в деревне ") + settlement->name};
}

// Насколько поступок весит для рассказа о человеке.
float memoryWeight(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Killed:
    case MemoryKind::Bereaved:
        return 1.2f;
    case MemoryKind::Betrayed:
    case MemoryKind::Turned:
        return 1.1f;
    case MemoryKind::Saved:
        return 1.0f;
    case MemoryKind::Assaulted:
        return 0.9f;
    case MemoryKind::Robbed:
    case MemoryKind::Expelled:
    case MemoryKind::Abandoned:
        return 0.8f;
    case MemoryKind::Defended:
        return 0.6f;
    case MemoryKind::Helped:
        return 0.5f;
    default:
        return 0.0f;
    }
}

bool grievance(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Robbed:
    case MemoryKind::Betrayed:
    case MemoryKind::Assaulted:
    case MemoryKind::Abandoned:
    case MemoryKind::Killed:
    case MemoryKind::Expelled:
        return true;
    default:
        return false;
    }
}

bool kindness(MemoryKind kind) {
    return kind == MemoryKind::Saved || kind == MemoryKind::Defended || kind == MemoryKind::Helped;
}

// Что случилось, одним предложением без начала: «Peter Green обокрал его дом»,
// «тот обокрал его дом» (doer — как назвать сделавшего). holder — кто помнит.
std::string memoryClause(const World& world, EntityId holder, const Memory& memory, const std::string& doer) {
    const bool hf = female(world, holder);
    const bool df = female(world, memory.who);
    const bool toSelf = memory.victim == kInvalidId || memory.victim == holder;
    const std::string victim = toSelf ? std::string(him(hf)) : fullName(world, memory.victim);
    switch (memory.kind) {
    case MemoryKind::Robbed:
        return toSelf ? doer + " " + g(df, "обокрал", "обокрала") + " " + him(hf) + " дом"
                      : doer + " " + g(df, "обокрал", "обокрала") + " дом " + victim;
    case MemoryKind::Saved:
        return toSelf ? doer + " " + g(df, "спас", "спасла") + " " + toHim(hf) + " жизнь"
                      : doer + " " + g(df, "спас", "спасла") + " " + victim;
    case MemoryKind::Defended:
        return doer + " " + g(df, "заступился", "заступилась") + " за " +
               (toSelf ? std::string(hf ? "неё" : "него") : victim);
    case MemoryKind::Abandoned:
        return doer + " " + g(df, "бросил", "бросила") + " " + victim + " в беде";
    case MemoryKind::Bereaved:
        if (memory.about != kInvalidId && named(world, memory.about)) {
            return fullName(world, memory.about) + " " + g(female(world, memory.about), "убил", "убила") + " " +
                   fullName(world, memory.who);
        }
        return std::string(he(hf)) + " " + g(hf, "потерял", "потеряла") + " " + fullName(world, memory.who);
    case MemoryKind::Turned:
        return std::string(he(hf)) + " " + g(hf, "узнал", "узнала") + " " + fullName(world, memory.who) +
               " среди зомби";
    case MemoryKind::Expelled:
        return doer + " " + g(df, "выгнал", "выгнала") + " " + victim + " из группы";
    case MemoryKind::Helped:
        return doer + " " + g(df, "поделился", "поделилась") + " с " + (toSelf ? withHim(hf) : victim) +
               " последним";
    case MemoryKind::Betrayed:
        return toSelf ? doer + ", " + g(df, "которому", "которой") + " " + he(hf) + " " +
                            g(hf, "доверял", "доверяла") + ", " + g(df, "предал", "предала") + " " + him(hf)
                      : doer + " " + g(df, "предал", "предала") + " " + victim;
    case MemoryKind::Assaulted:
        return doer + " " + g(df, "напал", "напала") + " на " + (toSelf ? std::string(hf ? "неё" : "него") : victim);
    case MemoryKind::Killed:
        return std::string(he(hf)) + " " + g(hf, "видел", "видела") + ", как " + doer + " " +
               g(df, "убил", "убила") + " " + victim;
    default:
        return {};
    }
}

// Самое весомое, что человек помнит: о чём рассказать при знакомстве.
const Memory* notableMemory(const World& world, entt::entity entity) {
    const auto* memories = world.registry().try_get<Memories>(entity);
    if (memories == nullptr) {
        return nullptr;
    }
    const Memory* best = nullptr;
    float bestScore = kMemoryWorth;
    for (std::size_t i = 0; i < memories->count; ++i) {
        const Memory& memory = memories->items[i];
        if (!named(world, memory.who)) {
            continue; // о безымянных (зомби) рассказывать нечего
        }
        float score = memory.strength * memoryWeight(memory.kind);
        if (memory.victim != kInvalidId) {
            score *= 0.6f; // чужая беда
        }
        if (memory.source != kInvalidId) {
            score *= 0.6f; // слух
        }
        if (score > bestScore) {
            bestScore = score;
            best = &memory;
        }
    }
    return best;
}

// Как зовут обидчика в воспоминании о самом сильном, что other сделал holder.
const Memory* grudgeAgainst(const World& world, entt::entity holder, EntityId other) {
    const auto* memories = world.registry().try_get<Memories>(holder);
    if (memories == nullptr) {
        return nullptr;
    }
    const Memory* best = nullptr;
    for (std::size_t i = 0; i < memories->count; ++i) {
        const Memory& memory = memories->items[i];
        const bool his = memory.who == other && grievance(memory.kind);
        const bool killedKin = memory.kind == MemoryKind::Bereaved && memory.about == other;
        if ((his || killedKin) && (best == nullptr || memory.strength > best->strength)) {
            best = &memory;
        }
    }
    return best;
}

// Добро, которое other сделал holder.
const Memory* kindnessFrom(const World& world, entt::entity holder, EntityId other) {
    const auto* memories = world.registry().try_get<Memories>(holder);
    if (memories == nullptr) {
        return nullptr;
    }
    const Memory* best = nullptr;
    for (std::size_t i = 0; i < memories->count; ++i) {
        const Memory& memory = memories->items[i];
        if (memory.who == other && kindness(memory.kind) &&
            (memory.victim == kInvalidId || memory.victim == world.registry().get<Id>(holder).value) &&
            (best == nullptr || memory.strength > best->strength)) {
            best = &memory;
        }
    }
    return best;
}

// Род в шаблоне: {он}, {его}, {ему}, {него}, {ним}, {нём} — местоимения
// человека; {мужская|женская} — форма слова. С большой буквы — {Он}, {Его}.
std::string fill(std::string_view text, bool f) {
    std::string out;
    out.reserve(text.size() + 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') {
            out += text[i];
            continue;
        }
        const std::size_t close = text.find('}', i);
        if (close == std::string_view::npos) {
            out += text.substr(i);
            break;
        }
        const std::string_view token = text.substr(i + 1, close - i - 1);
        if (const std::size_t bar = token.find('|'); bar != std::string_view::npos) {
            out += f ? token.substr(bar + 1) : token.substr(0, bar);
        } else {
            struct Pronoun {
                std::string_view key;
                const char* male;
                const char* female;
            };
            static constexpr Pronoun kPronouns[] = {
                {"он", "он", "она"},   {"Он", "Он", "Она"},   {"его", "его", "её"},
                {"Его", "Его", "Её"},  {"ему", "ему", "ей"},  {"Ему", "Ему", "Ей"},
                {"него", "него", "неё"}, {"ним", "ним", "ней"}, {"нём", "нём", "ней"},
            };
            bool found = false;
            for (const Pronoun& pronoun : kPronouns) {
                if (pronoun.key == token) {
                    out += f ? pronoun.female : pronoun.male;
                    found = true;
                    break;
                }
            }
            if (!found) {
                out += text.substr(i, close - i + 1);
            }
        }
        i = close;
    }
    return out;
}

// Вариант фразы — по id и месту в тексте: у каждого героя свой голос, но тот
// же самый от раза к разу.
std::size_t variant(EntityId id, std::uint32_t salt, std::size_t count) {
    std::uint64_t x =
        (static_cast<std::uint64_t>(id) << 8) ^ (static_cast<std::uint64_t>(salt) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return static_cast<std::size_t>(x % count);
}

// Профессия: «продавщица» и «продавщицей» (работает кем).
struct Trade {
    std::string name;
    std::string instr;
};

Trade tradeOf(Profession profession, bool f) {
    switch (profession) {
    case Profession::Shopkeeper:
        return f ? Trade{"продавщица", "продавщицей"} : Trade{"продавец", "продавцом"};
    case Profession::Farmer:
        return {"фермер", "фермером"};
    case Profession::Doctor:
        return {"врач", "врачом"};
    case Profession::PoliceOfficer:
        return {"полицейский", "в полиции"};
    case Profession::Teacher:
        return f ? Trade{"учительница", "учительницей"} : Trade{"учитель", "учителем"};
    default:
        return {professionName(profession), professionName(profession)};
    }
}

// Последняя нота воспоминания: «Такое не забывают.»; пусто — без неё.
std::string memoryCoda(MemoryKind kind, bool toSelf, bool f) {
    if (!toSelf) {
        return {};
    }
    switch (kind) {
    case MemoryKind::Robbed:
    case MemoryKind::Betrayed:
    case MemoryKind::Assaulted:
    case MemoryKind::Abandoned:
    case MemoryKind::Expelled:
        return " Такое не забывают.";
    case MemoryKind::Saved:
        return " Этот долг не отдать.";
    case MemoryKind::Bereaved:
        return fill(" С тех пор что-то в {нём} погасло.", f);
    case MemoryKind::Turned:
        return " Это лицо будет сниться долго.";
    case MemoryKind::Helped:
        return " Добро теперь — редкость.";
    default:
        return {};
    }
}

// Характер одной-двумя фразами по самым ярким чертам; пусто — ничего яркого.
std::string character(const Traits& traits, bool f, std::size_t most = 2) {
    struct Word {
        float value;
        const char* high;
        const char* low;
    };
    const Word words[] = {
        {traits.bravery, "Страх {ему} почти незнаком.", "{Он} боится — темноты, дорог, чужих шагов."},
        {traits.altruism, "Последним куском {он} поделится с другим.", "Чужая беда {его} не трогает."},
        {traits.greed, "{Ему} всегда мало.", "Вещи для {него} мало значат."},
        {traits.aggression, "Вспыхивает от одного слова.", "Мухи не обидит."},
        {traits.sociability, "Без людей {он} вянет.", "Людей сторонится."},
        {traits.curiosity, "{Его} вечно тянет за горизонт.", "Дальше своей улицы не ходит."},
        {traits.prudence, "Всё откладывает на чёрный день.", "О завтрашнем дне не думает."},
        {traits.leadership, "{Привык|Привыкла}, что {его} слушают.", "{Привык|Привыкла}, что решают за {него}."},
    };
    // Две самые яркие, по порядку таблицы при равенстве.
    std::vector<std::pair<float, const char*>> picked;
    for (const Word& word : words) {
        const float shift = word.value - 0.5f;
        if (std::abs(shift) >= kMarked) {
            picked.emplace_back(std::abs(shift), shift > 0.0f ? word.high : word.low);
        }
    }
    std::stable_sort(picked.begin(), picked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (picked.empty()) {
        return {};
    }
    std::string line = fill(picked[0].second, f);
    if (picked.size() > 1 && most > 1) {
        line += " " + fill(picked[1].second, f);
    }
    return line;
}

// Тик укуса человека (последний); 0 — не найден.
std::uint64_t bittenAt(const World& world, EntityId id) {
    const Chronicle& chronicle = world.chronicle();
    const std::vector<ChronicleEvent>& events = chronicle.events();
    std::uint64_t tick = 0;
    for (const std::uint32_t index : chronicle.eventsOf(id)) {
        if (events[index].kind == ChronicleKind::Bitten && events[index].subject == id) {
            tick = events[index].tick;
        }
    }
    return tick;
}

// «сегодня утром» — для середины фразы.
std::string agoLower(std::uint64_t then, std::uint64_t now);

// Как человек заразился — началом фразы (этап 7.5): «John укусили сегодня
// утром», «Mary заразилась вчера от John», «Raymond заболел первым». Живой —
// по ранам, погибший — по летописи.
std::string infectedClause(const World& world, EntityId id, bool f) {
    const std::string name = firstName(world, id);
    const InfectionRoute route = infectionRouteOf(world, id);
    if (route == InfectionRoute::Bite || route == InfectionRoute::None) {
        const std::uint64_t at = bittenAt(world, id);
        std::string line = name + " укусили";
        if (at != 0) {
            line += " " + agoLower(at, world.tick());
        }
        return line;
    }
    if (route == InfectionRoute::Origin) {
        return isPatientZero(world, id) ? name + fill(" {заболел первым|заболела первой}", f)
                                        : name + fill(" {заразился|заразилась} неизвестно где", f);
    }
    std::string line = name + fill(" {заразился|заразилась}", f);
    const entt::entity entity = entityOf(world, id);
    if (const auto* wounds = entity != entt::null ? world.registry().try_get<Wounds>(entity) : nullptr) {
        line += " " + agoLower(wounds->infectedAt, world.tick());
        if (named(world, wounds->infectedBy)) {
            line += " от " + firstName(world, wounds->infectedBy);
        }
    }
    return line;
}

std::string agoLower(std::uint64_t then, std::uint64_t now) {
    std::string text = agoPhrase(then, now);
    // Только свои слова: первая буква — кириллица из двух байт.
    auto* bytes = reinterpret_cast<unsigned char*>(text.data());
    if (text.size() >= 2 && bytes[0] == 0xD0) {
        if (bytes[1] >= 0x90 && bytes[1] <= 0x9F) {
            bytes[1] = static_cast<unsigned char>(bytes[1] + 0x20);
        } else if (bytes[1] >= 0xA0 && bytes[1] <= 0xAF) {
            bytes[0] = 0xD1;
            bytes[1] = static_cast<unsigned char>(bytes[1] - 0x20);
        }
    }
    return text;
}

// Положение: биты.
constexpr std::uint8_t kBitten = 1;
constexpr std::uint8_t kFever = 2;
constexpr std::uint8_t kBleeding = 4;
constexpr std::uint8_t kDying = 8;
constexpr std::uint8_t kInfected = 16; // заразился от больного (этап 7.5)

// Местоимения «который» в нужном падеже и роде.
const char* whoAcc(bool f) { return f ? "которую" : "которого"; }   // винительный
const char* whoDat(bool f) { return f ? "которой" : "которому"; }   // дательный
const char* whoPrep(bool f) { return f ? "которой" : "котором"; }   // предложный
const char* whoNom(bool f) { return f ? "которая" : "который"; }

} // namespace

std::string agoPhrase(std::uint64_t then, std::uint64_t now) {
    if (now - std::min(now, then) < kTicksPerGameHour) {
        return "Только что";
    }
    const CalendarTime past = calendarAt(then);
    const CalendarTime today = calendarAt(now);
    const auto dayOf = [](const CalendarTime& time) { return time.gameTick / kTicksPerGameDay; };
    const std::uint64_t days = dayOf(today) - std::min(dayOf(today), dayOf(past));
    if (days == 0) {
        if (past.hour >= 5 && past.hour < 11) {
            return "Сегодня утром";
        }
        if (past.hour >= 11 && past.hour < 17) {
            return "Сегодня днём";
        }
        if (past.hour >= 17 && past.hour < 22) {
            return "Сегодня вечером";
        }
        return "Этой ночью";
    }
    if (days == 1) {
        return past.hour >= 22 || past.hour < 5 ? "Прошлой ночью" : "Вчера";
    }
    if (days == 2) {
        return "Позавчера";
    }
    constexpr const char* kDays[] = {"", "", "", "Три дня назад", "Четыре дня назад", "Пять дней назад",
                                     "Шесть дней назад"};
    if (days < std::size(kDays)) {
        return kDays[days];
    }
    if (days <= 9) {
        return "Неделю назад";
    }
    return "Давно";
}

std::string introduction(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity == entt::null || !named(world, id)) {
        return {};
    }
    const entt::registry& registry = world.registry();
    const bool f = female(world, id);
    if (const auto* corpse = registry.try_get<Corpse>(entity)) {
        if (corpse->kind != CorpseKind::Human) {
            return {};
        }
        return fill("{Его|Её} мы уже не застали {живым|живой}. Осталось только имя.", f);
    }
    if (registry.all_of<Zombie>(entity)) {
        return fill("Когда-то {его} звали так. Теперь от {него} осталась только оболочка.", f);
    }
    const auto* person = registry.try_get<Person>(entity);
    if (person == nullptr) {
        return {};
    }
    std::vector<std::string> sentences;

    // Кто он и откуда. Название поселения не склоняется — при нём слово
    // «город» или «деревня»: «из деревни Липки», «в городе Берёзовск».
    const EntityId home = person->formerHome != kInvalidId ? person->formerHome : person->home;
    const Town town = townOf(world, home);
    const auto* job = registry.try_get<Job>(entity);
    const WorldPhase phase = worldPhase(world);
    const bool employed = isEmployed(job);
    const int years = ageOf(registry, entity);
    // Где живёт, сказано ли уже (с работой или школой).
    bool placed = employed;
    if (registry.all_of<Age>(entity) && years < kAdultAge) {
        // Ребёнок (этап 7.3): возраст и школа.
        const bool pupil = job != nullptr && job->profession == Profession::Pupil;
        std::string line = fill("{Ему|Ей} ", f) + ageText(years) + ".";
        if (pupil && phase < WorldPhase::Panic) {
            line += " Ходит в школу" + (town.in.empty() ? std::string() : " " + town.in) + ".";
            placed = true;
        } else if (pupil) {
            line += " Школа закрыта — и неизвестно, откроется ли снова.";
        }
        sentences.push_back(line);
    } else if (!employed && registry.all_of<Age>(entity) && years >= 67 && phase < WorldPhase::Collapse) {
        sentences.push_back(fill("{Ему|Ей} ", f) + ageText(years) + ", давно на пенсии.");
    }
    if (employed) {
        const Trade trade = tradeOf(job->profession, f);
        const std::string from = town.from.empty() ? std::string() : " из " + town.from;
        const std::string in = town.in.empty() ? std::string() : " " + town.in;
        std::string line;
        if (phase >= WorldPhase::Collapse) {
            line = variant(id, 1, 2) == 0
                       ? fill("Когда-то {был|была} ", f) + trade.instr + in + ". Теперь это ничего не значит."
                       : capitalize(trade.name) + from + " — в прошлой жизни, которой больше нет.";
        } else if (phase >= WorldPhase::Outbreak) {
            line = capitalize(trade.name) + from + ". Ещё неделю назад это что-то значило.";
        } else {
            line = variant(id, 1, 2) == 0 ? "Работает " + trade.instr + in + " — как вчера, как год назад."
                                          : capitalize(trade.name) + from + ". Жизнь идёт своим чередом.";
        }
        sentences.push_back(line);
    }
    // Где живёт — если не сказано вместе с работой.
    const std::string where = placed || town.in.empty() ? std::string() : " " + town.in;

    // С кем живёт: группа, семья, соседи.
    const entt::entity group = entityOf(world, person->group);
    if (group != entt::null && registry.all_of<Group>(group)) {
        if (!employed && !town.from.empty()) {
            sentences.push_back("Родом из " + town.from + ".");
        }
        const Group& info = registry.get<Group>(group);
        if (info.leader == id) {
            const int others = static_cast<int>(info.members.size()) - 1;
            sentences.push_back(others > 0 ? fill("За {ним} идут ещё ", f) + people(others) +
                                                 fill(" — и {он} за них в ответе.", f)
                                           : std::string("Собирает людей вокруг себя."));
        } else {
            sentences.push_back(variant(id, 2, 2) == 0
                                    ? fill("{Его} семья теперь — группа ", f) + fullName(world, info.leader) + "."
                                    : fill("{Прибился|Прибилась} к группе ", f) + fullName(world, info.leader) +
                                          ": вместе выжить легче.");
        }
    } else if (person->home != kInvalidId) {
        std::vector<EntityId> mates;
        for (const auto [other, them] : registry.view<const Person>().each()) {
            if (other != entity && them.home == person->home && !registry.any_of<Corpse, Zombie>(other)) {
                mates.push_back(registry.get<Id>(other).value);
            }
        }
        std::sort(mates.begin(), mates.end()); // порядок вида не детерминирован
        // Родня — «жена Mary», «сын Tom» (этап 7.3); остальные — по именам.
        std::vector<std::string> family;
        std::vector<std::string> others;
        for (const EntityId mate : mates) {
            const KinKind kind = kinOf(registry, entity, mate);
            if (kind != KinKind::Count) {
                family.push_back(std::string(kinWord(kind, female(world, mate))) + " " + firstName(world, mate));
            } else {
                others.push_back(fullName(world, mate));
            }
        }
        if (family.size() > 3) {
            family.resize(3);
            family.back() = "другие";
        }
        if (!family.empty()) {
            const std::string at = where.empty() ? "Дома" : "Дома," + where + ",";
            sentences.push_back(at + fill(" {его} ", f) + (family.size() == 1 ? "ждёт " : "ждут ") +
                                listNames(family) + ".");
        } else if (!others.empty()) {
            others.resize(std::min<std::size_t>(others.size(), 2));
            sentences.push_back("Делит крышу" + where + " с " + listNames(others) + ".");
        } else {
            sentences.push_back(variant(id, 3, 2) == 0
                                    ? fill("Живёт {один|одна}", f) + where + " — тишина в доме давно стала привычной."
                                    : fill("Живёт {один|одна}", f) + where + ", и вечерами слышно только ветер.");
        }
    }

    // Что с ним было — или каков он.
    if (const Memory* memory = notableMemory(world, entity)) {
        const std::string clause = memoryClause(world, id, *memory, fullName(world, memory->who));
        if (!clause.empty()) {
            const std::string ago = agoPhrase(memory->tick, world.tick());
            std::string line = ago + " " + clause + ".";
            const bool witnessed = memory->victim != kInvalidId && memory->victim != id &&
                                   memory->source == kInvalidId && memory->kind != MemoryKind::Killed;
            if (witnessed) {
                line = ago + fill(" у {него} на глазах ", f) + clause + ".";
            }
            if (memory->source != kInvalidId && named(world, memory->source)) {
                line = ago + fill(" {он} {узнал|узнала} от ", f) + fullName(world, memory->source) + ": " + clause +
                       ".";
            }
            const bool toSelf = memory->victim == kInvalidId || memory->victim == id;
            sentences.push_back(line + memoryCoda(memory->kind, toSelf && memory->source == kInvalidId, f));
        }
    } else if (const auto* traits = registry.try_get<Traits>(entity)) {
        // Знакомство и так длинное — тогда одна черта.
        if (std::string words = character(*traits, f, sentences.size() >= 2 ? 1 : 2); !words.empty()) {
            sentences.push_back(std::move(words));
        }
    }

    // Заражение — главное, что о нём надо знать: укус или больной рядом (этап 7.5).
    if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && isInfected(*wounds)) {
        const bool fever = isFeverish(*wounds);
        std::string line;
        if (wounds->bites > 0) {
            const std::uint64_t at = bittenAt(world, id);
            line = fill("{Укушен|Укушена}", f);
            if (at != 0) {
                line += " " + agoLower(at, world.tick());
            }
            line += fever ? " — и жар уже поднимается." : ". Пока никто не знает.";
        } else if (wounds->route == InfectionRoute::Origin) {
            line = isPatientZero(world, id)
                       ? "Болен{|а} — первым из всех, и где подхватил{|а} заразу, не знает никто"
                       : "Болен{|а} — и где подхватил{|а} заразу, не знает никто";
            line += fever ? ". Жар уже поднимается." : ", даже {он|она} сам{|а}.";
            line = fill(line, f);
        } else {
            line = fill("{Заразился|Заразилась} ", f) + agoLower(wounds->infectedAt, world.tick());
            if (named(world, wounds->infectedBy)) {
                line += " от " + firstName(world, wounds->infectedBy);
            }
            line += fever ? " — и жар уже поднимается." : " — и ещё не знает об этом.";
        }
        sentences.push_back(line);
    }

    std::string text;
    for (const std::string& sentence : sentences) {
        text += text.empty() ? sentence : " " + sentence;
    }
    return text;
}

std::string motive(const World& world, const Story* story, const Foresight* omen, EntityId target) {
    const entt::registry& registry = world.registry();
    if (omen != nullptr) {
        const EntityId hero = omen->hero;
        const bool hf = female(world, hero);
        const std::string name = firstName(world, hero);
        switch (omen->kind) {
        case ForesightKind::Revenge: {
            const std::string other = fullName(world, omen->other);
            const bool of = female(world, omen->other);
            const entt::entity self = entityOf(world, hero);
            if (const Memory* memory = self != entt::null ? grudgeAgainst(world, self, omen->other) : nullptr) {
                std::string clause;
                if (memory->kind == MemoryKind::Bereaved) {
                    clause = std::string(of ? "та" : "тот") + " " + g(of, "убил", "убила") + " " +
                             fullName(world, memory->who);
                } else {
                    clause = memoryClause(world, hero, *memory, of ? "та" : "тот");
                }
                if (variant(hero, 10, 2) == 0) {
                    return name + fill(" так и не {простил|простила} ", hf) + other + ": " + clause +
                           fill(". Сегодня {он} идёт сводить счёты.", hf);
                }
                return name + " не может простить " + other + ": " + clause + fill(". Теперь {он} идёт за ", hf) +
                       withHim(of) + ".";
            }
            return name + " идёт к " + other + ". Старые счёты пора закрыть.";
        }
        case ForesightKind::Theft: {
            const entt::entity self = entityOf(world, hero);
            const auto* person = self != entt::null ? registry.try_get<Person>(self) : nullptr;
            if (person != nullptr && person->group != kInvalidId && omen->other != kInvalidId) {
                const entt::entity group = entityOf(world, person->group);
                if (group != entt::null && registry.all_of<Group>(group) &&
                    registry.get<Group>(group).leader == omen->other) {
                    return name + " уходит из группы " + fullName(world, omen->other) +
                           " — тихо, не прощаясь, и не с пустыми руками.";
                }
            }
            const std::string house =
                named(world, omen->other) ? "дому " + fullName(world, omen->other) : std::string("чужому дому");
            if (const auto* needs = self != entt::null ? registry.try_get<Needs>(self) : nullptr;
                needs != nullptr && needs->hunger >= kPressing) {
                return "Голод сильнее совести: " + name + " идёт к " + house + ".";
            }
            if (const auto* traits = self != entt::null ? registry.try_get<Traits>(self) : nullptr;
                traits != nullptr && traits->greed >= kStrongTrait) {
                return name + fill(" давно {присматривался|присматривалась} к ", hf) + house +
                       fill(". Сегодня {он} {решился|решилась}.", hf);
            }
            return name + fill(" {решился|решилась}. Путь лежит к ", hf) + house + ".";
        }
        case ForesightKind::Raid: {
            const entt::entity village = entityOf(world, omen->other);
            const auto* settlement = village != entt::null ? registry.try_get<Settlement>(village) : nullptr;
            if (settlement == nullptr) {
                return "Орда снялась с места. Куда её понесёт — не знает никто.";
            }
            const std::string to =
                std::string(settlement->kind == SettlementKind::Town ? "городу " : "деревне ") + settlement->name;
            std::string line = "Орда бредёт к " + to + ".";
            if (omen->crowd > 0) {
                line += " Там " + people(omen->crowd) + " — и никто ещё не знает, что идёт к ним.";
            }
            return line;
        }
        case ForesightKind::Turning: {
            const entt::entity body = entityOf(world, hero);
            if (body != entt::null && registry.all_of<Corpse>(body)) {
                // Укус или болезнь (этап 7.5).
                return infectionRouteOf(world, hero) == InfectionRoute::Bite
                           ? name + fill(" {умер|умерла} от укуса. Но это ещё не конец.", hf)
                           : name + fill(" {сгорел|сгорела} в жару. Но это ещё не конец.", hf);
            }
            return infectedClause(world, hero, hf) +
                   ". Жар поднимается, а вокруг — люди, которые ничего не знают.";
        }
        case ForesightKind::CacheRace:
            if (named(world, omen->other)) {
                return "Груз упал с неба. " + name + " и " + firstName(world, omen->other) +
                       " идут к нему — и каждый надеется успеть первым.";
            }
            return "Груз упал с неба, и " + name + " спешит к нему, пока не опередили.";
        default:
            break;
        }
    }
    if (story != nullptr && story->active()) {
        const bool hf = female(world, story->hero);
        const std::string name = firstName(world, story->hero);
        switch (story->kind) {
        case StoryKind::Feud: {
            const entt::entity self = entityOf(world, story->hero);
            if (const Memory* memory = self != entt::null ? grudgeAgainst(world, self, story->other) : nullptr) {
                const std::string clause = memoryClause(world, story->hero, *memory, fullName(world, story->other));
                if (!clause.empty()) {
                    return name + " помнит: " + clause + ". Такое не прощают.";
                }
            }
            if (named(world, story->other)) {
                return "Между " + name + " и " + fullName(world, story->other) +
                       " — старая обида. Она никуда не делась.";
            }
            break;
        }
        case StoryKind::Group: {
            const entt::entity group = entityOf(world, story->focus);
            if (group != entt::null && registry.all_of<Group>(group)) {
                const Group& info = registry.get<Group>(group);
                const auto nameOf = [&](EntityId who) {
                    return who == target ? firstName(world, who) : fullName(world, who);
                };
                if (info.members.size() == 2) {
                    const EntityId other = info.members[0] == info.leader ? info.members[1] : info.members[0];
                    return nameOf(info.leader) + " и " + nameOf(other) +
                           " теперь держатся вместе. Вдвоём легче — пока.";
                }
                return nameOf(info.leader) + " держит вместе " + people(static_cast<int>(info.members.size())) +
                       ". Надолго ли хватит этого союза?";
            }
            break;
        }
        case StoryKind::Infection: {
            if (!alivePerson(world, story->hero)) {
                break;
            }
            return infectedClause(world, story->hero, hf) +
                   fill(". Сколько {ему} осталось — не знает никто.", hf);
        }
        case StoryKind::Salvage:
            return "Где-то упал груз, и " + name + " знает, где.";
        default:
            break;
        }
    }
    // Быт: что гонит человека сейчас.
    const entt::entity entity = entityOf(world, target);
    if (entity == entt::null || !alivePerson(world, target)) {
        return {};
    }
    const auto* needs = registry.try_get<Needs>(entity);
    const auto* mind = registry.try_get<Mind>(entity);
    if (needs == nullptr || mind == nullptr) {
        return {};
    }
    const bool f = female(world, target);
    const std::string name = firstName(world, target);
    switch (mind->action) {
    case Action::Forage:
    case Action::Scavenge:
    case Action::Hunt:
    case Action::Buy:
        if (needs->hunger >= kPressing) {
            return "Голод гонит " + name + " из дома: запасы кончились.";
        }
        break;
    case Action::Drink:
        if (needs->thirst >= kPressing) {
            return name + fill(" давно не {пил|пила}. Губы потрескались.", f);
        }
        break;
    case Action::Socialize:
        if (needs->loneliness >= kPressing) {
            return name + fill(" {устал|устала} от тишины пустого дома.", f);
        }
        break;
    case Action::GoHome:
    case Action::Flee:
        if (needs->fear >= kPressing) {
            return name + fill(" {напуган|напугана}. Каждая тень кажется угрозой.", f);
        }
        break;
    default:
        break;
    }
    return {};
}

std::string bridge(const World& world, EntityId from, EntityId to) {
    if (from == to || !alivePerson(world, from) || !alivePerson(world, to)) {
        return {};
    }
    const entt::registry& registry = world.registry();
    const entt::entity a = world.find(from);
    const entt::entity b = world.find(to);
    const bool af = female(world, from);
    const bool bf = female(world, to);
    const std::string aName = firstName(world, from);
    std::string relation;

    if (const Memory* memory = grudgeAgainst(world, a, to)) {
        // Прежний герой помнит, что сделал новый.
        if (memory->source != kInvalidId && named(world, memory->source) && memory->source != to &&
            memory->source != from) {
            relation = std::string("о ") + whoPrep(bf) + " " + aName + " " + g(af, "слышал", "слышала") + " от " +
                       fullName(world, memory->source);
        } else {
            relation = std::string(whoAcc(bf)) + " " + aName + " не может простить";
        }
    } else if (const Memory* good = kindnessFrom(world, a, to)) {
        switch (good->kind) {
        case MemoryKind::Saved:
            relation = std::string(whoDat(bf)) + " " + aName + " " + g(af, "обязан", "обязана") + " жизнью";
            break;
        case MemoryKind::Defended:
            relation = std::string(whoNom(bf)) + " " + g(bf, "заступился", "заступилась") + " за " + aName;
            break;
        default:
            relation = std::string(whoNom(bf)) + " " + g(bf, "поделился", "поделилась") + " с " + aName +
                       " последним";
            break;
        }
    } else if (grudgeAgainst(world, b, from) != nullptr) {
        relation = std::string("у ") + (bf ? "которой" : "которого") + " свои счёты с " + aName;
    } else {
        const auto& pa = registry.get<Person>(a);
        const auto& pb = registry.get<Person>(b);
        const auto* ra = registry.try_get<Relations>(a);
        const auto* rb = registry.try_get<Relations>(b);
        const Bond* ab = ra != nullptr ? ra->find(to) : nullptr;
        const Bond* ba = rb != nullptr ? rb->find(from) : nullptr;
        // Родня — кем приходится (этап 7.3): «сын John», даже если живут порознь.
        if (const KinKind kind = kinOf(registry, a, to); kind != KinKind::Count) {
            relation = std::string(kinWord(kind, bf)) + " " + aName;
        } else if (pa.home != kInvalidId && pa.home == pb.home && pa.group == kInvalidId) {
            relation = std::string(g(bf, "сосед ", "соседка ")) + aName + " по дому";
        } else if (ab != nullptr && (ab->origin == BondOrigin::OldFriend || ab->origin == BondOrigin::ExPartner ||
                                     ab->origin == BondOrigin::Colleague)) {
            relation = std::string(bondOriginName(ab->origin, bf)) + " " + aName;
        } else if (pa.group != kInvalidId && pa.group == pb.group) {
            relation = "из одной группы с " + aName;
        } else if ((ab != nullptr && ab->friendly()) || (ba != nullptr && ba->friendly())) {
            relation = std::string(g(bf, "друг ", "подруга ")) + aName;
        } else if ((ab != nullptr && ab->hostile()) || (ba != nullptr && ba->hostile())) {
            relation = std::string("с ") + (bf ? "которой" : "которым") + " " + aName + " не ладит";
        }
    }
    if (relation.empty()) {
        return {};
    }
    const std::string intent = intentLine(world, to);
    static constexpr const char* kOpenings[] = {"Тем временем", "А в это время", "В это же самое время"};
    std::string line = std::string(kOpenings[variant(from ^ to, 20, std::size(kOpenings))]) + " " +
                       fullName(world, to) + ", " + relation;
    if (!intent.empty()) {
        line += ", " + intent;
    }
    return line + ".";
}

std::string remark(const World& world, EntityId id, Remark& kind) {
    if (!alivePerson(world, id)) {
        return {};
    }
    const entt::registry& registry = world.registry();
    const entt::entity entity = world.find(id);
    const auto* traits = registry.try_get<Traits>(entity);
    const auto* mind = registry.try_get<Mind>(entity);
    if (traits == nullptr || mind == nullptr) {
        return {};
    }
    const bool f = female(world, id);
    const std::string name = firstName(world, id);
    const auto* needs = registry.try_get<Needs>(entity);
    const auto& person = registry.get<Person>(entity);
    switch (mind->action) {
    case Action::Avenge:
        if (traits->aggression >= kStrongTrait - 0.08f) {
            kind = Remark::Unforgiving;
            return name + " не умеет прощать.";
        }
        break;
    case Action::Flee:
        if (traits->bravery <= kWeakTrait) {
            kind = Remark::Coward;
            return std::string("Смелостью ") + name + " никогда не " + g(f, "отличался", "отличалась") + ".";
        }
        break;
    case Action::Fight:
        if (traits->bravery >= kStrongTrait) {
            kind = Remark::Fearless;
            return name + " не из тех, кто отступает.";
        }
        break;
    case Action::Share:
        if (traits->altruism >= kStrongTrait) {
            kind = Remark::Generous;
            return name + " всегда отдаст последнее.";
        }
        break;
    case Action::Steal:
    case Action::Scavenge:
        if (mind->action == Action::Steal && traits->greed >= kStrongTrait) {
            kind = Remark::Greedy;
            return name + " всегда было мало своего.";
        }
        if (traits->greed <= 0.45f && traits->altruism >= 0.5f && needs != nullptr && needs->hunger >= kPressing) {
            kind = Remark::Desperate;
            return name + " никогда не " + g(f, "брал", "брала") + " чужого. До сегодняшнего дня.";
        }
        break;
    case Action::Socialize:
        if (traits->sociability >= kStrongTrait) {
            kind = Remark::Lonely;
            return name + " не выносит одиночества.";
        }
        break;
    case Action::Wander:
    case Action::Forage:
        if (traits->curiosity >= kStrongTrait + 0.1f) { // гуляют часто — только самые непоседливые
            kind = Remark::Restless;
            return name + " — из тех, кто не сидит на месте.";
        }
        break;
    case Action::Buy:
        if (traits->prudence >= kStrongTrait) {
            kind = Remark::Hoarder;
            return name + " всегда " + g(f, "запасался", "запасалась") + " впрок.";
        }
        break;
    case Action::Trade:
        if (traits->greed >= kStrongTrait - 0.05f) {
            kind = Remark::Shrewd;
            return name + " своего не упустит.";
        }
        break;
    case Action::Work:
        if (worldPhase(world) >= WorldPhase::Panic) {
            kind = Remark::Dutiful;
            return std::string("Многие уже бросили работу. ") + name + " — нет.";
        }
        break;
    default:
        break;
    }
    // Вожак — о нём и без поступка.
    if (const entt::entity group = entityOf(world, person.group);
        group != entt::null && registry.all_of<Group>(group) && registry.get<Group>(group).leader == id &&
        traits->leadership >= kStrongTrait) {
        kind = Remark::Leader;
        return "За " + name + " идут: " + he(f) + " умеет вести за собой.";
    }
    return {};
}

// --- Голос за кадром -----------------------------------------------------------

void Narrator::reset() {
    *this = Narrator{};
}

void Narrator::say(std::string text) {
    if (text.empty()) {
        return;
    }
    // Одно и то же зритель недавно слышал — не повторять (мотив линии, связка).
    if (const auto it = spoken_.find(text); it != spoken_.end() && clock_ - it->second < kRepeatLine) {
        return;
    }
    if (std::find(queue_.begin(), queue_.end(), text) != queue_.end() || text == current_) {
        return;
    }
    queue_.push_back(std::move(text));
}

void Narrator::queueIntroduction(const World& world, EntityId id) {
    if (id == kInvalidId || introduced(id) || !named(world, id)) {
        return;
    }
    if (std::string text = introduction(world, id); !text.empty()) {
        introduced_.insert(id);
        condition_[id] = conditionOf(world, id); // о нынешнем положении сказано в знакомстве
        say(std::move(text));
    }
}

void Narrator::beginScene(const World& world, EntityId previous, EntityId subject, std::string motiveLine) {
    queue_.clear();
    current_.clear();
    age_ = length_ = 0.0f;
    gap_ = 0.0f;
    subject_ = subject;
    if (previous != kInvalidId && previous != subject) {
        say(bridge(world, previous, subject));
    }
    queueIntroduction(world, subject);
    say(std::move(motiveLine));
}

std::uint8_t Narrator::conditionOf(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity == entt::null || !alivePerson(world, id)) {
        return 0;
    }
    std::uint8_t bits = 0;
    if (const auto* wounds = world.registry().try_get<Wounds>(entity)) {
        if (isInfected(*wounds)) {
            bits |= wounds->bites > 0 ? kBitten : kInfected;
        }
        if (isFeverish(*wounds)) {
            bits |= kFever;
        }
        if (wounds->bleeding > 0.0f) {
            bits |= kBleeding;
        }
    }
    if (const auto* needs = world.registry().try_get<Needs>(entity); needs != nullptr && needs->health < 0.2f) {
        bits |= kDying;
    }
    return bits;
}

float Narrator::alpha() const {
    if (current_.empty()) {
        return 0.0f;
    }
    const float in = std::clamp(age_ / kLineFade, 0.0f, 1.0f);
    const float out = std::clamp((length_ - age_) / kLineFade, 0.0f, 1.0f);
    return std::min(in, out);
}

void Narrator::update(const World& world, EntityId subject, float dt, bool visible) {
    if (!visible) {
        return; // часы фраз стоят, пока титров не видно (перебивка, чёрный кадр)
    }
    if (subject != subject_) {
        // Камера перешла к другому в той же сцене: представить его.
        subject_ = subject;
        queueIntroduction(world, subject);
    }
    clock_ += dt;
    if (!current_.empty()) {
        age_ += dt;
        if (age_ < length_) {
            return;
        }
        current_.clear();
        gap_ = kLineGap;
    }
    if (gap_ > 0.0f) {
        gap_ -= dt;
        return;
    }
    if (queue_.empty() && subject_ != kInvalidId && introduced(subject_)) {
        // Новое в положении: укусили, началась лихорадка, кровь, при смерти.
        const std::uint8_t now = conditionOf(world, subject_);
        std::uint8_t& known = condition_[subject_];
        const std::uint8_t fresh = static_cast<std::uint8_t>(now & ~known);
        known = now;
        const bool f = female(world, subject_);
        const std::string name = firstName(world, subject_);
        if ((fresh & kFever) != 0) {
            say("У " + name + " начинается жар. Времени всё меньше.");
        } else if ((fresh & kBitten) != 0) {
            say("Зубы всё-таки достали " + name + ".");
        } else if ((fresh & kInfected) != 0) {
            say(name + fill(" {заразился|заразилась}. Пока об этом не знает никто — даже {он|она} сам{|а}.", f));
        }
        if ((fresh & kDying) != 0) {
            say(name + " при смерти. Каждый вдох даётся с трудом.");
        } else if ((fresh & kBleeding) != 0) {
            say(name + fill(" {ранен|ранена} — кровь не останавливается.", f));
        }
        // Поступок выдаёт характер.
        Remark kind = Remark::Count;
        if (std::string text = remark(world, subject_, kind); !text.empty()) {
            const std::uint64_t key = (static_cast<std::uint64_t>(subject_) << 8) | static_cast<std::uint64_t>(kind);
            const auto it = remarked_.find(key);
            if (it == remarked_.end() || clock_ - it->second >= kRemarkRepeat) {
                remarked_[key] = clock_;
                say(std::move(text));
            }
        }
    }
    if (queue_.empty()) {
        return;
    }
    current_ = std::move(queue_.front());
    queue_.pop_front();
    spoken_[current_] = clock_;
    age_ = 0.0f;
    length_ = std::clamp(kLineBase + static_cast<float>(glyphs(current_)) / kCharsPerSecond, kLineMin, kLineMax);
}

} // namespace lw::ui
