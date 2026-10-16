#include "ui/Format.h"

#include "sim/TimeScale.h"

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
        return "сарай";
    case BuildingKind::Apartment:
        return "многоэтажка";
    case BuildingKind::Shop:
        return "магазин";
    }
    return "здание";
}

const char* settlementKindName(SettlementKind kind) {
    return kind == SettlementKind::Town ? "город" : "деревня";
}

const char* actionName(Action action) {
    constexpr const char* kNames[] = {"отдыхает", "спит",      "ест",    "пьёт",
                                      "ищет припасы", "собирает в лесу", "общается",
                                      "гуляет",   "уходит домой", "убегает", "дерётся",
                                      "охотится", "перевязывается", "делится", "меняется",
                                      "крадёт",   "мстит"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(Action::Count));
    const auto index = static_cast<std::size_t>(action);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* soundName(SoundKind kind) {
    constexpr const char* kNames[] = {"тишина", "шаги", "обыск", "голос", "стон", "птицы", "выстрел", "крик"};
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

bool idLink(EntityId id) {
    char label[32];
    std::snprintf(label, sizeof(label), "#%llu", static_cast<unsigned long long>(id));
    ImGui::PushID(static_cast<int>(id));
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopID();
    return clicked;
}

void formatGameDuration(char* buffer, std::size_t size, std::uint64_t ticks) {
    const std::uint64_t minutes = ticks / kTicksPerGameMinute;
    if (minutes >= 60) {
        std::snprintf(buffer, size, "%llu ч %02llu мин",
                      static_cast<unsigned long long>(minutes / 60),
                      static_cast<unsigned long long>(minutes % 60));
    } else {
        std::snprintf(buffer, size, "%llu мин", static_cast<unsigned long long>(minutes));
    }
}

} // namespace lw::ui
