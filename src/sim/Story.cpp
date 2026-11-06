#include "sim/Story.h"

#include "sim/Components.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace lw {

namespace {

// Закрытая линия ещё столько принимает события — эпилог: похороны, «узнал в
// зомби», запоздалая месть.
constexpr std::uint64_t kEpilogue = 6 * kTicksPerGameHour;
// Вражда без событий забывается (обида угасает за ~30 суток, но для зрителя
// линия кончается раньше).
constexpr std::uint64_t kFeudFades = 10 * kTicksPerGameDay;
// Укушенный умер, а тело так и не встало.
constexpr std::uint64_t kBodyStays = kTicksPerGameDay;
// Заражение без событий (до обращения — не больше 30 игровых часов).
constexpr std::uint64_t kInfectionFades = 3 * kTicksPerGameDay;
// Ивент без событий (ивент сам кончается не позже чем через 4 суток).
constexpr std::uint64_t kSalvageFades = 5 * kTicksPerGameDay;
// Сколько из узнавших о грузе попадает в события линии (участниками — все).
constexpr std::size_t kLearnerBeats = 8;
constexpr std::uint32_t kNoEvent = std::numeric_limits<std::uint32_t>::max();

// Поступок, после которого остаётся обида (MemoryKind).
bool isOffence(std::int32_t kind) {
    switch (static_cast<MemoryKind>(kind)) {
    case MemoryKind::Abandoned:
    case MemoryKind::Robbed:
    case MemoryKind::Expelled:
    case MemoryKind::Betrayed:
    case MemoryKind::Assaulted:
    case MemoryKind::Killed:
        return true;
    default:
        return false;
    }
}

} // namespace

void StoryTracker::update(const World& world) {
    const std::vector<ChronicleEvent>& events = world.chronicle().events();
    for (; cursor_ < events.size(); ++cursor_) {
        consume(world, events[cursor_], static_cast<std::uint32_t>(cursor_));
    }
    if (world.tick() >= nextExpire_) {
        expire(world.tick());
        nextExpire_ = world.tick() + kTicksPerGameHour;
    }
}

const Story* StoryTracker::find(std::uint32_t id) const {
    return id >= 1 && id <= stories_.size() ? &stories_[id - 1] : nullptr;
}

std::vector<std::uint32_t> StoryTracker::storiesOf(EntityId id) const {
    std::vector<std::uint32_t> ids;
    if (const auto it = byEntity_.find(id); it != byEntity_.end()) {
        for (const std::uint32_t index : it->second) {
            ids.push_back(index + 1);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

const Story* StoryTracker::lineOf(const Foresight& foresight) const {
    const auto byKey = [this](const std::unordered_map<EntityId, std::uint32_t>& map,
                              EntityId key) -> const Story* {
        const auto it = map.find(key);
        return it != map.end() ? &stories_[it->second] : nullptr;
    };
    const Story* story = nullptr;
    switch (foresight.kind) {
    case ForesightKind::Revenge:
    case ForesightKind::Theft:
        if (foresight.other != kInvalidId && foresight.hero != foresight.other) {
            if (const auto it = feuds_.find(pairOf(foresight.hero, foresight.other));
                it != feuds_.end()) {
                story = &stories_[it->second];
            }
        }
        break;
    case ForesightKind::Turning:
        story = byKey(infections_, foresight.hero);
        break;
    case ForesightKind::CacheRace:
        story = byKey(salvages_, foresight.focus);
        break;
    default:
        break;
    }
    return story != nullptr && story->active() ? story : nullptr;
}

bool StoryTracker::accepts(const Story& story, std::uint64_t tick) {
    return story.active() || tick <= story.ended + kEpilogue;
}

Story& StoryTracker::start(StoryKind kind, EntityId hero, EntityId other, EntityId focus,
                           std::uint64_t tick) {
    Story& story = stories_.emplace_back();
    story.id = static_cast<std::uint32_t>(stories_.size());
    story.kind = kind;
    story.hero = hero;
    story.other = other;
    story.focus = focus;
    story.begun = tick;
    story.updated = tick;
    join(story, hero);
    join(story, other);
    index(story, focus);
    return story;
}

void StoryTracker::index(const Story& story, EntityId id) {
    if (id == kInvalidId) {
        return;
    }
    std::vector<std::uint32_t>& list = byEntity_[id];
    const std::uint32_t at = story.id - 1;
    if (std::find(list.begin(), list.end(), at) == list.end()) {
        list.push_back(at);
    }
}

void StoryTracker::join(Story& story, EntityId id) {
    if (id == kInvalidId || id == story.focus ||
        std::find(story.cast.begin(), story.cast.end(), id) != story.cast.end()) {
        return;
    }
    story.cast.push_back(id);
    index(story, id);
}

void StoryTracker::beat(Story& story, std::uint32_t index, StoryStage stage, std::uint64_t tick) {
    if (!accepts(story, tick) || (!story.beats.empty() && story.beats.back().event == index)) {
        return;
    }
    if (!story.active()) {
        story.beats.push_back({index, StoryStage::Ended}); // эпилог
        story.updated = tick;
        return;
    }
    story.beats.push_back({index, stage});
    story.stage = std::max(story.stage, stage);
    story.updated = tick;
}

void StoryTracker::finish(Story& story, std::uint32_t index, StoryEnding ending,
                          std::uint64_t tick) {
    if (!story.active()) {
        return;
    }
    if (index != kNoEvent) {
        story.beats.push_back({index, StoryStage::Ended});
        story.updated = tick;
    }
    story.stage = StoryStage::Ended;
    story.ending = ending;
    story.ended = tick;
}

Story* StoryTracker::keyed(const std::unordered_map<EntityId, std::uint32_t>& map, EntityId key) {
    const auto it = map.find(key);
    return it != map.end() ? &stories_[it->second] : nullptr;
}

Story* StoryTracker::feud(EntityId victim, EntityId offender, bool open, std::uint64_t tick) {
    if (victim == kInvalidId || offender == kInvalidId || victim == offender) {
        return nullptr;
    }
    const Pair key = pairOf(victim, offender);
    if (const auto it = feuds_.find(key); it != feuds_.end()) {
        Story& story = stories_[it->second];
        if (accepts(story, tick)) {
            return &story;
        }
    }
    if (!open) {
        return nullptr;
    }
    Story& story = start(StoryKind::Feud, victim, offender, kInvalidId, tick);
    story.betrayal = friends_.contains(key);
    feuds_[key] = story.id - 1;
    return &story;
}

void StoryTracker::offence(EntityId victim, EntityId offender, std::uint32_t index,
                           std::uint64_t tick) {
    if (Story* story = feud(victim, offender, true, tick)) {
        beat(*story, index, story->beats.empty() ? StoryStage::Setup : StoryStage::Rising, tick);
    }
}

void StoryTracker::consume(const World& world, const ChronicleEvent& event, std::uint32_t index) {
    using K = ChronicleKind;
    const std::uint64_t tick = event.tick;
    switch (event.kind) {
    // --- Вражда --------------------------------------------------------------
    case K::Befriended:
        friends_.insert(pairOf(event.subject, event.object));
        break;
    case K::Stole: // вор — subject, хозяин дома — third
        offence(event.third, event.subject, index, tick);
        break;
    case K::CaughtThief:
    case K::AbandonedBy:
    case K::AssaultedBy:
        offence(event.subject, event.object, index, tick);
        break;
    case K::BetrayedBy:
        offence(event.subject, event.object, index, tick);
        if (Story* story = feud(event.subject, event.object, false, tick)) {
            story->betrayal = true;
        }
        break;
    case K::HeardOf:
        if (isOffence(event.a)) {
            offence(event.subject, event.object, index, tick);
        }
        break;
    case K::Witnessed:
        if (event.a == static_cast<std::int32_t>(MemoryKind::Killed)) {
            // Видел, как убили близкого: теперь это его вражда с убийцей.
            offence(event.subject, event.object, index, tick);
            if (Story* story = feud(event.subject, event.object, false, tick)) {
                join(*story, event.third);
            }
        } else if (isOffence(event.a)) {
            offence(event.third, event.object, index, tick);
            if (Story* story = feud(event.third, event.object, false, tick)) {
                join(*story, event.subject);
            }
        }
        break;
    case K::Disliked:
        if (Story* story = feud(event.subject, event.object, false, tick)) {
            beat(*story, index, StoryStage::Rising, tick);
        }
        break;
    case K::Fled:
        if (event.b == 1) { // от человека
            if (Story* story = feud(event.subject, event.object, false, tick)) {
                beat(*story, index, StoryStage::Rising, tick);
            }
        }
        break;
    case K::Avenged:
        if (Story* story = feud(event.subject, event.object, true, tick)) {
            if (story->active()) {
                finish(*story, index, StoryEnding::Revenge, tick);
            } else {
                beat(*story, index, StoryStage::Ended, tick);
            }
        }
        break;
    case K::Reconciled:
        if (Story* story = feud(event.subject, event.object, false, tick)) {
            if (story->active()) {
                finish(*story, index, StoryEnding::Reconciled, tick);
            }
        }
        break;

    // --- Смерть касается всех линий погибшего ----------------------------------
    case K::Died: {
        const auto it = byEntity_.find(event.subject);
        if (it == byEntity_.end()) {
            break;
        }
        const std::vector<std::uint32_t> list = it->second; // finish и join могут дописать
        for (const std::uint32_t at : list) {
            Story& story = stories_[at];
            const bool main = event.subject == story.hero || event.subject == story.other;
            if (!story.active()) {
                if (main) {
                    beat(story, index, StoryStage::Ended, tick);
                }
                continue;
            }
            switch (story.kind) {
            case StoryKind::Feud:
                if (!main) {
                    beat(story, index, StoryStage::Rising, tick);
                } else if (event.object == story.hero) {
                    finish(story, index, StoryEnding::Revenge, tick);
                } else if (event.object == story.other) {
                    finish(story, index, StoryEnding::Killed, tick);
                } else {
                    finish(story, index, StoryEnding::Death, tick);
                }
                break;
            case StoryKind::Infection:
                if (event.subject == story.hero) {
                    beat(story, index, StoryStage::Climax, tick);
                }
                break;
            case StoryKind::Group:
            case StoryKind::Salvage:
                beat(story, index, StoryStage::Rising, tick);
                break;
            default:
                break;
            }
        }
        break;
    }
    case K::Mourned:   // subject узнал о гибели близкого object
    case K::SawTurned: // subject узнал в зомби близкого object
        if (const auto it = byEntity_.find(event.object); it != byEntity_.end()) {
            const std::vector<std::uint32_t> list = it->second;
            for (const std::uint32_t at : list) {
                Story& story = stories_[at];
                if ((story.kind == StoryKind::Feud || story.kind == StoryKind::Infection) &&
                    event.object == story.hero && accepts(story, tick)) {
                    join(story, event.subject);
                    beat(story, index, StoryStage::Rising, tick);
                }
            }
        }
        break;

    // --- Группа ----------------------------------------------------------------
    case K::GroupFormed: {
        Story& story = start(StoryKind::Group, event.subject, kInvalidId, event.object, tick);
        groups_[event.object] = story.id - 1;
        beat(story, index, StoryStage::Setup, tick);
        break;
    }
    case K::JoinedGroup:
        if (Story* story = keyed(groups_, event.object)) {
            join(*story, event.subject);
            // Первые, кто вступил сразу, — ещё завязка.
            const bool founding = tick < story->begun + kTicksPerGameHour;
            beat(*story, index, founding ? StoryStage::Setup : StoryStage::Rising, tick);
        }
        break;
    case K::LeftGroup:
        if (Story* story = keyed(groups_, event.object)) {
            beat(*story, index, StoryStage::Rising, tick);
        }
        break;
    case K::Expelled: // вожак subject изгнал object из группы third
        if (Story* story = keyed(groups_, event.third)) {
            beat(*story, index, StoryStage::Climax, tick);
        }
        offence(event.object, event.subject, index, tick);
        break;
    case K::LedGroup: // a: 0 — прежний погиб, 1 — сместил его, 2 — прежний сбежал
        if (Story* story = keyed(groups_, event.object)) {
            if (story->active()) {
                story->hero = event.subject;
                join(*story, event.subject);
            }
            beat(*story, index, event.a == 1 ? StoryStage::Climax : StoryStage::Rising, tick);
        }
        break;
    case K::Deserted:
        if (Story* story = keyed(groups_, event.object)) {
            beat(*story, index, StoryStage::Climax, tick);
        }
        break;
    case K::GroupDisbanded:
        if (Story* story = keyed(groups_, event.subject)) {
            join(*story, event.object);
            finish(*story, index, StoryEnding::Disbanded, tick);
        }
        break;

    // --- Заражение -------------------------------------------------------------
    case K::Bitten:
        if (Story* story = keyed(infections_, event.subject); story != nullptr && story->active()) {
            beat(*story, index, StoryStage::Rising, tick); // укусили ещё раз
        } else {
            Story& fresh = start(StoryKind::Infection, event.subject, event.object, kInvalidId, tick);
            infections_[event.subject] = fresh.id - 1;
            beat(fresh, index, StoryStage::Setup, tick);
        }
        break;
    case K::Feverish:
        if (Story* story = keyed(infections_, event.subject); story != nullptr && story->active()) {
            beat(*story, index, StoryStage::Rising, tick);
        }
        break;
    case K::Turned:
        if (Story* story = keyed(infections_, event.subject)) {
            finish(*story, index, StoryEnding::Turned, tick);
        }
        break;

    // --- Груз ивента -----------------------------------------------------------
    case K::EventBegan: {
        Story& story = start(StoryKind::Salvage, kInvalidId, kInvalidId, event.subject, tick);
        salvages_[event.subject] = story.id - 1;
        beat(story, index, StoryStage::Setup, tick);
        break;
    }
    case K::LearnedOfEvent:
        if (Story* story = keyed(salvages_, event.object); story != nullptr && story->active()) {
            if (story->hero == kInvalidId) {
                story->hero = event.subject;
            }
            if (story->cast.size() < kLearnerBeats) {
                beat(*story, index, StoryStage::Rising, tick);
            }
            join(*story, event.subject);
        }
        break;
    case K::Salvaged: { // object — груз (или сам ивент, если груза нет)
        EntityId eventId = event.object;
        if (!salvages_.contains(eventId)) {
            const entt::entity entity = world.find(event.object);
            const auto* cache =
                entity != entt::null ? world.registry().try_get<Cache>(entity) : nullptr;
            eventId = cache != nullptr ? cache->event : kInvalidId;
        }
        Story* story = keyed(salvages_, eventId);
        if (story == nullptr || !story->active()) {
            break;
        }
        join(*story, event.subject);
        if ((event.a > 0 || event.b > 0) && story->stage < StoryStage::Climax) {
            story->hero = event.subject; // первым добрался до добра
            beat(*story, index, StoryStage::Climax, tick);
        } else {
            beat(*story, index, StoryStage::Rising, tick);
        }
        break;
    }
    case K::EventEnded:
        if (Story* story = keyed(salvages_, event.subject)) {
            finish(*story, index,
                   story->stage == StoryStage::Climax ? StoryEnding::Claimed
                                                      : StoryEnding::Unclaimed,
                   tick);
        }
        break;
    default:
        break;
    }
}

void StoryTracker::expire(std::uint64_t now) {
    for (Story& story : stories_) {
        if (!story.active()) {
            continue;
        }
        const std::uint64_t idle = now - std::min(now, story.updated);
        switch (story.kind) {
        case StoryKind::Feud:
            if (idle > kFeudFades) {
                finish(story, kNoEvent, StoryEnding::Faded, now);
            }
            break;
        case StoryKind::Infection:
            if (story.stage == StoryStage::Climax && idle > kBodyStays) {
                finish(story, kNoEvent, StoryEnding::Death, now);
            } else if (idle > kInfectionFades) {
                finish(story, kNoEvent, StoryEnding::Faded, now);
            }
            break;
        case StoryKind::Salvage:
            if (idle > kSalvageFades) {
                finish(story, kNoEvent,
                       story.stage == StoryStage::Climax ? StoryEnding::Claimed
                                                         : StoryEnding::Unclaimed,
                       now);
            }
            break;
        default: // группа живёт, пока не распадётся
            break;
        }
    }
}

float storyInterest(const Story& story, std::uint64_t now) {
    float weight = 1.0f;
    switch (story.kind) {
    case StoryKind::Feud:
        weight = story.betrayal ? 1.2f : 1.0f;
        break;
    case StoryKind::Group:
        weight = 0.6f;
        break;
    case StoryKind::Infection:
        weight = 0.9f;
        break;
    case StoryKind::Salvage:
        weight = 0.8f;
        break;
    default:
        break;
    }
    constexpr float kStageWeight[] = {0.6f, 1.0f, 1.5f, 0.8f};
    static_assert(std::size(kStageWeight) == static_cast<std::size_t>(StoryStage::Count));
    // Свежесть: линия остывает за несколько игровых часов, закрытая — быстрее.
    const float hours = static_cast<float>(now - std::min(now, story.updated)) /
                        static_cast<float>(kTicksPerGameHour);
    const float freshness = std::exp(-hours / (story.active() ? 8.0f : 2.0f));
    const float richness = 1.0f +
                           0.12f * static_cast<float>(std::min<std::size_t>(story.beats.size(), 12)) +
                           0.08f * static_cast<float>(std::min<std::size_t>(story.cast.size(), 8));
    return weight * kStageWeight[static_cast<std::size_t>(story.stage)] * freshness * richness;
}

StoryEnding expectedEnding(const Story& story) {
    switch (story.kind) {
    case StoryKind::Feud:
        return StoryEnding::Revenge;
    case StoryKind::Group:
        return StoryEnding::Disbanded;
    case StoryKind::Infection:
        return StoryEnding::Turned;
    case StoryKind::Salvage:
        return StoryEnding::Claimed;
    default:
        return StoryEnding::None;
    }
}

const char* storyKindName(StoryKind kind) {
    constexpr const char* kNames[] = {"feud", "group", "infection", "salvage"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(StoryKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "?";
}

const char* storyStageName(StoryStage stage) {
    constexpr const char* kNames[] = {"setup", "rising", "climax", "ended"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(StoryStage::Count));
    const auto index = static_cast<std::size_t>(stage);
    return index < std::size(kNames) ? kNames[index] : "?";
}

const char* storyEndingName(StoryEnding ending) {
    constexpr const char* kNames[] = {"none",    "revenge",   "reconciled", "killed",
                                      "death",   "faded",     "turned",     "disbanded",
                                      "claimed", "unclaimed"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(StoryEnding::Count));
    const auto index = static_cast<std::size_t>(ending);
    return index < std::size(kNames) ? kNames[index] : "?";
}

} // namespace lw
