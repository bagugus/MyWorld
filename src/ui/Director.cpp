#include "ui/Director.h"

#include "render/CameraController.h"
#include "sim/Calendar.h"
#include "sim/Components.h"
#include "sim/SimDriver.h"
#include "sim/TimeScale.h"
#include "sim/Map.h"
#include "sim/World.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"
#include "ui/Inspector.h"
#include "ui/SeriesFrame.h"
#include "ui/StoryText.h"
#include "ui/Ui.h"

#include <imgui.h>
#include <raymath.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
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
// Линия, где столько ничего не происходит, а герой занят мирным делом, —
// тоже затишье, как бы интересна она ни была.
constexpr std::uint64_t kStaleLine = 2 * kTicksPerGameHour;
// Затишье дольше kQuietDelay секунд — время ×kLullSpeed (если пользователь не
// менял скорость последние kUserPace секунд).
constexpr float kQuietDelay = 3.0f;
constexpr int kLullSpeed = 10;
constexpr float kUserPace = 30.0f;
// Сколько секунд подпись «Тем временем…».
constexpr float kCutTitle = 5.0f;
// Операторская работа (12а.5). Замедление: предвестие интереснее
// kSlowInterest, до кульминации меньше kSlowEta секунд симуляции — время x1,
// пока не случится, но не дольше kSlowMax реальных секунд.
constexpr float kSlowEta = 30.0f;
constexpr float kSlowInterest = 0.3f;
constexpr float kSlowMax = 40.0f;
// Ближе этого (метры) двое — уже средний план: видно обоих.
constexpr float kNear = 250.0f;
// Кого из участников уместить в средний план: не дальше этого от героя.
constexpr float kCompany = 300.0f;
// Сколько участников линии брать в кадр.
constexpr std::size_t kCompanyMax = 8;
// Подписи и титры (12а.6). Итог эпизода — сколько секунд на экране; итог
// подводится, если за человеком следили хотя бы kMinWatch секунд (гибель в
// кадре — всегда).
constexpr float kSummaryTime = 9.0f;
constexpr float kMinWatch = 6.0f;
// «Ранее в сериях» — если героя не показывали дольше kRecapAfter секунд;
// держится kRecapTime секунд: kRecapBefore событий до прошлого показа и
// kRecapSince — с тех пор.
constexpr float kRecapAfter = 45.0f;
constexpr float kRecapTime = 14.0f;
constexpr std::size_t kRecapBefore = 2;
constexpr std::size_t kRecapSince = 3;
// Сколько итогов эпизодов в окне.
constexpr std::size_t kSummariesListed = 40;
// Сериал (12а.7): полосы широкого кадра выезжают за kBarsTime секунд; начало —
// проявка из чёрного за kOpeningFade, титры — ещё через kTitlesDelay; имя
// нового героя проявляется за kNameFade.
constexpr float kBarsTime = 0.8f;
constexpr float kOpeningFade = 1.2f;
constexpr float kTitlesDelay = 0.6f;
constexpr float kNameFade = 0.8f;
// Дом, где сидит герой, — точка взгляда на такой высоте, метры.
constexpr float kHouseLookHeight = 3.0f;
// Общий план поселения: какую долю радиуса уместить.
constexpr float kEstablishingShare = 0.6f;

} // namespace

Director::Director(const World& world) : world_(world) {}

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
    slowmo_ = slowmoSpent_ = false;
    camerawork_.reset();
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
        pick.hero = protagonistOf(pick);
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
        pick.hero = hero.id;
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
    if (pick.target != shown_ || cut == Cut::Scene) {
        camerawork_.cut(); // далеко — пролёт
        cinema_.cut();     // в сериале — склейка
    }
    if (!pick.same(current_) || lost_) {
        held_ = 0.0f;
        slowmo_ = slowmoSpent_ = false;
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
        if (!fresh && returnedAfter_ >= kRecapAfter && !series_) {
            // В сериале «ранее в сериях» — в начале эпизода (шоураннер).
            // Ранее в сериях — и что случилось, пока мы были не с ним (12а.6).
            recapHero_ = shown_;
            recapAt_ = clock_;
            recapBefore_ = notableEvents(world_, shown_, 0, episodes.lastTick + 1, kRecapBefore);
            recapSince_ =
                notableEvents(world_, shown_, episodes.lastTick + 1, world_.tick() + 1, kRecapSince);
        }
    }
    episodes.lastSeen = clock_;
    episodes.lastTick = world_.tick();
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
    if (currentInterest_ < kLullInterest) {
        return true;
    }
    const Story* story = stories_.find(current_.story);
    return story != nullptr && !current_.hasOmen && calm(current_.target) &&
           world_.tick() - std::min(world_.tick(), story->updated) > kStaleLine;
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
    if (series_ && timelapse_) {
        wanted = kTimelapseSpeed; // кадр-пауза таймлапсом (12а.9)
    } else if (slowmo_) {
        wanted = 1; // кульминация — в реальном времени
    } else if (!series_ && quietFor_ >= kQuietDelay && userPaced_ >= kUserPace && !driver.paused() &&
               !driver.skipping()) {
        // В сериале затишье не ускоряют, а вырезают скачком (шоураннер).
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
    // Скорость времени — по тому, сколько тиков прошло за кадр (скачок времени
    // шоураннера не считается: это не скорость, а склейка).
    const std::uint64_t tick = world_.tick();
    // Таймлапс кадра-паузы — тоже приём монтажа, а не скорость.
    const bool lapsing = timelapse_ && driver.speed() == kTimelapseSpeed;
    if (dt > 0.0f && !skippedByShow_ && !lapsing) {
        const float rate = static_cast<float>(tick - std::min(tick, lastTick_)) /
                           static_cast<float>(kTicksPerSecond) / dt;
        simRate_ += (rate - simRate_) * std::min(1.0f, dt * kRateSmoothing);
    }
    lastTick_ = tick;
    if (!series_ && (pausedByShow_ || skippedByShow_)) {
        // Сериал кончился посреди титров или скачка: вернуть миру ход.
        if (pausedByShow_) {
            driver.setPaused(wasPaused_);
            pausedByShow_ = false;
        }
        if (skippedByShow_) {
            driver.cancelSkip();
            skippedByShow_ = false;
        }
    }
    sinceChoice_ += dt;
    const bool refresh = sinceChoice_ >= kChoicePeriod || (series_ && show_.wantsFresh());
    if (refresh) {
        sinceChoice_ = 0.0f;
        omens_ = foresee(world_);
    }
    if (enabled_ && !series_ && shown_ != kInvalidId && inspector.selected() != shown_) {
        enabled_ = false; // пользователь выбрал своё — камера его
    }
    if (!enabled_) {
        subject_ = kInvalidId; // выключили — итога не подводим
        pace(driver, dt);
        return;
    }
    held_ += dt;
    slowmoFor_ += dt;
    breather_ = std::max(0.0f, breather_ - dt);
    tension_ += (intensity() - tension_) * std::min(1.0f, dt / kTensionTime);
    trackEpisode();
    trackSubject();
    pace(driver, dt);
    if (series_) {
        updateShow(inspector, driver, dt, refresh);
        return;
    }
    if (!refresh) {
        return;
    }
    const std::vector<Pick> picks = candidates();
    const Refresh state = refreshCurrent(picks, inspector);
    choose(picks, state, inspector);
}

Director::Refresh Director::refreshCurrent(const std::vector<Pick>& picks, Inspector& inspector) {
    Refresh state;
    state.found = std::find_if(picks.begin(), picks.end(), [this](const Pick& pick) { return pick.same(current_); });
    float currentInterest = 0.0f;
    bool currentUrgent = false;
    bool resolved = false; // кульминация только что случилась
    if (state.found != picks.end()) {
        if (current_.hasOmen && !state.found->hasOmen) {
            held_ = 0.0f; // кульминация случилась — досмотреть, что вышло
            resolved = true;
        }
        const bool retarget = state.found->target != current_.target;
        current_ = *state.found;
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
    // Замедление в кульминации (12а.5): срок — в секундах симуляции, чтобы
    // само замедление его не сбивало.
    if (slowmo_ && (!current_.hasOmen || lost_ || slowmoFor_ >= kSlowMax)) {
        slowmo_ = false;
        slowmoSpent_ = true;
    } else if (!slowmo_ && !slowmoSpent_ && current_.hasOmen && !lost_ &&
               current_.omen.eta <= kSlowEta && current_.interest >= kSlowInterest) {
        slowmo_ = true;
        slowmoFor_ = 0.0f;
    }
    currentUrgent = currentUrgent || slowmo_;
    currentInterest_ = currentInterest;
    state.interest = currentInterest;
    state.urgent = currentUrgent;
    return state;
}

void Director::choose(const std::vector<Pick>& picks, const Refresh& state, Inspector& inspector) {
    if (picks.empty()) {
        return;
    }
    const auto found = state.found;
    const float currentInterest = state.interest;
    const bool currentUrgent = state.urgent;
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

void Director::updateShow(Inspector& inspector, SimDriver& driver, float dt, bool refresh) {
    if (!show_.active()) {
        show_.start();
    }
    bool climax = slowmo_ || urgent(current_);
    if (refresh) {
        showPicks_ = candidates();
        climax = refreshCurrent(showPicks_, inspector).urgent;
    }
    ShowInput in;
    in.world = &world_;
    in.stories = &stories_;
    in.cast = &cast_;
    in.omens = &omens_;
    in.picks = &showPicks_;
    in.refreshed = refresh;
    in.current = current_;
    in.lost = lost_;
    in.lostFor = lost_ ? held_ : 0.0f;
    in.currentInterest = currentInterest_;
    in.climax = climax && current_.hasOmen && !lost_;
    in.quiet = quiet();
    in.paused = driver.paused() && !pausedByShow_;
    in.skipping = driver.skipping();
    in.simRate = simRate_;
    const ShowPhase before = show_.phase();
    const ShowOrders orders = show_.update(in, dt);
    if (show_.phase() != before) {
        // В консоль (лог raylib): ход эпизода можно проверить по запуску со снимком.
        TraceLog(LOG_INFO, "SERIES: %s", frame::clean(show_.status()).c_str());
    }
    if (orders.show) {
        if (orders.show->target != kInvalidId) {
            show(*orders.show, inspector, Cut::Scene);
        } else {
            // Показывать некого — общий план поселения.
            current_ = {};
            lost_ = false;
            shown_ = kInvalidId;
            cinema_.cut();
        }
    }
    if (orders.recut) {
        cinema_.cut();
    }
    if (orders.timelapse) {
        timelapse_ = *orders.timelapse;
    }
    if (orders.skip > 0) {
        driver.requestSkip(orders.skip); // скачок времени — порциями промотки
        skippedByShow_ = true;
    } else if (skippedByShow_ && !driver.skipping() && show_.phase() != ShowPhase::Jump) {
        skippedByShow_ = false;
    }
    if (orders.pause) {
        if (*orders.pause && !pausedByShow_) {
            wasPaused_ = driver.paused();
            pausedByShow_ = true;
            driver.setPaused(true); // титры эпизода: мир ждёт
        } else if (!*orders.pause && pausedByShow_) {
            driver.setPaused(wasPaused_);
            pausedByShow_ = false;
        }
    }
}

bool Director::positionOf(EntityId id, float alpha, Vector2& at) const {
    return ui::positionOf(world_, id, alpha, at);
}

ShotPlan Director::planOf(float alpha) const {
    ShotPlan plan;
    if (!current_.valid() || !positionOf(current_.target, alpha, plan.subject)) {
        return plan;
    }
    plan.valid = true;
    plan.size = ShotSize::Close;
    plan.subjectId = current_.target;
    const auto add = [&](EntityId id, float within) {
        Vector2 at{};
        if (id != current_.target && positionOf(id, alpha, at) &&
            Vector2Distance(at, plan.subject) <= within) {
            plan.extra.push_back(at);
        }
    };
    const Story* story = stories_.find(current_.story);
    if (current_.hasOmen && !lost_) {
        const Foresight& omen = current_.omen;
        const Vector2 place{omen.x, omen.y};
        switch (omen.kind) {
        case ForesightKind::Raid:
            // Общий план: орда и поселение, к которому она идёт.
            plan.size = ShotSize::Wide;
            add(omen.other, std::numeric_limits<float>::infinity());
            plan.extra.push_back(place);
            break;
        case ForesightKind::CacheRace:
            plan.size = ShotSize::Medium;
            add(omen.other, std::numeric_limits<float>::infinity());
            plan.extra.push_back(place);
            break;
        case ForesightKind::Revenge:
        case ForesightKind::Theft:
            // Издалека — крупно герой; когда близко — оба в кадре.
            if (omen.distance <= kNear) {
                plan.size = ShotSize::Medium;
                add(omen.other != kInvalidId ? omen.other : omen.focus, kNear * 2.0f);
                plan.extra.push_back(place);
            }
            break;
        case ForesightKind::Turning:
            plan.size = ShotSize::Medium; // укушенный и люди вокруг
            break;
        default:
            break;
        }
    } else if (story != nullptr && current_.visit == kInvalidId) {
        switch (story->kind) {
        case StoryKind::Group:
            plan.size = ShotSize::Medium;
            if (const entt::entity group = world_.find(story->focus);
                group != entt::null && world_.registry().all_of<Group>(group)) {
                for (const EntityId member : world_.registry().get<Group>(group).members) {
                    add(member, kCompany);
                }
            }
            break;
        case StoryKind::Feud:
            plan.size = ShotSize::Medium;
            add(story->hero, kCompany);
            add(story->other, kCompany);
            break;
        case StoryKind::Salvage:
            plan.size = ShotSize::Medium;
            for (std::size_t i = 0; i < story->cast.size() && plan.extra.size() < kCompanyMax; ++i) {
                add(story->cast[i], kCompany);
            }
            break;
        case StoryKind::Infection:
        default:
            break; // крупно: укушенный
        }
    }
    if (series_) {
        // В 3D человека в доме не видно: в кадре весь дом, со стороны двери.
        const entt::registry& registry = world_.registry();
        const entt::entity entity = world_.find(current_.target);
        const auto* person = entity != entt::null ? registry.try_get<Person>(entity) : nullptr;
        const entt::entity home = person != nullptr && person->indoors ? world_.find(person->home) : entt::null;
        if (home != entt::null && registry.all_of<Building>(home)) {
            const Building& building = registry.get<Building>(home);
            const float x0 = static_cast<float>(building.origin.x) * Map::kCellSize;
            const float z0 = static_cast<float>(building.origin.y) * Map::kCellSize;
            const float x1 = x0 + static_cast<float>(building.width) * Map::kCellSize;
            const float z1 = z0 + static_cast<float>(building.height) * Map::kCellSize;
            if (plan.size == ShotSize::Close) {
                plan.size = ShotSize::Medium;
            }
            for (const Vector2 corner : {Vector2{x0, z0}, Vector2{x1, z0}, Vector2{x0, z1}, Vector2{x1, z1}}) {
                plan.extra.push_back(corner);
            }
            const Vector2 door = {Map::cellCenter(building.door.x), Map::cellCenter(building.door.y)};
            const Vector2 center = {(x0 + x1) * 0.5f, (z0 + z1) * 0.5f};
            plan.viewFrom = std::atan2(door.x - center.x, door.y - center.y) * RAD2DEG;
            plan.lookHeight = kHouseLookHeight;
        }
    }
    return plan;
}

ShotPlan Director::establishingPlan() const {
    ShotPlan plan;
    const entt::registry& registry = world_.registry();
    if (!exists(establishing_)) {
        int largest = -1;
        for (const auto [entity, settlement] : registry.view<const Settlement>().each()) {
            if (settlement.radius > largest) {
                largest = settlement.radius;
                establishing_ = registry.get<Id>(entity).value;
            }
        }
    }
    const entt::entity entity = world_.find(establishing_);
    if (entity == entt::null) {
        return plan;
    }
    const Settlement& settlement = registry.get<Settlement>(entity);
    plan.valid = true;
    plan.size = ShotSize::Wide;
    plan.subjectId = establishing_;
    plan.subject = {Map::cellCenter(settlement.center.x), Map::cellCenter(settlement.center.y)};
    const float reach = static_cast<float>(settlement.radius) * Map::kCellSize * kEstablishingShare;
    for (const Vector2 side : {Vector2{-1.0f, 0.0f}, Vector2{1.0f, 0.0f}, Vector2{0.0f, -1.0f}, Vector2{0.0f, 1.0f}}) {
        plan.extra.push_back(Vector2Add(plan.subject, Vector2Scale(side, reach)));
    }
    return plan;
}

void Director::handleHotkeys(bool keyboardFree) {
    if (!keyboardFree) {
        return;
    }
    if (IsKeyPressed(KEY_F11)) {
        requestSeries(!series());
    } else if (series() && IsKeyPressed(KEY_ESCAPE)) {
        requestSeries(false);
    }
}

void Director::enterSeries(CameraController& camera) {
    savedMode_ = camera.mode() == ViewMode::Cinema ? ViewMode::Orbit : camera.mode();
    savedZoom_ = camera.zoom();
    savedEnabled_ = enabled_;
    if (!enabled_) {
        setEnabled(true);
    }
    camera.setMode(ViewMode::Cinema);
    cinema_.reset();
    show_.start(); // эпизод 1
    series_ = true;
    seriesClock_ = 0.0f;
    bars_ = 0.0f;
    if (fullscreenAllowed_ && !IsWindowFullscreen() && !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) {
        ToggleBorderlessWindowed();
        fullscreen_ = true;
    }
    HideCursor();
}

void Director::leaveSeries(CameraController& camera) {
    series_ = false;
    bars_ = 0.0f;
    timelapse_ = false; // скорость пользователя вернёт pace
    show_.stop(); // паузу и промотку шоураннера снимет ближайший update
    if (fullscreen_) {
        ToggleBorderlessWindowed();
        fullscreen_ = false;
    }
    ShowCursor();
    camera.leaveCinema(savedMode_, savedZoom_);
    if (enabled_ != savedEnabled_) {
        setEnabled(savedEnabled_);
    }
}

void Director::updateCamera(CameraController& camera, std::optional<float> heading, float alpha, float dt) {
    if (seriesRequest_) {
        const bool on = *seriesRequest_;
        seriesRequest_.reset();
        if (on && !series_) {
            enterSeries(camera);
        } else if (!on && series_) {
            leaveSeries(camera);
        }
    }
    const bool climax = slowmo_ || urgent(current_);
    if (!series_) {
        if (!enabled_) {
            camerawork_.idle();
            return;
        }
        const ShotPlan plan = planOf(alpha);
        shot_ = plan.size;
        camerawork_.update(camera, plan, climax, dt);
        return;
    }
    // Сериал: кадр ставит только кинокамера.
    seriesClock_ += dt;
    bars_ = std::min(1.0f, bars_ + dt / kBarsTime);
    if (camera.mode() != ViewMode::Cinema) {
        camera.setMode(ViewMode::Cinema);
    }
    // Под вводным титром и на установочном плане — место сцены общим планом
    // (12а.8), дальше — план кадра.
    ShotPlan plan;
    if (show_.interlude()) {
        plan = show_.interludePlan(world_, alpha); // кадр-пауза (12а.9)
    } else if (show_.establishing()) {
        plan = show_.placePlan(world_, alpha);
    }
    if (!plan.valid) {
        plan = planOf(alpha);
    }
    if (!plan.valid) {
        plan = establishingPlan(); // смотреть пока нечего
    }
    shot_ = plan.size;
    cinema_.update(camera, plan, plan.subjectId == shown_ ? heading : std::nullopt, climax, dt);
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
    if (ImGui::Button("Сериал (F11)")) {
        requestSeries(true);
    }
    ImGui::SetItemTooltip("Кино в 3D во весь экран: камеру ведёт режиссёр, окна спрятаны; выход — Esc");
    ImGui::SameLine();
    bool enabled = enabled_;
    if (ImGui::Checkbox("Режиссёр: камера сама ведёт историю", &enabled)) {
        setEnabled(enabled);
    }
    ImGui::Checkbox("Темп: ускорять затишье, замедлять кульминацию", &tempo_);
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
        if (slowmo_) {
            state += ", замедление";
        }
        ImGui::TextDisabled("%s; напряжение %.2f, на кадре %.0f с", state.c_str(), tension_, held_);
        // Операторская работа (12а.5).
        ImGui::TextDisabled("план: %s%s%s", shotSizeTitle(shot_),
                            camerawork_.flying() ? ", пролёт" : "",
                            camerawork_.pushIn() > 1.05f ? ", наезд" : "");
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
    drawSummaries(inspector);

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
                    std::string(omenTitle(omen.kind)) + ": " + omenLine(world_, omen);
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
    if (series_) {
        drawSeriesFrame();
        return;
    }
    if (!enabled_) {
        return;
    }
    drawTitles(inspector);
    if (!current_.valid()) {
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
        } else if (slowmo_ && baseSpeed_ > 1) {
            ImGui::TextColored(muted, "Замедление — кульминация");
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
            ImGui::TextColored(color, "Вот-вот: %s", omenLine(world_, current_.omen).c_str());
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

void Director::trackSubject() {
    const EntityId id = shown_;
    if (subject_ != kInvalidId) {
        Witnessed now;
        if (id != subject_) {
            closeSubject(); // камера ушла
        } else if (subjectAlive_ && !observe(world_, subject_, now)) {
            closeSubject(); // погиб в кадре: итог сразу, камера остаётся на теле
            return;
        } else if (subjectAlive_) {
            subjectWatch_ = now;
            return;
        } else {
            return;
        }
    }
    subject_ = id;
    subjectSince_ = world_.tick();
    subjectShownAt_ = clock_;
    subjectWatch_ = {};
    subjectAlive_ = id != kInvalidId && observe(world_, id, subjectWatch_);
}

void Director::closeSubject() {
    const EntityId who = subject_;
    subject_ = kInvalidId;
    Witnessed now;
    const bool died = subjectAlive_ && !observe(world_, who, now);
    if (who == kInvalidId || (!died && clock_ - subjectShownAt_ < kMinWatch)) {
        return;
    }
    std::string text = episodeSummary(world_, who, subjectSince_, subjectWatch_);
    if (text.empty()) {
        return;
    }
    show_.note(text); // для «ранее в сериях» следующего эпизода
    summaries_.push_back({who, world_.tick(), std::move(text)});
    summaryAt_ = clock_;
}

void Director::drawTitles(Inspector& inspector) {
    const bool summary = !summaries_.empty() && clock_ - summaryAt_ < kSummaryTime;
    const bool recap = recapHero_ == shown_ && shown_ != kInvalidId && clock_ - recapAt_ < kRecapTime &&
                       (!recapBefore_.empty() || !recapSince_.empty());
    const std::string intent = intentLine(world_, shown_);
    if (!summary && !recap && intent.empty()) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y - 36.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowSizeConstraints({0.0f, 0.0f}, {display.x * 0.5f, display.y * 0.5f});
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    EntityId clicked = kInvalidId;
    if (ImGui::Begin("###director-titles", nullptr, kFlags)) {
        const ImVec4 gold{1.0f, 0.82f, 0.35f, 1.0f};
        const ImVec4 muted{0.7f, 0.78f, 0.9f, 1.0f};
        const std::vector<ChronicleEvent>& events = world_.chronicle().events();
        if (summary) {
            // Итог эпизода: от кого ушли (или кто погиб в кадре).
            ImGui::TextColored(muted, "Итог эпизода");
            ImGui::TextColored(gold, "%s", summaries_.back().text.c_str());
            if (recap || !intent.empty()) {
                ImGui::Separator();
            }
        }
        if (recap) {
            const auto lines = [&](const char* title, const std::vector<std::uint32_t>& list) {
                if (list.empty()) {
                    return;
                }
                ImGui::TextColored(muted, "%s", title);
                for (const std::uint32_t index : list) {
                    ImGui::PushID(static_cast<int>(index));
                    if (const EntityId id = drawChronicleEvent(events[index], kInvalidId);
                        id != kInvalidId) {
                        clicked = id;
                    }
                    ImGui::PopID();
                }
            };
            lines("Ранее в сериях:", recapBefore_);
            lines("Пока нас не было:", recapSince_);
            if (!intent.empty()) {
                ImGui::Separator();
            }
        }
        if (!intent.empty()) {
            // Имя крупно, ниже — положение и что он хочет.
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextUnformatted(idLabel(shown_).c_str());
            ImGui::SetWindowFontScale(1.0f);
            if (const std::string status = statusLine(world_, shown_); !status.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", status.c_str());
            }
            ImGui::TextUnformatted(intent.c_str());
        }
    }
    ImGui::End();
    if (clicked != kInvalidId) {
        enabled_ = false; // пользователь выбрал своё
        inspector.select(clicked);
    }
}

void Director::drawSummaries(Inspector& inspector) {
    char header[64];
    std::snprintf(header, sizeof(header), "Итоги эпизодов: %zu###summaries", summaries_.size());
    if (!ImGui::CollapsingHeader(header)) {
        return;
    }
    if (summaries_.empty()) {
        ImGui::TextDisabled("пока ни одного: итог подводится, когда камера уходит от героя");
        return;
    }
    EntityId clicked = kInvalidId;
    // Новые сверху.
    const std::size_t shown = std::min(kSummariesListed, summaries_.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const Summary& summary = summaries_[summaries_.size() - 1 - i];
        ImGui::PushID(static_cast<int>(i));
        char time[48];
        formatGameDuration(time, sizeof(time), world_.tick() - std::min(world_.tick(), summary.tick));
        if (ImGui::Selectable(summary.text.c_str())) {
            clicked = summary.who;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s назад", time);
        }
        ImGui::PopID();
    }
    if (clicked != kInvalidId) {
        enabled_ = false; // пользователь выбрал своё
        inspector.select(clicked);
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

void Director::drawSeriesFrame() {
    using frame::Align;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = frame::scale();
    const float bar = frame::barHeight(bars_);
    const float margin = display.x * 0.07f;
    frame::drawBars(bars_);
    // Начало сериала — проявка из чёрного; склейка — короткая проявка.
    // Сцены (12а.8): затемнение между сценами и чёрные титры эпизода.
    frame::drawBlackout(std::max({cinema_.blackout(), 1.0f - frame::fade(seriesClock_, kOpeningFade),
                                  show_.curtain()}));
    show_.draw(bar); // титры шоураннера — поверх затемнения
    const float titles = frame::fade(seriesClock_ - kTitlesDelay, 1.0f);
    if (titles <= 0.0f) {
        return;
    }
    const bool hidden = show_.hidesTitles();
    const auto tint = [titles](ImVec4 color, float alpha = 1.0f) {
        color.w *= titles * alpha;
        return color;
    };
    const ImVec4 white{0.95f, 0.94f, 0.9f, 1.0f};
    const ImVec4 muted{0.7f, 0.74f, 0.8f, 1.0f};
    const ImVec4 gold{1.0f, 0.82f, 0.42f, 1.0f};
    const ImVec4 amber{1.0f, 0.62f, 0.3f, 1.0f};
    const ImVec4 straw{0.95f, 0.85f, 0.55f, 1.0f};

    // Верхняя полоса: слева — монтаж, по центру — линия, справа — герой
    // сериала; ниже — что вот-вот случится.
    // Монтаж в сериале ведёт шоураннер: перебивка, передышка, будни.
    std::string label = show_.label();
    const float labelAlpha = 1.0f;
    if (label.empty() && slowmo_) {
        label = "Кульминация";
    }
    // Справа — номер эпизода сериала, а в дебют героя — через кого он связан.
    std::string heroLine = show_.episode() > 0 ? "Эпизод " + std::to_string(show_.episode()) : "";
    if (const EntityId hero = protagonistOf(current_); hero != kInvalidId) {
        const Protagonist* protagonist = cast_.find(hero);
        const auto it = episodes_.find(hero);
        const int episode = it != episodes_.end() ? it->second.count : 0;
        const bool opening = hero == episodeHero_ && clock_ - episodeStart_ < kEpisodeTitle;
        if (protagonist != nullptr && opening && episode == 1) {
            heroLine = "Новый герой";
            if (protagonist->introducedBy != kInvalidId) {
                heroLine += " · связан с " + idLabel(protagonist->introducedBy);
            }
        }
    }
    const Story* story = stories_.find(current_.story);
    std::string storyLine;
    if (story != nullptr) {
        storyLine = titleOf(*story) + " · " + stageTitle(story->stage);
    } else if (current_.hasOmen) {
        storyLine = omenTitle(current_.omen.kind);
    }
    const float row = bar * 0.2f;
    if (hidden) {
        // На чёрном и на общих планах — только титры шоураннера и монтаж.
        storyLine.clear();
        if (show_.phase() != ShowPhase::Cutaway && show_.phase() != ShowPhase::Cliffhanger) {
            label.clear();
            heroLine.clear();
        }
    }
    frame::text(italicFont(), 19.0f * scale, margin, row, Align::Left, tint(muted, labelAlpha), label);
    frame::text(titleFont(), 20.0f * scale, display.x * 0.5f, row, Align::Center, tint(white),
                frame::clean(storyLine), display.x * 0.5f);
    frame::text(titleFont(), 17.0f * scale, display.x - margin, row, Align::Right, tint(gold),
                frame::clean(heroLine));
    if (current_.hasOmen && !lost_ && !hidden) {
        const std::string omen = "Вот-вот: " + frame::clean(omenLine(world_, current_.omen));
        frame::text(captionFont(), 18.0f * scale, display.x * 0.5f, row + 30.0f * scale, Align::Center,
                    tint(urgent(current_) ? amber : straw), omen, display.x * 0.7f);
    }

    // Нижняя полоса: имя крупно, рядом — положение, ниже — чего он хочет.
    float y = display.y - bar + bar * 0.14f;
    const float nameAlpha = frame::fade(clock_ - subjectShownAt_, kNameFade);
    const std::string intent = hidden ? std::string() : intentLine(world_, shown_);
    if (!intent.empty()) {
        const std::string name = frame::clean(idLabel(shown_));
        const float nameSize = 32.0f * scale;
        const float height = frame::text(titleFont(), nameSize, margin, y, Align::Left, tint(white, nameAlpha), name);
        if (const std::string status = statusLine(world_, shown_); !status.empty()) {
            const float width = titleFont()->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, name.c_str()).x;
            frame::text(captionFont(), 19.0f * scale, margin + width + 16.0f * scale, y + nameSize * 0.32f,
                        Align::Left, tint(muted, nameAlpha), frame::clean(status));
        }
        y += height + 2.0f * scale;
        frame::text(captionFont(), 21.0f * scale, margin, y, Align::Left, tint(white, nameAlpha),
                    frame::clean(intent), display.x - 2.0f * margin);
    } else if (!current_.valid() && !hidden) {
        if (const entt::entity place = world_.find(establishing_); place != entt::null) {
            const CalendarTime time = calendarAt(world_.tick());
            char when[64];
            std::snprintf(when, sizeof(when), "день %d, %02d:%02d", time.day, time.hour, time.minute);
            const float height = frame::text(titleFont(), 32.0f * scale, margin, y, Align::Left, tint(white),
                                             world_.registry().get<Settlement>(place).name);
            frame::text(captionFont(), 21.0f * scale, margin, y + height + 2.0f * scale, Align::Left,
                        tint(muted), when);
        }
    }

    // Над нижней полосой — субтитры: итог эпизода, «ранее в сериях».
    struct Line {
        ImFont* font;
        float size;
        ImVec4 color;
        std::string text;
    };
    std::vector<Line> lines;
    const float wrap = display.x * 0.6f;
    const ShowPhase phase = show_.phase();
    const bool cards = phase == ShowPhase::Opening || phase == ShowPhase::Closing ||
                       phase == ShowPhase::Cutaway || phase == ShowPhase::Cliffhanger;
    if (!summaries_.empty() && clock_ - summaryAt_ < kSummaryTime && !cards) {
        // В сериале итог подводится сцене (эпизод — у шоураннера).
        const float alpha = frame::fade(clock_ - summaryAt_, 0.5f, kSummaryTime, 1.2f);
        lines.push_back({italicFont(), 18.0f * scale, tint(muted, alpha), "Итог сцены"});
        lines.push_back({titleFont(), 22.0f * scale, tint(gold, alpha), frame::clean(summaries_.back().text)});
    }
    if (recapHero_ == shown_ && shown_ != kInvalidId && clock_ - recapAt_ < kRecapTime) {
        const float alpha = frame::fade(clock_ - recapAt_, 0.6f, kRecapTime, 1.5f);
        const std::vector<ChronicleEvent>& events = world_.chronicle().events();
        const auto add = [&](const char* title, const std::vector<std::uint32_t>& list) {
            if (list.empty()) {
                return;
            }
            lines.push_back({italicFont(), 18.0f * scale, tint(muted, alpha), title});
            for (const std::uint32_t index : list) {
                lines.push_back({captionFont(), 20.0f * scale, tint(white, alpha),
                                 frame::clean(chronicleEventText(events[index], kInvalidId))});
            }
        };
        add("Ранее в сериях", recapBefore_);
        add("Пока нас не было", recapSince_);
    }
    float total = 0.0f;
    for (const Line& line : lines) {
        total += frame::measure(line.font, line.size, line.text, wrap) + 4.0f * scale;
    }
    y = display.y - bar - 20.0f * scale - total;
    for (const Line& line : lines) {
        y += frame::text(line.font, line.size, display.x * 0.5f, y, Align::Center, line.color, line.text, wrap) +
             4.0f * scale;
    }
}

} // namespace lw::ui
