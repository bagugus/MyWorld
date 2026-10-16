#include "ui/Director.h"

#include "sim/World.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"
#include "ui/Inspector.h"

#include <imgui.h>

#include <algorithm>
#include <iterator>
#include <string>
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

void Director::setEnabled(bool enabled) {
    enabled_ = enabled;
    current_ = 0;
    shown_ = kInvalidId;
    held_ = 0.0f;
    sinceChoice_ = kChoicePeriod; // выбрать линию сразу
}

EntityId Director::targetOf(const Story& story) const {
    for (const EntityId id : {story.hero, story.focus}) {
        if (id != kInvalidId && world_.find(id) != entt::null) {
            return id;
        }
    }
    return kInvalidId;
}

void Director::show(const Story& story, Inspector& inspector) {
    if (targetOf(story) == kInvalidId) {
        return;
    }
    if (story.id != current_) {
        held_ = 0.0f;
    }
    current_ = story.id;
    opened_ = story.id;
    shown_ = targetOf(story);
    inspector.select(shown_);
    inspector.setFollow(true);
}

void Director::update(Inspector& inspector, float dt) {
    stories_.update(world_);
    if (!enabled_) {
        return;
    }
    if (shown_ != kInvalidId && inspector.selected() != shown_) {
        enabled_ = false; // пользователь выбрал своё — камера его
        return;
    }
    held_ += dt;
    sinceChoice_ += dt;

    const std::uint64_t now = world_.tick();
    const Story* current = stories_.find(current_);
    if (current != nullptr && targetOf(*current) != shown_ && targetOf(*current) != kInvalidId) {
        show(*current, inspector); // сменился герой: новый вожак, первый у груза
    }
    if (sinceChoice_ < kChoicePeriod) {
        return;
    }
    sinceChoice_ = 0.0f;

    const Story* best = nullptr;
    float bestInterest = 0.0f;
    for (const Story& story : stories_.stories()) {
        const float interest = storyInterest(story, now);
        if (interest > bestInterest && targetOf(story) != kInvalidId) {
            best = &story;
            bestInterest = interest;
        }
    }
    if (best == nullptr || best == current) {
        return;
    }
    const float currentInterest =
        current != nullptr && targetOf(*current) != kInvalidId ? storyInterest(*current, now) : 0.0f;
    if (currentInterest < kDeadInterest ||
        (held_ >= kMinHold && bestInterest > currentInterest * kSwitchMargin)) {
        show(*best, inspector);
    }
}

void Director::drawPanel(Inspector& inspector) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Слева посередине: слева вверху и внизу — время, внизу по центру — летопись,
    // справа — статистика и инспектор.
    ImGui::SetNextWindowPos({12.0f, display.y * 0.3f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460.0f, 360.0f}, ImGuiCond_FirstUseEver);
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
    ImGui::TextDisabled("линий: %zu, идут: %zu", all.size(), active);

    EntityId clicked = kInvalidId;
    if (ImGui::BeginChild("stories", {0.0f, ImGui::GetContentRegionAvail().y * 0.45f}, true)) {
        for (std::size_t i = 0; i < std::min(kListed, listed.size()); ++i) {
            const Story& story = *listed[i].second;
            ImGui::PushID(static_cast<int>(story.id));
            const std::string label = titleOf(story) + " · " + stageTitle(story.stage);
            const bool current = enabled_ && story.id == current_;
            if (ImGui::Selectable(label.c_str(), story.id == opened_ || current)) {
                opened_ = story.id;
                if (enabled_) {
                    show(story, inspector); // режиссёр продолжает с этой линии
                } else if (const EntityId target = targetOf(story); target != kInvalidId) {
                    inspector.select(target);
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
    const Story* story = enabled_ ? stories_.find(current_) : nullptr;
    if (story == nullptr) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, 12.0f}, ImGuiCond_Always, {0.5f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("###director-caption", nullptr, kFlags)) {
        ImGui::TextUnformatted(titleOf(*story).c_str());
        ImGui::TextDisabled("%s", stateLine(*story).c_str());
        if (!story->beats.empty()) {
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
