#include "ui/Format.h"

#include "sim/Names.h"
#include "sim/Scenario.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>

namespace lw::ui {

const char* buildingName(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return "дом";
    case BuildingKind::Barn:
        return "ферма";
    case BuildingKind::Apartment:
        return "многоэтажка";
    case BuildingKind::Shop:
        return "магазин";
    case BuildingKind::Hospital:
        return "больница";
    case BuildingKind::PoliceStation:
        return "полиция";
    case BuildingKind::School:
        return "школа";
    }
    return "здание";
}

const char* professionName(Profession profession) {
    constexpr const char* kNames[] = {"без работы", "продавец", "фермер", "врач", "полицейский",
                                      "учитель", "школьник"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(Profession::Count));
    const auto index = static_cast<std::size_t>(profession);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* kinWord(KinKind kind, bool female, Case grammaticalCase) {
    // По KinKind: мужской и женский род в четырёх падежах.
    static constexpr const char* kWords[][2][4] = {
        {{"муж", "мужа", "мужа", "мужем"}, {"жена", "жены", "жену", "женой"}},
        {{"отец", "отца", "отца", "отцом"}, {"мать", "матери", "мать", "матерью"}},
        {{"сын", "сына", "сына", "сыном"}, {"дочь", "дочери", "дочь", "дочерью"}},
        {{"брат", "брата", "брата", "братом"}, {"сестра", "сестры", "сестру", "сестрой"}},
        {{"дед", "деда", "деда", "дедом"}, {"бабушка", "бабушки", "бабушку", "бабушкой"}},
        {{"внук", "внука", "внука", "внуком"}, {"внучка", "внучки", "внучку", "внучкой"}},
    };
    static_assert(std::size(kWords) == static_cast<std::size_t>(KinKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    if (index >= std::size(kWords)) {
        return "родственник";
    }
    return kWords[index][female ? 1 : 0][static_cast<std::size_t>(grammaticalCase)];
}

const char* bondOriginName(BondOrigin origin, bool female) {
    switch (origin) {
    case BondOrigin::Housemate:
        return female ? "соседка по дому" : "сосед по дому";
    case BondOrigin::Neighbour:
        return female ? "соседка" : "сосед";
    case BondOrigin::Kin:
        return "родня";
    case BondOrigin::Colleague:
        return "коллега";
    case BondOrigin::Classmate:
        return "по школе";
    case BondOrigin::OldFriend:
        return female ? "давняя подруга" : "давний друг";
    case BondOrigin::OldFeud:
        return "давняя вражда";
    case BondOrigin::ExPartner:
        return female ? "бывшая жена" : "бывший муж";
    default:
        return "";
    }
}

std::string ageText(int years) {
    const int last = years % 10;
    const int lastTwo = years % 100;
    const char* word = lastTwo >= 11 && lastTwo <= 14 ? "лет" : last == 1 ? "год" : last >= 2 && last <= 4 ? "года" : "лет";
    return std::to_string(years) + " " + word;
}

const char* settlementKindName(SettlementKind kind) {
    return kind == SettlementKind::Town ? "город" : "деревня";
}

const char* actionName(Action action) {
    constexpr const char* kNames[] = {"отдыхает", "спит",      "ест",    "пьёт",
                                      "ищет припасы", "собирает в лесу", "общается",
                                      "гуляет",   "уходит домой", "убегает", "дерётся",
                                      "охотится", "перевязывается", "делится", "меняется",
                                      "крадёт",   "мстит",    "идёт к месту ивента",
                                      "на работе", "за покупками", "у врача"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(Action::Count));
    const auto index = static_cast<std::size_t>(action);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* soundName(SoundKind kind) {
    constexpr const char* kNames[] = {"тишина", "шаги", "обыск", "голос", "стон", "птицы", "выстрел", "крик",
                                      "гул самолёта"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(SoundKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* zombieStateName(ZombieState state) {
    constexpr const char* kNames[] = {"стоит", "бредёт", "идёт на шум", "преследует",
                                      "идёт с ордой"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ZombieState::Count));
    const auto index = static_cast<std::size_t>(state);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* animalKindName(AnimalKind kind) {
    constexpr const char* kNames[] = {"олень", "кабан", "заяц"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(AnimalKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* animalStateName(AnimalState state) {
    constexpr const char* kNames[] = {"пасётся", "переходит", "убегает", "бросается"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(AnimalState::Count));
    const auto index = static_cast<std::size_t>(state);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* weaponName(WeaponKind kind) {
    constexpr const char* kNames[] = {"кулаки", "нож", "топор", "пистолет", "дробовик", "винтовка"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(WeaponKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* deathCauseName(DeathCause cause) {
    constexpr const char* kNames[] = {"убит в схватке", "истёк кровью",   "умер от голода",
                                      "умер от жажды",  "умер от изнеможения", "сгорел от заражения"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(DeathCause::Count));
    const auto index = static_cast<std::size_t>(cause);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* infectionRouteText(InfectionRoute route) {
    constexpr const char* kNames[] = {"не заражён",     "неизвестно где", "укус зомби",
                                      "под одной крышей", "в больнице",   "лицом к лицу",
                                      "рядом",           "у тела"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(InfectionRoute::Count));
    const auto index = static_cast<std::size_t>(route);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* corpseKindName(CorpseKind kind) {
    constexpr const char* kNames[] = {"человек", "зомби", "животное"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(CorpseKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* memoryKindName(MemoryKind kind) {
    constexpr const char* kNames[] = {"спас от зомби",   "заступился",    "бросил в беде",
                                      "обокрал дом",     "погиб",         "встал зомби",
                                      "изгнал из группы", "помог",  "предал",
                                      "напал",        "убил"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(MemoryKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* eventKindName(EventKind kind) {
    constexpr const char* kNames[] = {"падение припасов"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(EventKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* scenarioKindName(ScenarioKind kind) {
    constexpr const char* kNames[] = {"зомби-вирус"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ScenarioKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* phaseName(WorldPhase phase) {
    constexpr const char* kNames[] = {"мир", "вспышка", "паника", "коллапс", "выживание"};
    static_assert(std::size(kNames) == kWorldPhases);
    const auto index = static_cast<std::size_t>(phase);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* itemName(ItemKind kind) {
    constexpr const char* kNames[] = {"еда",      "вода",       "мясо",     "бинты",
                                      "лекарства", "патроны к пистолету", "патроны к дробовику",
                                      "патроны к винтовке", "нож", "топор", "пистолет",
                                      "дробовик", "винтовка"};
    static_assert(std::size(kNames) == kItemKinds);
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "—";
}

void formatInventory(char* buffer, std::size_t size, const Inventory& inventory) {
    std::size_t used = 0;
    buffer[0] = '\0';
    for (std::size_t i = 0; i < kItemKinds && used < size; ++i) {
        if (inventory.items[i] == 0) {
            continue;
        }
        const int written = std::snprintf(buffer + used, size - used, "%s%s %d",
                                          used > 0 ? ", " : "", itemName(static_cast<ItemKind>(i)),
                                          inventory.items[i]);
        if (written < 0) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    if (used == 0) {
        std::snprintf(buffer, size, "пусто");
    }
}

void meter(const char* label, float value) {
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%s %.0f%%", label, value * 100.0f);
    ImGui::ProgressBar(std::clamp(value, 0.0f, 1.0f), {-FLT_MIN, 0.0f}, overlay);
}

namespace {
const World* nameSource = nullptr;
} // namespace

void setNameSource(const World* world) {
    nameSource = world;
}

std::string idLabel(EntityId id) {
    if (nameSource == nullptr) {
        return "#" + std::to_string(id);
    }
    return labelOf(nameSource->registry(), nameSource->find(id), id);
}

bool idLink(EntityId id) {
    const std::string label = idLabel(id);
    ImGui::PushID(static_cast<int>(id));
    const bool clicked = ImGui::SmallButton(label.c_str());
    ImGui::PopID();
    return clicked;
}

void formatGameDuration(char* buffer, std::size_t size, std::uint64_t ticks) {
    const std::uint64_t minutes = ticks / kTicksPerGameMinute;
    if (minutes >= 24 * 60) {
        std::snprintf(buffer, size, "%llu сут %llu ч",
                      static_cast<unsigned long long>(minutes / (24 * 60)),
                      static_cast<unsigned long long>(minutes / 60 % 24));
    } else if (minutes >= 60) {
        std::snprintf(buffer, size, "%llu ч %02llu мин",
                      static_cast<unsigned long long>(minutes / 60),
                      static_cast<unsigned long long>(minutes % 60));
    } else {
        std::snprintf(buffer, size, "%llu мин", static_cast<unsigned long long>(minutes));
    }
}

void formatNextPhase(char* buffer, std::size_t size, const World& world, const Scenario& scenario) {
    buffer[0] = '\0';
    if (scenario.phase == WorldPhase::Survival) {
        return;
    }
    const WorldPhase next = nextPhase(scenario.phase);
    const std::uint64_t tick = world.tick();
    const auto until = [tick](std::uint64_t at, char* text, std::size_t length) {
        char duration[32];
        formatGameDuration(duration, sizeof(duration), at > tick ? at - tick : 0);
        std::snprintf(text, length, "через %s", duration);
    };
    char deadline[96] = "не по расписанию";
    if (const std::uint64_t at = phaseDeadline(scenario); at != Scenario::kNever) {
        until(at, deadline, sizeof(deadline));
    }
    const int trigger = phaseTrigger(scenario);
    if (trigger == 0) {
        std::snprintf(buffer, size, "%s — %s", phaseName(next), deadline);
        return;
    }
    char earliest[128] = "";
    if (const std::uint64_t at = phaseEarliest(scenario); at > tick) {
        char text[96];
        until(at, text, sizeof(text));
        std::snprintf(earliest, sizeof(earliest), ", но не раньше чем %s", text);
    }
    std::snprintf(buffer, size, "%s — при %d пострадавших (сейчас %d)%s, иначе %s", phaseName(next),
                  trigger, scenarioStats(world).casualties(), earliest, deadline);
}

} // namespace lw::ui
