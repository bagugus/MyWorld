#include "ui/Narrator.h"

#include "sim/Calendar.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/Scenario.h"
#include "sim/Story.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/Captions.h"
#include "ui/Format.h"

#include <algorithm>
#include <cmath>
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
constexpr float kLineBase = 1.6f;
constexpr float kCharsPerSecond = 15.0f;
constexpr float kLineMin = 4.0f;
constexpr float kLineMax = 10.0f;
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

std::string lastName(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity != entt::null) {
        if (const auto* name = world.registry().try_get<Name>(entity)) {
            return name->last;
        }
    }
    return {};
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

// Характер двумя словами: «смелый и нелюдимый»; пусто — ничего яркого.
std::string character(const Traits& traits, bool f) {
    struct Word {
        float value;
        const char* high[2];
        const char* low[2];
    };
    const Word words[] = {
        {traits.bravery, {"смелый", "смелая"}, {"робкий", "робкая"}},
        {traits.altruism, {"добрый", "добрая"}, {"чёрствый", "чёрствая"}},
        {traits.greed, {"жадный", "жадная"}, {"бескорыстный", "бескорыстная"}},
        {traits.aggression, {"вспыльчивый", "вспыльчивая"}, {"кроткий", "кроткая"}},
        {traits.sociability, {"общительный", "общительная"}, {"нелюдимый", "нелюдимая"}},
        {traits.curiosity, {"любопытный", "любопытная"}, {"нелюбопытный", "нелюбопытная"}},
        {traits.prudence, {"запасливый", "запасливая"}, {"беспечный", "беспечная"}},
        {traits.leadership, {"властный", "властная"}, {"покладистый", "покладистая"}},
    };
    // Два самых ярких, по порядку таблицы при равенстве.
    std::vector<std::pair<float, const char*>> picked;
    for (const Word& word : words) {
        const float shift = word.value - 0.5f;
        if (std::abs(shift) >= kMarked) {
            picked.emplace_back(std::abs(shift), shift > 0.0f ? word.high[f ? 1 : 0] : word.low[f ? 1 : 0]);
        }
    }
    std::stable_sort(picked.begin(), picked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (picked.empty()) {
        return {};
    }
    std::string line = picked[0].second;
    if (picked.size() > 1) {
        line += std::string(" и ") + picked[1].second;
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
        return std::string(g(f, "Его", "Её")) + " мы уже не застали " + g(f, "живым", "живой") + ".";
    }
    if (registry.all_of<Zombie>(entity)) {
        return std::string("Когда-то ") + g(f, "его", "её") + " звали так. Теперь это зомби.";
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
    const bool collapsed = worldPhase(world) >= WorldPhase::Collapse;
    const bool employed = job != nullptr && job->profession != Profession::None;
    if (employed) {
        std::string trade = professionName(job->profession);
        if (f && job->profession == Profession::Shopkeeper) {
            trade = "продавщица";
        } else if (f && job->profession == Profession::Teacher) {
            trade = "учительница";
        }
        std::string line = collapsed ? "В прошлом " + trade : capitalize(trade);
        if (!town.from.empty()) {
            line += " из " + town.from;
        }
        sentences.push_back(line + ".");
    }
    // Где живёт — если не сказано вместе с работой.
    const std::string where = employed || town.in.empty() ? std::string() : " " + town.in;

    // С кем живёт: группа, семья, соседи.
    const entt::entity group = entityOf(world, person->group);
    if (group != entt::null && registry.all_of<Group>(group)) {
        if (!employed && !town.from.empty()) {
            sentences.push_back("Родом из " + town.from + ".");
        }
        const Group& info = registry.get<Group>(group);
        if (info.leader == id) {
            sentences.push_back("Ведёт группу из " + std::to_string(info.members.size()) + " человек.");
        } else {
            sentences.push_back("Держится группы " + fullName(world, info.leader) + ".");
        }
    } else if (person->home != kInvalidId) {
        std::vector<EntityId> mates;
        for (const auto [other, them] : registry.view<const Person>().each()) {
            if (other != entity && them.home == person->home && !registry.any_of<Corpse, Zombie>(other)) {
                mates.push_back(registry.get<Id>(other).value);
            }
        }
        std::sort(mates.begin(), mates.end()); // порядок вида не детерминирован
        const std::string surname = lastName(world, id);
        std::vector<std::string> family;
        std::vector<std::string> others;
        for (const EntityId mate : mates) {
            if (lastName(world, mate) == surname) {
                family.push_back(firstName(world, mate));
            } else {
                others.push_back(fullName(world, mate));
            }
        }
        if (family.size() > 3) {
            family.resize(3);
            family.back() = "другие";
        }
        if (family.size() == 1) {
            sentences.push_back("Живёт" + where + " вдвоём с " + family.front() + ".");
        } else if (!family.empty()) {
            sentences.push_back("Живёт" + where + " с семьёй: " + listNames(family) + ".");
        } else if (!others.empty()) {
            others.resize(std::min<std::size_t>(others.size(), 2));
            sentences.push_back("Живёт" + where + " под одной крышей с " + listNames(others) + ".");
        } else {
            sentences.push_back(std::string(g(f, "Живёт один", "Живёт одна")) + where + ".");
        }
    }

    // Что с ним было — или каков он.
    if (const Memory* memory = notableMemory(world, entity)) {
        const std::string clause = memoryClause(world, id, *memory, fullName(world, memory->who));
        if (!clause.empty()) {
            std::string line = agoPhrase(memory->tick, world.tick()) + " " + clause;
            const bool witnessed = memory->victim != kInvalidId && memory->victim != id &&
                                   memory->source == kInvalidId && memory->kind != MemoryKind::Killed;
            if (witnessed) {
                line = agoPhrase(memory->tick, world.tick()) + " у " + (f ? "неё" : "него") + " на глазах " + clause;
            }
            if (memory->source != kInvalidId && named(world, memory->source)) {
                line = agoPhrase(memory->tick, world.tick()) + " " + he(f) + " " + g(f, "узнал", "узнала") +
                       " от " + fullName(world, memory->source) + ": " + clause;
            }
            sentences.push_back(line + ".");
        }
    } else if (const auto* traits = registry.try_get<Traits>(entity)) {
        if (const std::string words = character(*traits, f); !words.empty()) {
            sentences.push_back(std::string(g(f, "Человек ", "Женщина ")) + words + ".");
        }
    }

    // Укус — главное, что о нём надо знать.
    if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && wounds->infection > 0.0f) {
        const std::uint64_t at = bittenAt(world, id);
        std::string line = std::string(g(f, "Укушен", "Укушена"));
        if (at != 0) {
            line += " " + agoLower(at, world.tick());
        }
        sentences.push_back(line + (wounds->infection >= 0.5f ? " — и уже в лихорадке." : "."));
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
                return name + " не может простить " + other + ": " + clause + ". Теперь " + he(hf) + " идёт за " +
                       withHim(of) + ".";
            }
            return name + " идёт свести счёты с " + other + ".";
        }
        case ForesightKind::Theft: {
            const entt::entity self = entityOf(world, hero);
            const auto* person = self != entt::null ? registry.try_get<Person>(self) : nullptr;
            if (person != nullptr && person->group != kInvalidId && omen->other != kInvalidId) {
                const entt::entity group = entityOf(world, person->group);
                if (group != entt::null && registry.all_of<Group>(group) &&
                    registry.get<Group>(group).leader == omen->other) {
                    return name + " " + g(hf, "решил", "решила") + " уйти из группы " + fullName(world, omen->other) +
                           " — и не с пустыми руками.";
                }
            }
            std::string why;
            if (const auto* needs = self != entt::null ? registry.try_get<Needs>(self) : nullptr;
                needs != nullptr && needs->hunger >= kPressing) {
                why = name + " " + g(hf, "голоден", "голодна") + ", а своего ничего не осталось";
            } else if (const auto* traits = self != entt::null ? registry.try_get<Traits>(self) : nullptr;
                       traits != nullptr && traits->greed >= kStrongTrait) {
                why = name + " давно " + g(hf, "присматривался", "присматривалась") + " к чужому добру";
            } else {
                why = name + " " + g(hf, "решился", "решилась");
            }
            if (named(world, omen->other)) {
                return why + ". Дом " + fullName(world, omen->other) + " — совсем рядом.";
            }
            return why + ".";
        }
        case ForesightKind::Raid: {
            const entt::entity village = entityOf(world, omen->other);
            const auto* settlement = village != entt::null ? registry.try_get<Settlement>(village) : nullptr;
            if (settlement == nullptr) {
                return "Орда снялась с места.";
            }
            std::string line = "Орда бредёт к " + settlement->name + ".";
            if (omen->crowd > 0) {
                line += " Там " + people(omen->crowd) + ", и никто ещё не знает.";
            }
            return line;
        }
        case ForesightKind::Turning: {
            const entt::entity body = entityOf(world, hero);
            if (body != entt::null && registry.all_of<Corpse>(body)) {
                return name + " " + g(hf, "умер", "умерла") + " от укуса. Скоро тело встанет.";
            }
            const std::uint64_t at = bittenAt(world, hero);
            std::string line = name + " укусили";
            if (at != 0) {
                line += " " + agoLower(at, world.tick());
            }
            return line + ". Лихорадка уже началась, а рядом люди.";
        }
        case ForesightKind::CacheRace:
            if (named(world, omen->other)) {
                return "С неба упал груз. " + name + " и " + firstName(world, omen->other) +
                       " идут к нему — успеет кто-то один.";
            }
            return "С неба упал груз, и " + name + " спешит к нему.";
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
                    return name + " не " + g(hf, "забыл", "забыла") + ": " + clause + ".";
                }
            }
            if (named(world, story->other)) {
                return "Между " + name + " и " + fullName(world, story->other) + " — старая обида.";
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
                    return nameOf(info.leader) + " и " + nameOf(other) + " теперь держатся вместе. Надолго ли?";
                }
                return nameOf(info.leader) + " держит вместе " + people(static_cast<int>(info.members.size())) +
                       ". Надолго ли?";
            }
            break;
        }
        case StoryKind::Infection: {
            if (!alivePerson(world, story->hero)) {
                break;
            }
            const std::uint64_t at = bittenAt(world, story->hero);
            std::string line = name + " укусили";
            if (at != 0) {
                line += " " + agoLower(at, world.tick());
            }
            return line + ". Сколько ещё " + toHim(hf) + " осталось?";
        }
        case StoryKind::Salvage:
            return "Где-то упал груз. Об этом знает " + name + ".";
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
            return name + " " + g(f, "голоден", "голодна") + ", а дома почти ничего не осталось.";
        }
        break;
    case Action::Drink:
        if (needs->thirst >= kPressing) {
            return name + " давно не " + g(f, "пил", "пила") + ".";
        }
        break;
    case Action::Socialize:
        if (needs->loneliness >= kPressing) {
            return name + " " + g(f, "устал", "устала") + " быть " + g(f, "один", "одна") + ".";
        }
        break;
    case Action::GoHome:
    case Action::Flee:
        if (needs->fear >= kPressing) {
            return name + " " + g(f, "напуган", "напугана") + ".";
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
        if (pa.home != kInvalidId && pa.home == pb.home && pa.group == kInvalidId) {
            relation = lastName(world, from) == lastName(world, to)
                           ? "из одной семьи с " + aName
                           : std::string(g(bf, "сосед ", "соседка ")) + aName + " по дому";
        } else if (lastName(world, from) == lastName(world, to)) {
            // Одна фамилия — одна семья (жильцы дома при рождении мира), даже если
            // теперь живут порознь.
            relation = "из одной семьи с " + aName;
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
    std::string line = "Тем временем " + fullName(world, to) + ", " + relation;
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
        if (wounds->infection > 0.0f) {
            bits |= kBitten;
        }
        if (wounds->infection >= 0.5f) {
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
            say("У " + name + " начинается лихорадка.");
        } else if ((fresh & kBitten) != 0) {
            say(name + " укусили.");
        }
        if ((fresh & kDying) != 0) {
            say(name + " при смерти.");
        } else if ((fresh & kBleeding) != 0) {
            say(name + " " + g(f, "ранен", "ранена") + " и истекает кровью.");
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
