#include "ui/Director.h"

#include "sim/Components.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"
#include "ui/Inspector.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lw::ui {

namespace {

// Режиссёр держит линию хотя бы столько реальных секунд, прежде чем сменить
// её на более интересную (если линия не оборвалась): камера не дёргается.
constexpr float kMinHold = 20.0f;
// Новая линия должна быть заметно интереснее текущей.
constexpr float kSwitchMargin = 1.3f;
// Линии пересматриваются не каждый кадр.
constexpr float kChoicePeriod = 0.5f;
// Совсем остывшую линию бросают, не дожидаясь kMinHold.
constexpr float kDeadInterest = 0.02f;
// Сколько линий в списке окна.
constexpr std::size_t kListed = 30;
// Предвкушение (12а.2). Предвестие срочное, когда до кульминации осталось
// столько реальных секунд: режиссёр едет туда, не дожидаясь kMinHold (но не
// раньше kRushHold на прежнем кадре и если оно не намного скучнее текущего —
// kRushShare), и не уходит, пока оно срочное.
constexpr float kLead = 15.0f;
constexpr float kRushHold = 4.0f;
constexpr float kRushShare = 0.6f;
// Предвестие без линии сбылось или сорвалось — столько секунд досмотреть.
constexpr float kAftermath = 12.0f;
// Скорость времени ниже этого — пауза: срочного нет.
constexpr float kStillRate = 0.05f;
// Сглаживание замера скорости времени, 1/с.
constexpr float kRateSmoothing = 2.0f;
// Сколько предвестий в окне.
constexpr std::size_t kOmensListed = 8;

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

// «нарастание — чем кончится: месть?» или «развязка: месть».
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

// «Вражда: John Miller #12 — Peter Green #40».
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

} // namespace

Director::Director(const World& world) : world_(world) {}

bool Director::Pick::same(const Pick& other) const {
    if (story != 0 || other.story != 0) {
        return story == other.story;
    }
    return hasOmen && other.hasOmen && omen.same(other.omen);
}

void Director::setEnabled(bool enabled) {
    enabled_ = enabled;
    current_ = {};
    lost_ = false;
    shown_ = kInvalidId;
    held_ = 0.0f;
    sinceChoice_ = kChoicePeriod; // выбрать линию сразу
}

bool Director::exists(EntityId id) const {
    return id != kInvalidId && world_.find(id) != entt::null;
}

EntityId Director::targetOf(const Story& story) const {
    for (const EntityId id : {story.hero, story.focus}) {
        if (exists(id)) {
            return id;
        }
    }
    return kInvalidId;
}

std::vector<Director::Pick> Director::candidates() const {
    const std::uint64_t now = world_.tick();
    std::vector<Pick> picks;
    // Самое интересное предвестие каждой линии (omens_ — по убыванию).
    std::unordered_map<std::uint32_t, const Foresight*> lineOmen;
    for (const Foresight& omen : omens_) {
        if (const Story* story = stories_.lineOf(omen)) {
            lineOmen.try_emplace(story->id, &omen);
        } else if (exists(omen.hero)) {
            Pick& pick = picks.emplace_back();
            pick.hasOmen = true;
            pick.omen = omen;
            pick.target = omen.hero;
            pick.interest = omen.interest;
        }
    }
    for (const Story& story : stories_.stories()) {
        Pick pick;
        pick.story = story.id;
        pick.interest = storyInterest(story, now);
        pick.target = targetOf(story);
        if (const auto it = lineOmen.find(story.id); it != lineOmen.end()) {
            pick.hasOmen = true;
            pick.omen = *it->second;
            pick.interest += it->second->interest;
            if (exists(it->second->hero)) {
                pick.target = it->second->hero; // мститель, вор, кто ближе к грузу
            }
        }
        if (pick.target != kInvalidId && pick.interest > 0.0f) {
            picks.push_back(pick);
        }
    }
    std::stable_sort(picks.begin(), picks.end(),
                     [](const Pick& a, const Pick& b) { return a.interest > b.interest; });
    return picks;
}

float Director::realEta(const Foresight& omen) const {
    return simRate_ > kStillRate ? omen.eta / simRate_ : std::numeric_limits<float>::infinity();
}

bool Director::urgent(const Pick& pick) const {
    return pick.hasOmen && realEta(pick.omen) <= kLead;
}

void Director::show(const Pick& pick, Inspector& inspector) {
    if (pick.target == kInvalidId) {
        return;
    }
    if (!pick.same(current_) || lost_) {
        held_ = 0.0f;
    }
    current_ = pick;
    lost_ = false;
    if (pick.story != 0) {
        opened_ = pick.story;
    }
    shown_ = pick.target;
    inspector.select(shown_);
    inspector.setFollow(true);
}

void Director::update(Inspector& inspector, float dt) {
    stories_.update(world_);
    // Скорость времени — по тому, сколько тиков прошло за кадр.
    const std::uint64_t tick = world_.tick();
    if (dt > 0.0f) {
        const float rate = static_cast<float>(tick - std::min(tick, lastTick_)) /
                           static_cast<float>(kTicksPerSecond) / dt;
        simRate_ += (rate - simRate_) * std::min(1.0f, dt * kRateSmoothing);
    }
    lastTick_ = tick;
    sinceChoice_ += dt;
    const bool refresh = sinceChoice_ >= kChoicePeriod;
    if (refresh) {
        sinceChoice_ = 0.0f;
        omens_ = foresee(world_);
    }
    if (!enabled_) {
        return;
    }
    if (shown_ != kInvalidId && inspector.selected() != shown_) {
        enabled_ = false; // пользователь выбрал своё — камера его
        return;
    }
    held_ += dt;
    if (!refresh) {
        return;
    }

    const std::vector<Pick> picks = candidates();
    const auto found = std::find_if(picks.begin(), picks.end(),
                                    [this](const Pick& pick) { return pick.same(current_); });
    float currentInterest = 0.0f;
    bool currentUrgent = false;
    if (found != picks.end()) {
        if (current_.hasOmen && !found->hasOmen) {
            held_ = 0.0f; // кульминация случилась — досмотреть, что вышло
        }
        const bool retarget = found->target != current_.target;
        current_ = *found;
        if (retarget) {
            show(current_, inspector); // сменился герой: новый вожак, первый у груза
        }
        currentInterest = current_.interest;
        currentUrgent = urgent(current_);
    } else if (current_.valid()) {
        // Предвестие без линии сбылось или сорвалось (орда пришла, кража
        // случилась) — досмотреть, чем кончилось, и только потом уходить.
        if (!lost_) {
            lost_ = true;
            held_ = 0.0f;
        }
        currentInterest = held_ < kAftermath && exists(current_.target) ? current_.interest : 0.0f;
    }
    if (picks.empty() || found == picks.begin()) {
        return;
    }
    const Pick& best = picks.front();
    if (currentInterest < kDeadInterest) {
        show(best, inspector);
        return;
    }
    if (currentUrgent) {
        return; // вот-вот кульминация — от неё не уходят
    }
    // Успеть к кульминации: срочное предвестие перехватывает камеру раньше срока.
    const auto rush = std::find_if(picks.begin(), picks.end(),
                                   [this](const Pick& pick) { return urgent(pick); });
    if (rush != picks.end() && held_ >= kRushHold &&
        rush->interest >= kRushShare * currentInterest) {
        show(*rush, inspector);
        return;
    }
    if (held_ >= kMinHold && best.interest > currentInterest * kSwitchMargin) {
        show(best, inspector);
    }
}

std::string Director::omenLine(const Foresight& omen) const {
    const entt::registry& registry = world_.registry();
    std::string line;
    char text[160];
    switch (omen.kind) {
    case ForesightKind::Revenge:
        line = idLabel(omen.hero) + " идёт мстить " + idLabel(omen.other);
        break;
    case ForesightKind::Raid: {
        const entt::entity horde = world_.find(omen.focus);
        const entt::entity place = world_.find(omen.other);
        const std::size_t size =
            horde != entt::null ? registry.get<Horde>(horde).members.size() : std::size_t{0};
        std::snprintf(text, sizeof(text), "Орда (%zu) идёт к поселению %s, там людей: %d", size,
                      place != entt::null ? registry.get<Settlement>(place).name.c_str() : "?",
                      omen.crowd);
        line = text;
        break;
    }
    case ForesightKind::Turning: {
        const entt::entity body = world_.find(omen.hero);
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
        const entt::entity thief = world_.find(omen.hero);
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

Director::Pick Director::pickOf(const Story& story) const {
    Pick pick;
    pick.story = story.id;
    pick.interest = storyInterest(story, world_.tick());
    pick.target = targetOf(story);
    for (const Foresight& omen : omens_) {
        if (stories_.lineOf(omen) == &story) {
            pick.hasOmen = true;
            pick.omen = omen;
            pick.interest += omen.interest;
            if (exists(omen.hero)) {
                pick.target = omen.hero;
            }
            break;
        }
    }
    return pick;
}

Director::Pick Director::pickOf(const Foresight& omen) const {
    if (const Story* story = stories_.lineOf(omen)) {
        return pickOf(*story);
    }
    Pick pick;
    pick.hasOmen = true;
    pick.omen = omen;
    pick.target = exists(omen.hero) ? omen.hero : kInvalidId;
    pick.interest = omen.interest;
    return pick;
}

void Director::drawPanel(Inspector& inspector) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Слева посередине: слева вверху и внизу — время, внизу по центру — летопись,
    // справа — статистика и инспектор.
    ImGui::SetNextWindowPos({12.0f, display.y * 0.3f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460.0f, 420.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Режиссёр###director")) {
        ImGui::End();
        return;
    }
    bool enabled = enabled_;
    if (ImGui::Checkbox("Режиссёр: камера сама ведёт историю", &enabled)) {
        setEnabled(enabled);
    }

    const std::uint64_t now = world_.tick();
    const std::vector<Story>& all = stories_.stories();
    std::size_t active = 0;
    std::vector<std::pair<float, const Story*>> listed;
    for (const Story& story : all) {
        active += story.active() ? 1 : 0;
        const float interest = storyInterest(story, now);
        if (story.active() || interest > kDeadInterest) {
            listed.emplace_back(interest, &story);
        }
    }
    std::stable_sort(listed.begin(), listed.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    ImGui::TextDisabled("линий: %zu, идут: %zu; вот-вот: %zu", all.size(), active, omens_.size());

    EntityId clicked = kInvalidId;
    // Что вот-вот случится (12а.2): самые интересные предвестия.
    if (!omens_.empty()) {
        const float rows = static_cast<float>(std::min(kOmensListed, omens_.size()));
        const float height = (rows + 0.5f) * ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginChild("omens", {0.0f, height}, true)) {
            for (std::size_t i = 0; i < std::min(kOmensListed, omens_.size()); ++i) {
                const Foresight& omen = omens_[i];
                ImGui::PushID(static_cast<int>(i));
                const std::string label =
                    std::string(omenTitle(omen.kind)) + ": " + omenLine(omen);
                const bool watched = enabled_ && current_.hasOmen && current_.omen.same(omen);
                if (ImGui::Selectable(label.c_str(), watched)) {
                    const Pick pick = pickOf(omen);
                    if (enabled_) {
                        show(pick, inspector); // режиссёр продолжает отсюда
                    } else if (pick.target != kInvalidId) {
                        inspector.select(pick.target);
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%.2f", omen.interest);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    if (ImGui::BeginChild("stories", {0.0f, ImGui::GetContentRegionAvail().y * 0.45f}, true)) {
        for (std::size_t i = 0; i < std::min(kListed, listed.size()); ++i) {
            const Story& story = *listed[i].second;
            ImGui::PushID(static_cast<int>(story.id));
            const std::string label = titleOf(story) + " · " + stageTitle(story.stage);
            const bool current = enabled_ && story.id == current_.story;
            if (ImGui::Selectable(label.c_str(), story.id == opened_ || current)) {
                opened_ = story.id;
                const Pick pick = pickOf(story);
                if (enabled_) {
                    show(pick, inspector); // режиссёр продолжает с этой линии
                } else if (pick.target != kInvalidId) {
                    inspector.select(pick.target);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%.2f", listed[i].first);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (const Story* story = stories_.find(opened_)) {
        ImGui::TextUnformatted(titleOf(*story).c_str());
        ImGui::TextDisabled("%s; событий %zu", stateLine(*story).c_str(), story->beats.size());
        ImGui::TextUnformatted("Участники:");
        for (const EntityId id : story->cast) {
            ImGui::SameLine();
            if (idLink(id)) {
                clicked = id;
            }
        }
        if (ImGui::BeginChild("beats", {0.0f, 0.0f}, true)) {
            const std::vector<ChronicleEvent>& events = world_.chronicle().events();
            for (std::size_t i = 0; i < story->beats.size(); ++i) {
                const StoryBeat& beat = story->beats[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextDisabled("%-11s", stageTitle(beat.stage));
                ImGui::SameLine();
                if (const EntityId id = drawChronicleEvent(events[beat.event], kInvalidId);
                    id != kInvalidId) {
                    clicked = id;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    if (clicked != kInvalidId) {
        enabled_ = false; // пользователь выбрал своё
        inspector.select(clicked);
    }
    ImGui::End();
}

void Director::drawCaption(Inspector& inspector) {
    if (!enabled_ || !current_.valid()) {
        return;
    }
    const Story* story = stories_.find(current_.story);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, 12.0f}, ImGuiCond_Always, {0.5f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("###director-caption", nullptr, kFlags)) {
        if (story != nullptr) {
            ImGui::TextUnformatted(titleOf(*story).c_str());
            ImGui::TextDisabled("%s", stateLine(*story).c_str());
        } else {
            ImGui::TextUnformatted(omenTitle(current_.omen.kind));
            if (lost_) {
                ImGui::TextDisabled("случилось — смотрим, чем кончилось");
            }
        }
        if (current_.hasOmen && !lost_) {
            // Что вот-вот случится: ярче, когда кульминация совсем близко.
            const bool soon = urgent(current_);
            const ImVec4 color = soon ? ImVec4{1.0f, 0.62f, 0.3f, 1.0f} : ImVec4{0.95f, 0.85f, 0.5f, 1.0f};
            ImGui::TextColored(color, "Вот-вот: %s", omenLine(current_.omen).c_str());
        }
        if (story != nullptr && !story->beats.empty()) {
            const ChronicleEvent& last = world_.chronicle().events()[story->beats.back().event];
            if (const EntityId id = drawChronicleEvent(last, kInvalidId); id != kInvalidId) {
                enabled_ = false;
                inspector.select(id);
            }
        }
    }
    ImGui::End();
}

} // namespace lw::ui
