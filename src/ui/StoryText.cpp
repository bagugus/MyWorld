#include "ui/StoryText.h"

#include "sim/Components.h"
#include "sim/Infection.h"
#include "sim/Scenario.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/Captions.h"
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

namespace {

// Окончание по роду: «погиб» — «погибла».
std::string said(const World& world, EntityId id, const char* male, const char* female) {
    return isFemale(world, id) ? female : male;
}

// Жив ли человек (не тело, не зомби, есть в мире).
bool alive(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    return entity != entt::null && world.registry().all_of<Person>(entity);
}

std::string settlementName(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    if (entity != entt::null) {
        if (const auto* settlement = world.registry().try_get<Settlement>(entity)) {
            return settlement->name;
        }
    }
    return "поселению";
}

} // namespace

std::string storyHeadline(const World& world, const Story& story) {
    const EntityId hero = story.hero != kInvalidId ? story.hero : story.focus;
    const std::string a = idLabel(hero);
    const std::string b = idLabel(story.other);
    switch (story.kind) {
    case StoryKind::Feud:
        switch (story.ending) {
        case StoryEnding::Revenge:
            return a + said(world, hero, " отомстил ", " отомстила ") + b;
        case StoryEnding::Reconciled:
            return a + " и " + b + " помирились";
        case StoryEnding::Killed:
            return b + said(world, story.other, " убил ", " убила ") + a;
        case StoryEnding::Death:
            if (!alive(world, hero)) {
                return a + said(world, hero, " погиб, так и не простив ", " погибла, так и не простив ") + b;
            }
            return b + said(world, story.other, " погиб, так и не помирившись с ", " погибла, так и не помирившись с ") + a;
        case StoryEnding::Faded:
            return a + said(world, hero, " забыл обиду на ", " забыла обиду на ") + b;
        default:
            return story.betrayal ? b + said(world, story.other, " предал ", " предала ") + a
                                  : a + said(world, hero, " не простил ", " не простила ") + b;
        }
    case StoryKind::Group: {
        // Вожак — нынешний, если группа ещё есть.
        EntityId leader = story.hero;
        const entt::entity group = story.focus != kInvalidId ? world.find(story.focus) : entt::null;
        if (group != entt::null) {
            if (const auto* g = world.registry().try_get<Group>(group); g != nullptr && g->leader != kInvalidId) {
                leader = g->leader;
            }
        }
        if (story.ending == StoryEnding::Disbanded) {
            return "Люди " + idLabel(leader) + " разошлись";
        }
        return idLabel(leader) + said(world, leader, " и его люди", " и её люди");
    }
    case StoryKind::Infection: {
        // Укус или больной рядом (этап 7.5).
        const InfectionRoute route = infectionRouteOf(world, hero);
        const bool bitten = route == InfectionRoute::Bite;
        switch (story.ending) {
        case StoryEnding::Turned:
            return a + said(world, hero, " стал зомби", " стала зомби");
        case StoryEnding::Death:
            return bitten ? a + said(world, hero, " умер от укуса", " умерла от укуса")
                          : a + said(world, hero, " умер от заразы", " умерла от заразы");
        default:
            if (bitten) {
                return a + said(world, hero, " укушен", " укушена");
            }
            if (route == InfectionRoute::Origin) {
                return isPatientZero(world, hero)
                           ? a + said(world, hero, " заболел первым", " заболела первой")
                           : a + said(world, hero, " заразился неизвестно где", " заразилась неизвестно где");
            }
            if (story.other != kInvalidId) {
                return a + said(world, hero, " заразился от ", " заразилась от ") + b;
            }
            return a + said(world, hero, " заражён", " заражена");
        }
    }
    case StoryKind::Salvage:
        switch (story.ending) {
        case StoryEnding::Claimed:
            return "Груз забрали";
        case StoryEnding::Unclaimed:
            return "Груз так никому и не достался";
        default:
            return hero != kInvalidId && alive(world, hero) ? a + " и другие спешат к упавшему грузу"
                                                             : "Все спешат к упавшему грузу";
        }
    default:
        return a;
    }
}

std::string omenHeadline(const World& world, const Foresight& omen) {
    const entt::registry& registry = world.registry();
    std::string line;
    switch (omen.kind) {
    case ForesightKind::Revenge:
        line = idLabel(omen.hero) + " идёт мстить " + idLabel(omen.other);
        break;
    case ForesightKind::Raid:
        line = "Орда идёт к " + settlementName(world, omen.other);
        break;
    case ForesightKind::Turning: {
        const entt::entity body = world.find(omen.hero);
        if (body != entt::null && registry.all_of<Corpse>(body)) {
            line = "Тело " + idLabel(omen.hero) + " вот-вот встанет";
        } else {
            line = idLabel(omen.hero) + " вот-вот обратится";
        }
        if (omen.crowd > 0) {
            line += ", а рядом люди";
        }
        return line;
    }
    case ForesightKind::CacheRace:
        line = idLabel(omen.hero) + " и " + idLabel(omen.other) + " спешат к одному грузу";
        break;
    case ForesightKind::Theft: {
        const entt::entity thief = world.find(omen.hero);
        const auto* person = thief != entt::null ? registry.try_get<Person>(thief) : nullptr;
        if (person != nullptr && person->home == omen.focus) {
            line = idLabel(omen.hero) + " хочет сбежать с запасами группы";
        } else if (omen.other != kInvalidId) {
            line = idLabel(omen.hero) + " идёт обокрасть дом " + idLabel(omen.other);
        } else {
            line = idLabel(omen.hero) + " идёт обокрасть чужой дом";
        }
        break;
    }
    default:
        return {};
    }
    // Когда — словами, не минутами.
    const auto ticks = static_cast<std::uint64_t>(omen.eta * static_cast<float>(kTicksPerSecond));
    if (ticks < 2 * kTicksPerGameMinute) {
        line += omen.kind == ForesightKind::Raid ? " — она уже у околицы" : " — уже совсем близко";
    } else if (ticks < 30 * kTicksPerGameMinute) {
        line += " — скоро будет там";
    }
    return line;
}

} // namespace lw::ui
