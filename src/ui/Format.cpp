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
                                      "гуляет",   "уходит домой", "убегает"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(Action::Count));
    const auto index = static_cast<std::size_t>(action);
    return index < std::size(kNames) ? kNames[index] : "—";
}

const char* soundName(SoundKind kind) {
    constexpr const char* kNames[] = {"тишина", "шаги", "обыск", "голос", "стон"};
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
