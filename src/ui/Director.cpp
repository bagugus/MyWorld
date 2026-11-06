#include "ui/Director.h"

#include "sim/Components.h"
#include "sim/SimDriver.h"
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
#include <span>
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
// Главные герои (12а.3). Линия или предвестие с героем сериала интереснее в
// (1 + kHeroBonus + kMissedBonus × «соскучились»); соскучились — когда героя не
// показывали дольше kMissedAfter реальных секунд, полностью — ещё через kMissedRamp.
constexpr float kHeroBonus = 0.5f;
constexpr float kMissedBonus = 0.6f;
constexpr float kMissedAfter = 60.0f;
constexpr float kMissedRamp = 120.0f;
// Навестить героя в затишье: от kVisit (видели только что) до
// kVisit × (1 + kVisitMissed) — живая линия всегда интереснее.
constexpr float kVisit = 0.08f;
constexpr float kVisitMissed = 2.0f;
// Перерыв, после которого показ того же героя — новый эпизод.
constexpr float kEpisodeGap = 30.0f;
// Сколько секунд в подписи «возвращаемся» и «новый герой».
constexpr float kEpisodeTitle = 12.0f;
// Темп и монтаж (12а.4). Параллельный монтаж: две линии не скучнее
// kParallelMin, вторая — не меньше kParallelShare первой; камера переходит
// между ними каждые kCrossCut секунд.
constexpr float kCrossCut = 12.0f;
constexpr float kParallelMin = 0.35f;
constexpr float kParallelShare = 0.7f;
// Напряжение сглаживается за kTensionTime секунд; кульминация, досмотренная
// при напряжении не ниже kTenseEnough, — передышка на kBreather секунд.
constexpr float kTensionTime = 8.0f;
constexpr float kTenseEnough = 0.5f;
constexpr float kBreather = 20.0f;
// Затишье: ни одно предвестие интереснее kQuietOmen не случится раньше чем
// через kQuietEta секунд симуляции, а кадр скучнее kLullInterest. Счёт — в
// секундах симуляции, чтобы ускорение само не обрывало затишье.
constexpr float kQuietEta = 240.0f;
constexpr float kQuietOmen = 0.15f;
constexpr float kLullInterest = 0.3f;
// Затишье дольше kQuietDelay секунд — время ×kLullSpeed (если пользователь не
// менял скорость последние kUserPace секунд).
constexpr float kQuietDelay = 3.0f;
constexpr int kLullSpeed = 10;
constexpr float kUserPace = 30.0f;
// Сколько секунд подпись «Тем временем…».
constexpr float kCutTitle = 5.0f;

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
    if (visit != kInvalidId || other.visit != kInvalidId) {
        return visit == other.visit;
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
    partner_ = {};
    breather_ = 0.0f;
    tension_ = 0.0f;
    quietFor_ = 0.0f;
    currentInterest_ = 0.0f;
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

EntityId Director::protagonistOf(const Pick& pick) const {
    if (pick.visit != kInvalidId) {
        return pick.visit;
    }
    if (cast_.isHero(pick.target)) {
        return pick.target;
    }
    if (const Story* story = stories_.find(pick.story)) {
        for (const EntityId id : {story->hero, story->other}) {
            if (cast_.isHero(id)) {
                return id;
            }
        }
        for (const EntityId id : story->cast) {
            if (cast_.isHero(id)) {
                return id;
            }
        }
    }
    if (pick.hasOmen) {
        for (const EntityId id : {pick.omen.hero, pick.omen.other}) {
            if (cast_.isHero(id)) {
                return id;
            }
        }
    }
    return kInvalidId;
}

float Director::missed(EntityId hero) const {
    const auto it = episodes_.find(hero);
    if (it == episodes_.end()) {
        return 1.0f; // ни разу не показывали
    }
    const float absence = clock_ - it->second.lastSeen;
    return std::clamp((absence - kMissedAfter) / kMissedRamp, 0.0f, 1.0f);
}

void Director::rate(Pick& pick) const {
    // Линия без предвестия: если герой сериала в ней герой или противник —
    // камера на нём.
    if (const Story* story = stories_.find(pick.story);
        story != nullptr && !pick.hasOmen && !cast_.isHero(pick.target)) {
        for (const EntityId id : {story->hero, story->other}) {
            if (cast_.isHero(id) && exists(id)) {
                pick.target = id;
                break;
            }
        }
    }
    if (const EntityId hero = protagonistOf(pick); hero != kInvalidId) {
        pick.interest *= 1.0f + kHeroBonus + kMissedBonus * missed(hero);
    }
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
    for (Pick& pick : picks) {
        rate(pick);
    }
    // Главные герои без линии и предвестия — навестить (12а.3).
    for (const Protagonist& hero : cast_.heroes()) {
        if (!hero.alive() || !exists(hero.id) ||
            std::any_of(picks.begin(), picks.end(),
                        [&](const Pick& pick) { return protagonistOf(pick) == hero.id; })) {
            continue;
        }
        Pick& pick = picks.emplace_back();
        pick.visit = hero.id;
        pick.target = hero.id;
        pick.interest = kVisit * (1.0f + kVisitMissed * missed(hero.id));
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

void Director::show(const Pick& pick, Inspector& inspector, Cut cut) {
    if (pick.target == kInvalidId) {
        return;
    }
    if (!pick.same(current_) || lost_) {
        held_ = 0.0f;
        cut_ = cut;
        cutAt_ = clock_;
        if (cut != Cut::Parallel && cut != Cut::Retarget) {
            partner_ = {}; // параллельный монтаж кончился
        }
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

void Director::trackEpisode() {
    if (!enabled_ || !cast_.isHero(shown_)) {
        episodeHero_ = kInvalidId;
        return;
    }
    Episodes& episodes = episodes_[shown_];
    const bool fresh = episodes.count == 0;
    if (shown_ != episodeHero_ || clock_ - episodes.lastSeen > kEpisodeGap) {
        ++episodes.count; // новый эпизод: к герою вернулись
        returnedAfter_ = fresh ? 0.0f : clock_ - episodes.lastSeen;
        episodeStart_ = clock_;
        episodeHero_ = shown_;
    }
    episodes.lastSeen = clock_;
}

float Director::intensity() const {
    if (!current_.valid()) {
        return 0.0f;
    }
    if (current_.hasOmen && !lost_ && urgent(current_)) {
        return 1.0f;
    }
    return std::clamp(currentInterest_, 0.0f, 1.0f);
}

bool Director::calm(EntityId id) const {
    const entt::entity entity = world_.find(id);
    if (entity == entt::null) {
        return false;
    }
    const auto* mind = world_.registry().try_get<Mind>(entity);
    if (mind == nullptr) {
        return false;
    }
    switch (mind->action) {
    case Action::Idle:
    case Action::Sleep:
    case Action::Eat:
    case Action::Drink:
    case Action::Forage:
    case Action::Socialize:
    case Action::Wander:
    case Action::Treat:
    case Action::Share:
    case Action::Trade:
    case Action::Work:
    case Action::Buy:
        return true;
    default:
        return false;
    }
}

const Director::Pick* Director::parallelPartner(const std::vector<Pick>& picks,
                                                float currentInterest) const {
    if (current_.visit != kInvalidId || currentInterest < kParallelMin) {
        return nullptr;
    }
    const float needed = std::max(kParallelMin, kParallelShare * currentInterest);
    const EntityId hero = protagonistOf(current_);
    // Та же пара, что и раньше: камера ходит между двумя линиями.
    if (partner_.valid()) {
        for (const Pick& pick : picks) {
            if (pick.same(partner_)) {
                return pick.interest >= needed && pick.target != current_.target ? &pick : nullptr;
            }
        }
        return nullptr;
    }
    for (const Pick& pick : picks) {
        if (pick.same(current_) || pick.visit != kInvalidId || pick.target == current_.target ||
            (hero != kInvalidId && protagonistOf(pick) == hero)) {
            continue;
        }
        return pick.interest >= needed ? &pick : nullptr; // picks — по убыванию
    }
    return nullptr;
}

const Director::Pick* Director::calmPick(const std::vector<Pick>& picks) const {
    // Сперва главный герой за мирным делом, потом кто угодно.
    for (const bool heroOnly : {true, false}) {
        for (const Pick& pick : picks) {
            if (pick.hasOmen || pick.same(current_) || !calm(pick.target) ||
                (heroOnly && protagonistOf(pick) == kInvalidId)) {
                continue;
            }
            return &pick;
        }
    }
    return nullptr;
}

bool Director::quiet() const {
    if (current_.hasOmen && !lost_ && (urgent(current_) || current_.omen.eta < kQuietEta)) {
        return false;
    }
    for (const Foresight& omen : omens_) {
        if (omen.eta < kQuietEta && omen.interest >= kQuietOmen) {
            return false;
        }
    }
    return currentInterest_ < kLullInterest;
}

void Director::pace(SimDriver& driver, float dt) {
    if (!enabled_ || !tempo_) {
        // Режиссёр больше не ведёт: вернуть скорость пользователя, если её
        // с тех пор не меняли.
        if (baseSpeed_ != 0 && driver.speed() == setSpeed_) {
            driver.setSpeed(baseSpeed_);
        }
        baseSpeed_ = setSpeed_ = 0;
        quietFor_ = 0.0f;
        return;
    }
    if (baseSpeed_ == 0) {
        baseSpeed_ = setSpeed_ = driver.speed();
        userPaced_ = kUserPace;
    }
    if (driver.speed() != setSpeed_) {
        // Скорость сменил пользователь: это новая основа, и какое-то время
        // режиссёр её не трогает.
        baseSpeed_ = setSpeed_ = driver.speed();
        userPaced_ = 0.0f;
    }
    userPaced_ += dt;
    quietFor_ = quiet() ? quietFor_ + dt : 0.0f;
    int wanted = baseSpeed_;
    if (quietFor_ >= kQuietDelay && userPaced_ >= kUserPace && !driver.paused() &&
        !driver.skipping()) {
        wanted = std::max(baseSpeed_, kLullSpeed);
    }
    if (wanted != driver.speed()) {
        driver.setSpeed(wanted);
    }
    setSpeed_ = driver.speed();
}

void Director::update(Inspector& inspector, SimDriver& driver, float dt) {
    stories_.update(world_);
    cast_.update(world_);
    clock_ += dt;
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
    if (enabled_ && shown_ != kInvalidId && inspector.selected() != shown_) {
        enabled_ = false; // пользователь выбрал своё — камера его
    }
    if (!enabled_) {
        pace(driver, dt);
        return;
    }
    held_ += dt;
    breather_ = std::max(0.0f, breather_ - dt);
    tension_ += (intensity() - tension_) * std::min(1.0f, dt / kTensionTime);
    trackEpisode();
    pace(driver, dt);
    if (!refresh) {
        return;
    }

    const std::vector<Pick> picks = candidates();
    const auto found = std::find_if(picks.begin(), picks.end(),
                                    [this](const Pick& pick) { return pick.same(current_); });
    float currentInterest = 0.0f;
    bool currentUrgent = false;
    bool resolved = false; // кульминация только что случилась
    if (found != picks.end()) {
        if (current_.hasOmen && !found->hasOmen) {
            held_ = 0.0f; // кульминация случилась — досмотреть, что вышло
            resolved = true;
        }
        const bool retarget = found->target != current_.target;
        current_ = *found;
        if (retarget) {
            show(current_, inspector, Cut::Retarget); // новый вожак, первый у груза
        }
        currentInterest = current_.interest;
        currentUrgent = urgent(current_);
    } else if (const auto adopted = std::find_if(picks.begin(), picks.end(),
                                                 [this](const Pick& pick) {
                                                     return current_.visit != kInvalidId &&
                                                            protagonistOf(pick) == current_.visit;
                                                 });
               adopted != picks.end()) {
        // Навещали героя — а у него началась линия: остаёмся с ним.
        const bool retarget = adopted->target != current_.target;
        current_ = *adopted;
        if (retarget) {
            show(current_, inspector, Cut::Retarget);
        }
        currentInterest = current_.interest;
        currentUrgent = urgent(current_);
    } else if (current_.valid()) {
        // Предвестие без линии сбылось или сорвалось (орда пришла, кража
        // случилась) — досмотреть, чем кончилось, и только потом уходить.
        if (!lost_) {
            lost_ = true;
            held_ = 0.0f;
            resolved = current_.hasOmen;
        }
        currentInterest = held_ < kAftermath && exists(current_.target) ? current_.interest : 0.0f;
    }
    if (resolved && tension_ >= kTenseEnough) {
        breather_ = kBreather; // после кульминации — передышка
    }
    currentInterest_ = currentInterest;
    if (picks.empty()) {
        return;
    }
    const bool resting = breather_ > 0.0f;
    if (currentInterest < kDeadInterest) {
        // Кадр остыл. В передышку — мирная сцена, иначе самое интересное.
        if (const Pick* rest = resting ? calmPick(picks) : nullptr) {
            show(*rest, inspector, Cut::Calm);
        } else if (found != picks.begin()) {
            show(picks.front(), inspector, Cut::Start);
        }
        return;
    }
    if (currentUrgent) {
        return; // вот-вот кульминация — от неё не уходят
    }
    if (found != picks.begin()) {
        // Успеть к кульминации: срочное предвестие перехватывает камеру раньше
        // срока. В передышку — только ради главного героя.
        const auto rush = std::find_if(picks.begin(), picks.end(),
                                       [this](const Pick& pick) { return urgent(pick); });
        if (rush != picks.end() && held_ >= kRushHold &&
            rush->interest >= kRushShare * currentInterest &&
            (!resting || protagonistOf(*rush) != kInvalidId)) {
            show(*rush, inspector, Cut::Rush);
            return;
        }
    }
    if (resting) {
        return; // передышка: камера остаётся, где была
    }
    // Параллельный монтаж: две горячие линии по очереди.
    if (held_ >= kCrossCut) {
        if (const Pick* partner = parallelPartner(picks, currentInterest)) {
            const Pick previous = current_;
            show(*partner, inspector, Cut::Parallel);
            partner_ = previous;
            return;
        }
        partner_ = {};
    }
    const Pick& best = picks.front();
    if (found != picks.begin() && held_ >= kMinHold &&
        best.interest > currentInterest * kSwitchMargin) {
        show(best, inspector, Cut::Switch);
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
    rate(pick);
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
    rate(pick);
    return pick;
}

void Director::drawPanel(Inspector& inspector) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Слева посередине: слева вверху и внизу — время, внизу по центру — летопись,
    // справа — статистика и инспектор.
    ImGui::SetNextWindowPos({12.0f, display.y * 0.3f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460.0f, 500.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Режиссёр###director")) {
        ImGui::End();
        return;
    }
    bool enabled = enabled_;
    if (ImGui::Checkbox("Режиссёр: камера сама ведёт историю", &enabled)) {
        setEnabled(enabled);
    }
    ImGui::Checkbox("Темп: ускорять время в затишье", &tempo_);
    if (enabled_) {
        // Темп и монтаж (12а.4): что сейчас делает режиссёр.
        std::string state = tension_ >= kTenseEnough ? "напряжение" : "спокойно";
        if (breather_ > 0.0f) {
            state += ", передышка";
        }
        if (partner_.valid()) {
            state += ", параллельный монтаж";
        }
        if (baseSpeed_ != 0 && setSpeed_ > baseSpeed_) {
            state += ", затишье — время x" + std::to_string(setSpeed_);
        }
        ImGui::TextDisabled("%s; напряжение %.2f, на кадре %.0f с", state.c_str(), tension_, held_);
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
    drawCast(inspector);

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
                        show(pick, inspector, Cut::Chosen); // режиссёр продолжает отсюда
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
                    show(pick, inspector, Cut::Chosen); // режиссёр продолжает с этой линии
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
        // Монтаж (12а.4): как мы сюда попали.
        const ImVec4 muted{0.7f, 0.78f, 0.9f, 1.0f};
        if (cut_ == Cut::Parallel && clock_ - cutAt_ < kCutTitle) {
            ImGui::TextColored(muted, "Тем временем…");
        } else if (breather_ > 0.0f) {
            ImGui::TextColored(muted, "Передышка");
        } else if (baseSpeed_ != 0 && setSpeed_ > baseSpeed_) {
            ImGui::TextColored(muted, "Затишье — время x%d", setSpeed_);
        }
        if (const EntityId hero = protagonistOf(current_); hero != kInvalidId) {
            drawHeroCaption(hero, inspector);
        }
        if (current_.visit != kInvalidId) {
            // Навещаем героя: что было с ним последним.
            const std::span<const std::uint32_t> biography =
                world_.chronicle().eventsOf(current_.visit);
            if (!biography.empty()) {
                const ChronicleEvent& last = world_.chronicle().events()[biography.back()];
                if (const EntityId id = drawChronicleEvent(last, kInvalidId); id != kInvalidId) {
                    enabled_ = false;
                    inspector.select(id);
                }
            }
        } else if (story != nullptr) {
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

void Director::drawHeroCaption(EntityId hero, Inspector& inspector) {
    const Protagonist* protagonist = cast_.find(hero);
    if (protagonist == nullptr) {
        return;
    }
    const auto it = episodes_.find(hero);
    const int episode = it != episodes_.end() ? it->second.count : 0;
    const bool opening = hero == episodeHero_ && clock_ - episodeStart_ < kEpisodeTitle;
    const ImVec4 gold{1.0f, 0.82f, 0.35f, 1.0f};
    if (opening && episode == 1) {
        // Дебют: кто это и как связан с уже знакомыми.
        if (protagonist->introducedBy == kInvalidId) {
            ImGui::TextColored(gold, "Новый герой: %s", idLabel(hero).c_str());
            return;
        }
        ImGui::TextColored(gold, "Новый герой: %s — связан с %s", idLabel(hero).c_str(),
                           idLabel(protagonist->introducedBy).c_str());
        if (protagonist->via != Protagonist::kNoEvent) {
            const ChronicleEvent& event = world_.chronicle().events()[protagonist->via];
            if (const EntityId id = drawChronicleEvent(event, kInvalidId); id != kInvalidId) {
                enabled_ = false;
                inspector.select(id);
            }
        }
        return;
    }
    if (episode > 0 && hero == episodeHero_) {
        ImGui::TextColored(gold, "%s — главный герой, эпизод %d", idLabel(hero).c_str(), episode);
    } else {
        ImGui::TextColored(gold, "%s — главный герой", idLabel(hero).c_str());
    }
    if (opening && returnedAfter_ >= kMissedAfter) {
        ImGui::TextDisabled("возвращаемся к герою спустя %.0f мин", returnedAfter_ / 60.0f);
    }
}

void Director::drawCast(Inspector& inspector) {
    const std::vector<Protagonist>& heroes = cast_.heroes();
    char header[96];
    std::snprintf(header, sizeof(header), "Главные герои: %zu из %zu###cast", cast_.living(),
                  CastTracker::kRoster);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (heroes.empty()) {
        ImGui::TextDisabled("пока никого: героев делают события");
        return;
    }
    EntityId clicked = kInvalidId;
    // Сначала живые, потом выбывшие — и те и другие по порядку вхождения.
    for (const bool living : {true, false}) {
        for (const Protagonist& hero : heroes) {
            if (hero.alive() != living) {
                continue;
            }
            ImGui::PushID(static_cast<int>(hero.id));
            const auto it = episodes_.find(hero.id);
            const int episodes = it != episodes_.end() ? it->second.count : 0;
            std::string label = (living ? "* " : "  ") + idLabel(hero.id);
            if (!living) {
                label += std::string(" — ") + heroStatusTitle(hero.status);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            const bool watched = enabled_ && hero.id == episodeHero_;
            if (ImGui::Selectable(label.c_str(), watched)) {
                if (living && enabled_) {
                    Pick pick;
                    pick.visit = hero.id;
                    pick.target = hero.id;
                    pick.interest = kVisit;
                    show(pick, inspector, Cut::Chosen); // режиссёр продолжает с этого героя
                } else {
                    clicked = hero.id;
                }
            }
            if (!living) {
                ImGui::PopStyleColor();
            }
            if (hero.introducedBy != kInvalidId && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("введён через %s", idLabel(hero.introducedBy).c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("биография %.0f, линий %zu, эпизодов %d", cast_.weight(hero.id),
                                stories_.storiesOf(hero.id).size(), episodes);
            ImGui::PopID();
        }
    }
    if (clicked != kInvalidId) {
        enabled_ = false; // пользователь выбрал своё
        inspector.select(clicked);
    }
}

} // namespace lw::ui
