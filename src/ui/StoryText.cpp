#include "ui/StoryText.h"

#include "sim/Components.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/Format.h"

#include <cstdio>
#include <iterator>

namespace lw::ui {

const char* heroStatusTitle(HeroStatus status) {
    constexpr const char* kNames[] = {"в сериале", "погиб", "обратился", "ушёл со сцены"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(HeroStatus::Count));
    const auto index = static_cast<std::size_t>(status);
    return index < std::size(kNames) ? kNames[index] : "?";
}

const char* omenTitle(ForesightKind kind) {
    constexpr const char* kNames[] = {"Месть", "Орда", "Обращение", "Гонка за грузом", "Кража"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ForesightKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "?";
}

const char* kindTitle(const Story& story) {
    switch (story.kind) {
    case StoryKind::Feud:
        return story.betrayal ? "Предательство" : "Вражда";
    case StoryKind::Group:
        return "Группа";
    case StoryKind::Infection:
        return "Заражение";
    case StoryKind::Salvage:
        return "Гонка за грузом";
    default:
        return "?";
    }
}

const char* stageTitle(StoryStage stage) {
    constexpr const char* kNames[] = {"завязка", "нарастание", "кульминация", "развязка"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(StoryStage::Count));
    const auto index = static_cast<std::size_t>(stage);
    return index < std::size(kNames) ? kNames[index] : "?";
}

const char* endingTitle(StoryEnding ending) {
    constexpr const char* kNames[] = {"",
                                      "месть",
                                      "примирение",
                                      "обидчик убил",
                                      "гибель",
                                      "забылось",
                                      "обращение",
                                      "группа распалась",
                                      "груз забрали",
                                      "груз никому не достался"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(StoryEnding::Count));
    const auto index = static_cast<std::size_t>(ending);
    return index < std::size(kNames) ? kNames[index] : "?";
}

std::string stateLine(const Story& story) {
    std::string line = stageTitle(story.stage);
    if (story.active()) {
        line += " — чем кончится: ";
        line += endingTitle(expectedEnding(story));
        line += "?";
    } else {
        line += ": ";
        line += endingTitle(story.ending);
    }
    return line;
}

std::string titleOf(const Story& story) {
    std::string title = kindTitle(story);
    title += ": ";
    if (story.hero != kInvalidId) {
        title += idLabel(story.hero);
    } else if (story.focus != kInvalidId) {
        title += idLabel(story.focus);
    }
    if (story.kind == StoryKind::Feud && story.other != kInvalidId) {
        title += " — ";
        title += idLabel(story.other);
    }
    return title;
}

std::string omenLine(const World& world, const Foresight& omen) {
    const entt::registry& registry = world.registry();
    std::string line;
    char text[160];
    switch (omen.kind) {
    case ForesightKind::Revenge:
        line = idLabel(omen.hero) + " идёт мстить " + idLabel(omen.other);
        break;
    case ForesightKind::Raid: {
        const entt::entity horde = world.find(omen.focus);
        const entt::entity place = world.find(omen.other);
        const std::size_t size =
            horde != entt::null ? registry.get<Horde>(horde).members.size() : std::size_t{0};
        std::snprintf(text, sizeof(text), "Орда (%zu) идёт к поселению %s, там людей: %d", size,
                      place != entt::null ? registry.get<Settlement>(place).name.c_str() : "?",
                      omen.crowd);
        line = text;
        break;
    }
    case ForesightKind::Turning: {
        const entt::entity body = world.find(omen.hero);
        const bool dead = body != entt::null && registry.all_of<Corpse>(body);
        std::snprintf(text, sizeof(text),
                      dead ? " вот-вот встанет зомби, рядом людей: %d"
                           : " заражён, рядом людей: %d — обратится",
                      omen.crowd);
        line = (dead ? "Тело " : "") + idLabel(omen.hero) + text;
        break;
    }
    case ForesightKind::CacheRace:
        std::snprintf(text, sizeof(text), " спешат к грузу (всего %d)", omen.crowd);
        line = idLabel(omen.hero) + " и " + idLabel(omen.other) + text;
        break;
    case ForesightKind::Theft: {
        const entt::entity thief = world.find(omen.hero);
        const auto* person = thief != entt::null ? registry.try_get<Person>(thief) : nullptr;
        if (person != nullptr && person->home == omen.focus) {
            line = idLabel(omen.hero) + " хочет сбежать с запасами группы";
        } else {
            line = idLabel(omen.hero) + " идёт обокрасть дом " +
                   idLabel(omen.other != kInvalidId ? omen.other : omen.focus);
        }
        break;
    }
    default:
        break;
    }
    if (omen.distance >= 1.0f) {
        std::snprintf(text, sizeof(text), " — %.0f м", omen.distance);
        line += text;
    }
    const auto ticks = static_cast<std::uint64_t>(omen.eta * static_cast<float>(kTicksPerSecond));
    if (ticks < kTicksPerGameMinute) {
        line += ", сейчас";
    } else {
        char duration[32];
        formatGameDuration(duration, sizeof(duration), ticks);
        line += ", через ~";
        line += duration;
    }
    return line;
}

} // namespace lw::ui
