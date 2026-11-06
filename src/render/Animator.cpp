#include "render/Animator.h"

#include "sim/Calendar.h"
#include "sim/Chronicle.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/TimeScale.h"
#include "sim/Weapons.h"
#include "sim/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace lw {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTau = 6.28318531f;
// Быстрее этого (м/с симуляции) существо идёт, медленнее — стоит.
constexpr float kMoving = 0.05f;
// Поворот к цели, радиан в секунду (при ускорении времени — быстрее).
constexpr float kTurnRate = 8.0f;
// Как часто заново смотреть вокруг: стена, вода, тела, угроза.
constexpr float kProbeSeconds = 0.5f;
constexpr std::uint32_t kForgetFrames = 600;
// События летописи свежее этого получают значок эмоции.
constexpr std::uint64_t kEmoteWindow = 30 * kTicksPerSecond;
// Горе после вести о гибели близкого — столько игрового времени.
constexpr std::uint64_t kGriefTicks = 40 * kTicksPerGameMinute;
// Угрозу, замеченную не раньше этого, человек провожает взглядом.
constexpr std::uint64_t kThreatTicks = 5 * kTicksPerSecond;
// Зверь настораживается на то, что заметил недавно.
constexpr std::uint64_t kAlertTicks = 3 * kTicksPerSecond;
// Тело, которое встанет, начинает дёргаться за столько до этого.
constexpr std::uint64_t kTwitchTicks = 30 * kTicksPerSecond;
constexpr float kFallSeconds = 0.8f;
constexpr float kAnimalFallSeconds = 0.6f;
constexpr float kReloadSeconds = 1.6f;
// Подход к стене, воде, телу — метры в секунду и не дальше этого.
constexpr float kOffsetSpeed = 1.2f;
constexpr float kMaxOffset = 2.2f;
// Разговор — на таком расстоянии, объятие — на таком.
constexpr float kTalkDistance = 1.1f;
constexpr float kHugDistance = 0.45f;

// Занятия — что показывает поза. Ключ позы — занятие и вариант: при смене
// ключа поза переходит в новую плавно.
enum class Act : std::uint8_t {
    // Идёт.
    Walk,
    PocketsWalk,
    Run,
    Sneak,
    Patrol,
    Limp,
    BackAway,
    WalkLook, // идёт и оглядывается
    AimWalk,  // идёт, держа цель на мушке
    GuardWalk,
    // Стоит и отдыхает.
    Stand, // вариант — жест: переминается, оглядывается, потягивается…
    Wait,
    SitGround,
    SitKnees,
    SitStump,
    LieBack,
    LeanWall,
    // В комнате (12а.14).
    SitChair,
    SitSofa,
    EatTable,
    TalkSeated,
    BandageSeated,
    Cook,
    // Ест, пьёт, спит.
    EatStand,
    EatSit,
    DrinkBottle,
    ScoopWater,
    SleepBack,
    SleepCurled,
    // Работа.
    Dig,
    Reap,
    Counter,
    Examine,
    Clipboard,
    Lecture,
    PatrolStand,
    // Обыск, сбор, разделка.
    Search, // вариант: роется внизу или открывает шкафы
    Pick,
    PickLow,
    Butcher,
    // Общение.
    Talk, // вариант: говорит, кивает, смеётся, слушает
    Argue,
    Hug,
    Offer,
    // Бой, раны, горе, страх.
    Guard,
    Aim,
    Reload,
    Bandage,
    HoldWound,
    KneelGrief,
    KneelTouch,
    StandGrief,
    Cower,
    LookAround,
    // Тела и зомби.
    Corpse,
    Fall,
    ZombieWalk,
    ZombieChase,
    ZombieStand,
    ZombieFeed,
    ZombieBang,
    ZombieReach,
    Rise,
    // Звери.
    AnimalMove,
    Graze,
    Listen,
    Scratch,
    SitUp,
    Carcass,
};

// Жесты стоящего (Act::Stand, Act::Wait).
enum Gesture : std::uint32_t { kShift, kLook, kStretch, kCrossed, kHips, kScratch, kGestureCount };
// Варианты разговора (Act::Talk).
enum TalkMove : std::uint32_t { kSpeak, kNod, kLaugh, kListen, kTalkCount };
// Позы отдыха (Act::Stand и сидячие — выбор при отдыхе).
enum Rest : std::uint32_t { kRestStand, kRestSit, kRestKnees, kRestStump, kRestLie, kRestWall, kRestCount };

std::uint32_t mix32(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ULL;
    value ^= value >> 33;
    return static_cast<std::uint32_t>(value);
}

float unitOf(std::uint32_t seed, std::uint32_t salt) {
    return static_cast<float>(mix32((static_cast<std::uint64_t>(seed) << 32) ^ salt) & 0xFFFFu) / 65535.0f;
}

int weighted(const float* weights, int count, float u) {
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += std::max(0.0f, weights[i]);
    }
    if (total <= 0.0f) {
        return 0;
    }
    float pick = u * total;
    for (int i = 0; i < count; ++i) {
        pick -= std::max(0.0f, weights[i]);
        if (pick < 0.0f) {
            return i;
        }
    }
    return count - 1;
}

float headingTo(float fromX, float fromY, float toX, float toY) {
    return std::atan2(toX - fromX, toY - fromY);
}

float angleDiff(float from, float to) {
    return std::remainder(to - from, kTau);
}

// Как держит оружие: 0 — кулаки, 1 — нож, 2 — топор (и ружьё без патронов —
// как дубину), 3 — пистолет, 4 — длинное.
int gripOf(const Weapon* weapon) {
    if (weapon == nullptr) {
        return 0;
    }
    switch (weapon->kind) {
    case WeaponKind::Knife:
        return 1;
    case WeaponKind::Axe:
        return 2;
    case WeaponKind::Pistol:
        return weapon->ammo > 0 ? 3 : 0;
    case WeaponKind::Shotgun:
    case WeaponKind::Rifle:
        return weapon->ammo > 0 ? 4 : 2;
    default:
        return 0;
    }
}

bool lying(Act act) {
    return act == Act::LieBack || act == Act::SleepBack || act == Act::SleepCurled || act == Act::Corpse;
}

// Сколько длится переход в новую позу.
float blendLength(Act from, Act to) {
    if (lying(from) != lying(to)) {
        return 1.4f;
    }
    switch (to) {
    case Act::Walk:
    case Act::Run:
    case Act::PocketsWalk:
    case Act::Limp:
    case Act::ZombieWalk:
    case Act::ZombieChase:
    case Act::AnimalMove:
        return 0.25f;
    default:
        return 0.5f;
    }
}

std::optional<std::pair<float, float>> positionOf(const World& world, EntityId id) {
    if (id == kInvalidId) {
        return std::nullopt;
    }
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return std::nullopt;
    }
    const auto* position = world.registry().try_get<Position>(entity);
    if (position == nullptr) {
        return std::nullopt;
    }
    return std::pair{position->x, position->y};
}

// Значок эмоции по событию летописи: кому и какой.
void emotesOf(const ChronicleEvent& event, EntityId& who, Emote& emote) {
    who = event.subject;
    switch (event.kind) {
    case ChronicleKind::Fled:
    case ChronicleKind::Bitten:
    case ChronicleKind::AssaultedBy:
        emote = Emote::Alarm;
        break;
    case ChronicleKind::Befriended:
    case ChronicleKind::Reconciled:
    case ChronicleKind::SavedBy:
    case ChronicleKind::DefendedBy:
    case ChronicleKind::JoinedGroup:
        emote = Emote::Heart;
        break;
    case ChronicleKind::Shared:
        who = event.object;
        emote = Emote::Heart;
        break;
    case ChronicleKind::Disliked:
    case ChronicleKind::BetrayedBy:
    case ChronicleKind::CaughtThief:
    case ChronicleKind::AbandonedBy:
    case ChronicleKind::Avenged:
        emote = Emote::Anger;
        break;
    case ChronicleKind::Expelled:
        who = event.object;
        emote = Emote::Anger;
        break;
    case ChronicleKind::Mourned:
    case ChronicleKind::SawTurned:
        emote = Emote::Tear;
        break;
    case ChronicleKind::LearnedOfEvent:
        emote = Emote::Question;
        break;
    case ChronicleKind::FoundWeapon:
    case ChronicleKind::Salvaged:
    case ChronicleKind::Hunted:
        emote = Emote::Discovery;
        break;
    default:
        emote = Emote::None;
        break;
    }
}

} // namespace

// Что выбрано для существа в этом кадре.
struct Animator::Choice {
    Act act = Act::Stand;
    std::uint32_t variant = 0;
    Prop prop = Prop::None;
    bool bag = false;
    bool aiming = false;
    int grip = 0;
    bool attacks = false; // удар или выстрел поверх позы
    bool face = false;    // повернуться к точке
    float faceX = 0.0f;
    float faceY = 0.0f;
    bool faceMoving = false; // и на ходу (пятится, целится)
    bool look = false;       // голова — к точке
    float lookX = 0.0f;
    float lookY = 0.0f;
    float offX = 0.0f; // куда сдвинуться от позиции симуляции
    float offY = 0.0f;
    float tremble = 0.0f;
    float nervous = 0.0f;
    float twitch = 0.0f;
    int style = 0;
};

// Облик существа между кадрами.
struct Animator::State {
    Figure figure;
    std::uint32_t seen = 0;
    bool fresh = true;
    std::uint32_t seed = 0;
    float lastX = 0.0f;
    float lastY = 0.0f;
    // Ключ позы и когда он сменился.
    Act act = Act::Stand;
    std::uint32_t variant = 0;
    bool keyed = false;
    std::uint32_t starts = 0; // сколько раз менялось занятие: новый выбор варианта
    float since = 0.0f;       // часы: начало занятия
    float variantSince = 0.0f;
    HumanPose from;
    AnimalPose animalFrom;
    float blendAt = -100.0f;
    float blendLength = 0.3f;
    bool via = false; // через сидячую позу: лёжа ↔ стоя
    // Бой и смерть.
    std::uint64_t ready = 0;
    std::uint16_t ammo = 0;
    float attackAt = -100.0f;
    float reloadAt = -100.0f;
    float riseAt = -100.0f;
    float fallAt = -100.0f;
    HumanPose fallFrom;
    float offX = 0.0f;
    float offY = 0.0f;
    // Что вокруг — смотрится раз в kProbeSeconds.
    float probedAt = -100.0f;
    bool threat = false;
    float threatX = 0.0f;
    float threatY = 0.0f;
    EntityId grief = kInvalidId; // по ком горюет
    bool mourning = false;
    bool wall = false;
    Rect wallRect;
    bool water = false;
    float waterX = 0.0f;
    float waterY = 0.0f;
    bool patient = false;
    float patientX = 0.0f;
    float patientY = 0.0f;
};

Animator::Animator() = default;
Animator::~Animator() = default;

void Animator::beginFrame(const World& world, float dt, float alpha) {
    ++frame_;
    tick_ = world.tick();
    const double sim = (static_cast<double>(tick_) + std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
    const bool running = lastSim_ < 0.0 || sim > lastSim_ + 1e-6;
    if (dt > 0.0f && lastSim_ >= 0.0) {
        const float instant = static_cast<float>((sim - lastSim_) / dt);
        rate_ += (std::clamp(instant, 0.0f, 200.0f) - rate_) * std::min(1.0f, dt * 4.0f);
    }
    lastSim_ = sim;
    dt_ = running ? dt : 0.0f;
    clock_ += dt_;
    yearFraction_ = static_cast<float>(calendarAt(tick_).yearFraction);
    if (!indexed_) {
        indexBuildings(world);
    }
    readChronicle(world);
}

void Animator::readChronicle(const World& world) {
    const std::vector<ChronicleEvent>& events = world.chronicle().events();
    if (chronicleRead_ > events.size()) {
        chronicleRead_ = 0; // другой мир
    }
    std::size_t start = chronicleRead_;
    if (events.size() - start > 4096) {
        start = events.size() - 4096; // после промотки — только последние
    }
    for (std::size_t i = start; i < events.size(); ++i) {
        const ChronicleEvent& event = events[i];
        if (event.tick + kEmoteWindow < tick_) {
            continue;
        }
        EntityId who = kInvalidId;
        Emote emote = Emote::None;
        emotesOf(event, who, emote);
        if (emote != Emote::None && who != kInvalidId) {
            marks_[who] = {emote, clock_};
        }
    }
    chronicleRead_ = events.size();
}

void Animator::indexBuildings(const World& world) {
    indexed_ = true;
    const entt::registry& registry = world.registry();
    for (const auto [entity, building, id] : registry.view<const Building, const Id>().each()) {
        for (int y = 0; y < building.height; ++y) {
            for (int x = 0; x < building.width; ++x) {
                const std::int64_t key = (static_cast<std::int64_t>(building.origin.y + y) << 32) |
                                         static_cast<std::uint32_t>(building.origin.x + x);
                buildingCells_[key] = id.value;
            }
        }
    }
}

void Animator::gatherFrame(const World& world) {
    if (gathered_ == frame_) {
        return;
    }
    gathered_ = frame_;
    corpses_.clear();
    occupied_.clear();
    const entt::registry& registry = world.registry();
    for (const auto [entity, corpse, position, id] : registry.view<const Corpse, const Position, const Id>().each()) {
        corpses_.push_back({position.x, position.y, id.value, corpse.kind != CorpseKind::Animal});
    }
    for (const auto [entity, person] : registry.view<const Person>().each()) {
        if (person.indoors && person.home != kInvalidId) {
            occupied_.insert(person.home);
        }
    }
}

std::optional<Animator::Rect> Animator::buildingRect(const World& world, EntityId building) const {
    const entt::entity entity = world.find(building);
    if (entity == entt::null) {
        return std::nullopt;
    }
    const auto* b = world.registry().try_get<Building>(entity);
    if (b == nullptr) {
        return std::nullopt;
    }
    const float x0 = static_cast<float>(b->origin.x) * Map::kCellSize;
    const float y0 = static_cast<float>(b->origin.y) * Map::kCellSize;
    return Rect{x0, y0, x0 + static_cast<float>(b->width) * Map::kCellSize,
                y0 + static_cast<float>(b->height) * Map::kCellSize, building};
}

std::optional<Animator::Rect> Animator::wallNear(const World& world, float x, float y) const {
    const int cx = static_cast<int>(std::floor(x / Map::kCellSize));
    const int cy = static_cast<int>(std::floor(y / Map::kCellSize));
    std::optional<Rect> best;
    float bestDistance = 3.0f;
    EntityId seen[9] = {};
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const std::int64_t key = (static_cast<std::int64_t>(cy + dy) << 32) | static_cast<std::uint32_t>(cx + dx);
            const auto it = buildingCells_.find(key);
            if (it == buildingCells_.end() || std::find(seen, seen + count, it->second) != seen + count) {
                continue;
            }
            seen[count++] = it->second;
            const std::optional<Rect> rect = buildingRect(world, it->second);
            if (!rect) {
                continue;
            }
            const float px = std::clamp(x, rect->x0, rect->x1);
            const float py = std::clamp(y, rect->y0, rect->y1);
            const float d = std::hypot(x - px, y - py);
            if (d > 1e-3f && d < bestDistance) {
                bestDistance = d;
                best = rect;
            }
        }
    }
    return best;
}

namespace {

// Точка на прямоугольнике здания, ближайшая к (x, y), и направление от стены к ней.
struct WallPoint {
    float x = 0.0f;
    float y = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
};

template <typename R>
std::optional<WallPoint> wallPoint(const R& rect, float x, float y) {
    WallPoint p;
    p.x = std::clamp(x, rect.x0, rect.x1);
    p.y = std::clamp(y, rect.y0, rect.y1);
    const float dx = x - p.x;
    const float dy = y - p.y;
    const float d = std::hypot(dx, dy);
    if (d < 1e-3f) {
        return std::nullopt; // внутри здания
    }
    p.nx = dx / d;
    p.ny = dy / d;
    return p;
}

// Поза человека по занятию. t — секунды с начала варианта.
HumanPose humanPose(Act act, std::uint32_t variant, float t, Gait gait, std::uint32_t seed, int grip,
                    float nervous, int style) {
    switch (act) {
    case Act::Walk:
        return pose::walk(gait, seed);
    case Act::PocketsWalk:
        return pose::pocketsWalk(gait);
    case Act::Run:
        return pose::run(gait);
    case Act::Sneak:
        return pose::sneak(gait);
    case Act::Patrol:
        return pose::patrol(gait);
    case Act::Limp:
        return pose::limp(gait);
    case Act::BackAway:
        return pose::backAway(gait);
    case Act::WalkLook: {
        HumanPose p = pose::walk(gait, seed);
        const HumanPose look = pose::lookAround(t, seed, nervous);
        p.headYaw = look.headYaw;
        p.twist += 0.5f * look.twist;
        return p;
    }
    case Act::AimWalk:
    case Act::GuardWalk: {
        HumanPose p = pose::walk({gait.phase, std::min(gait.swing, 0.9f)}, seed);
        const HumanPose upper = act == Act::AimWalk ? pose::aim(t, grip) : pose::guard(t, grip);
        p.leftArm = upper.leftArm;
        p.rightArm = upper.rightArm;
        p.lean = upper.lean;
        p.twist = upper.twist;
        p.headYaw = upper.headYaw;
        p.headTilt = upper.headTilt;
        return p;
    }
    case Act::Stand:
    case Act::Wait:
        switch (variant) {
        case kLook:
            return pose::lookAround(t, seed, nervous);
        case kStretch:
            return pose::stretch(t);
        case kCrossed:
            return pose::armsCrossed(t, seed);
        case kHips:
            return pose::handsOnHips(t, seed);
        case kScratch:
            return pose::scratchHead(t);
        default:
            return pose::stand(t, seed);
        }
    case Act::SitGround:
        return pose::sitGround(t, seed);
    case Act::SitKnees:
        return pose::sitKnees(t, seed);
    case Act::SitStump:
        return pose::sitStump(t, seed);
    case Act::LieBack:
        return pose::lieBack(t, seed);
    case Act::LeanWall:
        return pose::leanWall(t, seed);
    case Act::SitChair:
        return pose::sitChair(t, seed);
    case Act::SitSofa:
        return pose::sitSofa(t, seed);
    case Act::EatTable:
        return pose::eatTable(t);
    case Act::TalkSeated:
        return pose::talkSeated(t, seed);
    case Act::BandageSeated:
        return pose::bandageSeated(t);
    case Act::Cook:
        return pose::counter(t, seed);
    case Act::EatStand:
        return pose::eat(t, false);
    case Act::EatSit:
        return pose::eat(t, true);
    case Act::DrinkBottle:
        return pose::drinkBottle(t);
    case Act::ScoopWater:
        return pose::scoopWater(t);
    case Act::SleepBack:
        return pose::sleepBack(t);
    case Act::SleepCurled:
        return pose::sleepCurled(t);
    case Act::Dig:
        return pose::dig(t);
    case Act::Reap:
        return pose::reap(t);
    case Act::Counter:
        return pose::counter(t, seed);
    case Act::Examine:
        return pose::examine(t);
    case Act::Clipboard:
        return pose::clipboard(t);
    case Act::Lecture:
        return pose::lecture(t, seed);
    case Act::PatrolStand:
        return pose::patrolStand(t, seed);
    case Act::Search:
        return variant == 0 ? pose::rummage(t) : pose::reachHigh(t);
    case Act::Pick:
        return pose::pick(t, false);
    case Act::PickLow:
        return pose::pick(t, true);
    case Act::Butcher:
        return pose::butcher(t);
    case Act::Talk:
        switch (variant) {
        case kNod:
            return pose::nod(t);
        case kLaugh:
            return pose::laugh(t);
        case kListen:
            return pose::listen(t, seed);
        default:
            return pose::talk(t, seed, 0.5f + 0.5f * nervous);
        }
    case Act::Argue:
        return pose::argue(t, seed);
    case Act::Hug:
        return pose::hug(t);
    case Act::Offer:
        return pose::offer(t);
    case Act::Guard:
        return pose::guard(t, grip);
    case Act::Aim:
        return pose::aim(t, grip);
    case Act::Reload:
        return pose::reload(t, grip);
    case Act::Bandage:
        return pose::bandage(t);
    case Act::HoldWound:
        return pose::holdWound(t);
    case Act::KneelGrief:
        return pose::kneelGrief(t, false);
    case Act::KneelTouch:
        return pose::kneelGrief(t, true);
    case Act::StandGrief:
        return pose::standGrief(t);
    case Act::Cower:
        return pose::cower(t);
    case Act::LookAround:
        return pose::lookAround(t, seed, nervous);
    case Act::Corpse:
    case Act::Fall:
        return pose::corpse(seed);
    case Act::ZombieWalk:
        return pose::zombieWalk(gait, style, false);
    case Act::ZombieChase:
        return pose::zombieWalk(gait, style, true);
    case Act::ZombieStand:
        return pose::zombieStand(t, style);
    case Act::ZombieFeed:
        return pose::zombieFeed(t);
    case Act::ZombieBang:
        return pose::zombieBang(t);
    case Act::ZombieReach:
        return pose::zombieReach(t, style);
    default:
        return pose::stand(t, seed);
    }
}

} // namespace

void Animator::chooseHuman(const World& world, entt::entity entity, State& state, float x, float y, float speed,
                           Choice& choice, const Placement* indoor) {
    const entt::registry& registry = world.registry();
    const auto* person = registry.try_get<Person>(entity);
    const auto* mind = registry.try_get<Mind>(entity);
    const auto* needs = registry.try_get<Needs>(entity);
    const auto* traits = registry.try_get<Traits>(entity);
    const auto* wounds = registry.try_get<Wounds>(entity);
    const auto* weapon = registry.try_get<Weapon>(entity);
    const auto* combat = registry.try_get<Combat>(entity);
    const auto* walker = registry.try_get<Walker>(entity);
    const std::uint32_t seed = state.seed;
    const bool moving = speed > kMoving;
    const float fear = needs != nullptr ? needs->fear : 0.0f;
    const float fatigue = needs != nullptr ? needs->fatigue : 0.0f;
    const bool hurt = (wounds != nullptr && wounds->bleeding > 0.02f) || (needs != nullptr && needs->health < 0.35f);
    const Action action = mind != nullptr ? mind->action : Action::Idle;
    const float pace = walker != nullptr ? walker->pace : 1.0f;
    choice.grip = gripOf(weapon);
    if (wounds != nullptr && wounds->infection > 0.5f) {
        choice.tremble = 0.5f; // лихорадка
    } else if (fear > 0.5f) {
        choice.tremble = 0.4f * (fear - 0.5f) / 0.5f;
    }

    // Пробы мира — раз в полсекунды.
    if (clock_ - state.probedAt >= kProbeSeconds || state.fresh) {
        state.probedAt = clock_;
        state.threat = false;
        if (const auto* awareness = registry.try_get<Awareness>(entity)) {
            float best = 1e9f;
            for (std::size_t i = 0; i < awareness->count; ++i) {
                const Contact& contact = awareness->contacts[i];
                if (contact.tick + kThreatTicks < tick_) {
                    continue;
                }
                const entt::entity other = world.find(contact.id);
                if (other == entt::null || !registry.all_of<Zombie>(other)) {
                    continue;
                }
                const float d = std::hypot(contact.x - x, contact.y - y);
                if (d < best) {
                    best = d;
                    state.threat = true;
                    state.threatX = contact.x;
                    state.threatY = contact.y;
                }
            }
        }
        // Горе: весть о гибели близкого совсем недавно.
        state.mourning = false;
        state.grief = kInvalidId;
        const Chronicle& chronicle = world.chronicle();
        const auto own = chronicle.eventsOf(registry.get<Id>(entity).value);
        for (auto it = own.rbegin(); it != own.rend(); ++it) {
            const ChronicleEvent& event = chronicle.events()[*it];
            if (event.tick + kGriefTicks < tick_) {
                break;
            }
            if ((event.kind == ChronicleKind::Mourned || event.kind == ChronicleKind::SawTurned) &&
                event.subject == registry.get<Id>(entity).value) {
                state.mourning = true;
                state.grief = event.kind == ChronicleKind::Mourned ? event.object : kInvalidId;
                break;
            }
        }
        const std::optional<Rect> wall = wallNear(world, x, y);
        state.wall = wall.has_value();
        if (wall) {
            state.wallRect = *wall;
        }
        state.water = false;
        const Map& map = world.map();
        const int cx = static_cast<int>(std::floor(x / Map::kCellSize));
        const int cy = static_cast<int>(std::floor(y / Map::kCellSize));
        float bestWater = 1e9f;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const Cell cell{cx + dx, cy + dy};
                if (!map.contains(cell) || map.water.at(cell.x, cell.y) == Water::None) {
                    continue;
                }
                const float wx = Map::cellCenter(cell.x);
                const float wy = Map::cellCenter(cell.y);
                const float d = std::hypot(wx - x, wy - y);
                if (d < bestWater) {
                    bestWater = d;
                    state.water = true;
                    state.waterX = wx;
                    state.waterY = wy;
                }
            }
        }
        // Врач на смене: пациент рядом.
        state.patient = false;
        if (action == Action::Work) {
            if (const auto* awareness = registry.try_get<Awareness>(entity)) {
                for (std::size_t i = 0; i < awareness->count; ++i) {
                    const Contact& contact = awareness->contacts[i];
                    const entt::entity other = world.find(contact.id);
                    if (other == entt::null || std::hypot(contact.x - x, contact.y - y) > 5.0f) {
                        continue;
                    }
                    const auto* otherMind = registry.try_get<Mind>(other);
                    if (otherMind != nullptr && otherMind->action == Action::SeeDoctor) {
                        state.patient = true;
                        state.patientX = contact.x;
                        state.patientY = contact.y;
                        break;
                    }
                }
            }
        }
    }

    const auto faceTo = [&choice](float fx, float fy) {
        choice.face = true;
        choice.faceX = fx;
        choice.faceY = fy;
        choice.look = true;
        choice.lookX = fx;
        choice.lookY = fy;
    };
    // Встать на расстоянии distance от точки (x, y) другого человека: сдвиг на половину
    // разницы — второй сделает вторую половину.
    const auto approach = [&](float ox, float oy, float distance, float share) {
        const float dx = ox - x;
        const float dy = oy - y;
        const float d = std::hypot(dx, dy);
        if (d > 1e-3f) {
            const float move = std::max(0.0f, (d - distance) * share);
            choice.offX = dx / d * move;
            choice.offY = dy / d * move;
        }
    };
    // К стене здания: спиной (lean) или лицом.
    const auto toWall = [&](const auto& rect, bool backToWall) {
        if (const std::optional<WallPoint> p = wallPoint(rect, x, y)) {
            const float gap = backToWall ? 0.28f : 0.45f;
            choice.offX = p->x + p->nx * gap - x;
            choice.offY = p->y + p->ny * gap - y;
            if (backToWall) {
                choice.face = true;
                choice.faceX = x + p->nx * 10.0f;
                choice.faceY = y + p->ny * 10.0f;
            } else {
                faceTo(p->x, p->y);
            }
            return true;
        }
        return false;
    };
    const auto nearestCorpse = [&](bool human, float radius, EntityId only) -> const Spot* {
        gatherFrame(world);
        const Spot* best = nullptr;
        float bestDistance = radius;
        for (const Spot& spot : corpses_) {
            if (spot.human != human || (only != kInvalidId && spot.id != only)) {
                continue;
            }
            const float d = std::hypot(spot.x - x, spot.y - y);
            if (d < bestDistance) {
                bestDistance = d;
                best = &spot;
            }
        }
        return best;
    };
    // Вариант, сменяющийся раз в period секунд (свой сдвиг у каждого).
    const auto epochOf = [&](float period, std::uint32_t salt) {
        const float length = period * (1.0f + 0.6f * unitOf(seed, salt));
        return static_cast<std::uint32_t>(std::floor((clock_ + length * unitOf(seed, salt + 1)) / length));
    };
    const auto choiceUnit = [&](std::uint32_t epoch, std::uint32_t salt) {
        return unitOf(seed ^ (state.starts * 0x632BE5ABu), epoch * 0x9E3779B9u + salt);
    };

    const float bravery = traits != nullptr ? traits->bravery : 0.5f;
    const float curiosity = traits != nullptr ? traits->curiosity : 0.5f;
    const float sociability = traits != nullptr ? traits->sociability : 0.5f;
    // Жест стоящего: переминается, оглядывается, потягивается, скрестил руки…
    const auto gesture = [&](bool waiting) {
        const std::uint32_t epoch = epochOf(4.5f, 50);
        const float weights[kGestureCount] = {
            4.0f,                                                    // переминается
            1.2f + 1.5f * curiosity + 1.5f * (1.0f - bravery) + (waiting ? 1.5f : 0.0f), // оглядывается
            0.4f + 1.2f * fatigue,                                   // потягивается
            1.4f + (waiting ? 1.0f : 0.0f),                          // руки скрещены
            0.7f,                                                    // руки в боки
            0.35f,                                                   // чешет затылок
        };
        return static_cast<std::uint32_t>(weighted(weights, kGestureCount, choiceUnit(epoch, 51)));
    };

    // --- Дома, в комнате (12а.14): поза — по месту ---
    if (indoor != nullptr) {
        if ((person != nullptr && person->asleep) || indoor->what == Occupation::Sleep) {
            choice.act = unitOf(seed ^ state.starts, 60) < 0.5f ? Act::SleepBack : Act::SleepCurled;
            return;
        }
        const bool seated = indoor->kind == SpotKind::Seat || indoor->kind == SpotKind::Desk;
        if (state.mourning) {
            choice.act = seated || indoor->kind == SpotKind::Sofa ? Act::SitChair : Act::StandGrief;
            return;
        }
        if (fear > 0.8f && !seated && indoor->kind != SpotKind::Sofa) {
            choice.act = Act::Cower;
            return;
        }
        switch (indoor->kind) {
        case SpotKind::Bed:
            choice.act = Act::LieBack;
            break;
        case SpotKind::Seat:
        case SpotKind::Desk:
            if (action == Action::Eat || action == Action::Drink) {
                choice.act = Act::EatTable;
                choice.prop = action == Action::Drink ? Prop::Bottle : Prop::Food;
            } else if (action == Action::Treat) {
                choice.act = Act::BandageSeated;
                choice.prop = Prop::Bandage;
            } else if (indoor->meal || action == Action::Socialize) {
                // За общим столом: то говорит, то слушает.
                choice.act = choiceUnit(epochOf(6.0f, 110), 111) < 0.45f ? Act::TalkSeated : Act::SitChair;
            } else {
                choice.act = Act::SitChair;
            }
            break;
        case SpotKind::Sofa:
            choice.act = Act::SitSofa;
            break;
        case SpotKind::Stove:
            choice.act = Act::Cook;
            break;
        default:
            if (action == Action::Socialize || action == Action::Share || action == Action::Trade) {
                choice.act = Act::Talk;
                choice.variant = static_cast<std::uint32_t>(
                    weighted(std::array<float, kTalkCount>{3.0f, 1.0f, 0.6f + sociability, 2.0f}.data(), kTalkCount,
                             choiceUnit(epochOf(3.5f, 112), 113)));
            } else {
                choice.act = Act::Stand;
                choice.variant = gesture(false);
            }
            break;
        }
        return;
    }

    // --- Сон ---
    if (person != nullptr && person->asleep) {
        choice.act = unitOf(seed ^ state.starts, 60) < 0.5f ? Act::SleepBack : Act::SleepCurled;
        return;
    }

    // --- Бой ---
    std::optional<std::pair<float, float>> target;
    if (combat != nullptr && combat->target != kInvalidId) {
        target = positionOf(world, combat->target);
    }
    if (target) {
        faceTo(target->first, target->second);
        choice.faceMoving = true;
        choice.attacks = true;
        const bool firearm = choice.grip >= 3;
        choice.aiming = firearm || (weapon != nullptr && isFirearm(weapon->kind));
        if (firearm) {
            choice.act = moving ? Act::AimWalk : Act::Aim;
        } else if (clock_ - state.reloadAt < kReloadSeconds) {
            choice.act = Act::Reload;
            choice.attacks = false;
        } else if (moving && pace > 1.3f) {
            choice.act = Act::Run;
            choice.faceMoving = false;
        } else {
            choice.act = moving ? Act::GuardWalk : Act::Guard;
        }
        return;
    }

    // --- В пути ---
    if (moving) {
        if (action == Action::Flee) {
            if (pace > 1.3f || !state.threat) {
                choice.act = Act::Run;
            } else {
                choice.act = Act::BackAway;
                faceTo(state.threatX, state.threatY);
                choice.faceMoving = true;
            }
            return;
        }
        if (pace > 1.3f) {
            choice.act = Act::Run;
        } else if (action == Action::Hunt && pace < 0.95f) {
            choice.act = Act::Sneak;
            choice.aiming = weapon != nullptr && isFirearm(weapon->kind) && weapon->ammo > 0;
        } else if (action == Action::Work && registry.all_of<Job>(entity) &&
                   registry.get<Job>(entity).profession == Profession::PoliceOfficer) {
            choice.act = Act::Patrol;
        } else if (hurt) {
            choice.act = Act::Limp;
        } else if (fear > 0.55f) {
            choice.act = Act::WalkLook;
            choice.nervous = fear;
        } else if (action == Action::Wander && (seed & 3u) == 0u) {
            choice.act = Act::PocketsWalk;
        } else {
            choice.act = Act::Walk;
        }
        choice.bag = action == Action::Forage;
        if (state.threat && fear > 0.3f) {
            choice.look = true;
            choice.lookX = state.threatX;
            choice.lookY = state.threatY;
        }
        return;
    }

    // --- На месте ---
    if (state.threat && fear > 0.3f) {
        choice.look = true;
        choice.lookX = state.threatX;
        choice.lookY = state.threatY;
    }
    if (state.mourning && action != Action::Flee && action != Action::Fight) {
        if (const Spot* body = nearestCorpse(true, 8.0f, state.grief); body != nullptr && state.grief != kInvalidId) {
            choice.act = Act::KneelTouch;
            faceTo(body->x, body->y);
            approach(body->x, body->y, 0.8f, 1.0f);
        } else {
            choice.act = Act::StandGrief;
        }
        return;
    }
    if (mind != nullptr && mind->phase == ActionPhase::Travel) {
        choice.act = Act::Wait;
        choice.variant = gesture(true);
        choice.nervous = fear;
        return;
    }
    const auto mindTarget = [&]() {
        return mind != nullptr ? positionOf(world, mind->target) : std::nullopt;
    };
    const auto rest = [&](bool standingOnly) {
        if (fear > 0.8f) {
            choice.act = Act::Cower;
            return;
        }
        if (fear > 0.5f) {
            choice.act = Act::LookAround;
            choice.nervous = fear;
            return;
        }
        if (hurt) {
            choice.act = Act::HoldWound;
            return;
        }
        // Поза отдыха держится полминуты и дольше, жесты стоящего — несколько секунд.
        const std::uint32_t epoch = static_cast<std::uint32_t>(
            std::floor((clock_ - state.since) / (25.0f + 20.0f * unitOf(seed, 70))));
        float weights[kRestCount] = {
            3.0f,                                            // стоит
            standingOnly ? 0.0f : 1.0f + 2.0f * fatigue,     // сидит на земле
            standingOnly ? 0.0f : 0.8f,                      // обхватив колени
            standingOnly ? 0.0f : 0.8f,                      // на чурбаке
            standingOnly || fear > 0.3f ? 0.0f : 0.3f + 2.5f * fatigue, // лёг
            state.wall ? 3.0f : 0.0f,                        // прислонился к стене
        };
        switch (weighted(weights, kRestCount, choiceUnit(epoch, 71))) {
        case kRestSit:
            choice.act = Act::SitGround;
            break;
        case kRestKnees:
            choice.act = Act::SitKnees;
            break;
        case kRestStump:
            choice.act = Act::SitStump;
            choice.prop = Prop::Stump;
            break;
        case kRestLie:
            choice.act = Act::LieBack;
            break;
        case kRestWall:
            choice.act = Act::LeanWall;
            toWall(state.wallRect, true);
            break;
        default:
            choice.act = Act::Stand;
            choice.variant = gesture(false);
            break;
        }
    };

    switch (action) {
    case Action::Eat: {
        const bool sit = unitOf(seed ^ state.starts, 80) < 0.35f + 0.5f * fatigue;
        choice.act = sit ? Act::EatSit : Act::EatStand;
        choice.prop = Prop::Food;
        break;
    }
    case Action::Drink:
        if (state.water) {
            choice.act = Act::ScoopWater;
            faceTo(state.waterX, state.waterY);
            const float d = std::hypot(state.waterX - x, state.waterY - y);
            if (d > 1e-3f) {
                const float move = std::clamp(d - 2.2f, 0.0f, 1.5f);
                choice.offX = (state.waterX - x) / d * move;
                choice.offY = (state.waterY - y) / d * move;
            }
        } else {
            choice.act = Act::DrinkBottle;
            choice.prop = Prop::Bottle;
        }
        break;
    case Action::Sleep:
        choice.act = Act::SitGround;
        break;
    case Action::Scavenge:
    case Action::Steal: {
        choice.act = Act::Search;
        choice.variant = static_cast<std::uint32_t>(choiceUnit(epochOf(5.0f, 90), 91) < 0.5f ? 0 : 1);
        std::optional<Rect> rect;
        if (mind != nullptr) {
            rect = buildingRect(world, mind->target);
        }
        if (rect) {
            toWall(*rect, false);
        } else if (state.wall) {
            toWall(state.wallRect, false);
        }
        break;
    }
    case Action::Salvage:
        choice.act = Act::Search;
        choice.variant = 0;
        break;
    case Action::Forage:
        choice.act = unitOf(seed ^ state.starts, 100) < 0.5f ? Act::Pick : Act::PickLow;
        choice.bag = true;
        break;
    case Action::Socialize:
    case Action::Avenge: {
        const auto other = mindTarget();
        const auto* relations = registry.try_get<Relations>(entity);
        const Bond* bond = relations != nullptr && mind != nullptr ? relations->find(mind->target) : nullptr;
        const bool hostile =
            action == Action::Avenge || (bond != nullptr && (bond->hostile() || bond->grudge > 0.35f));
        if (hostile) {
            choice.act = Act::Argue;
        } else {
            const std::uint32_t epoch = epochOf(4.0f, 110);
            const float affinity = bond != nullptr ? bond->affinity : 0.0f;
            const bool close = other && std::hypot(other->first - x, other->second - y) < 2.0f;
            if (close && affinity > 0.7f && choiceUnit(epoch, 111) < 0.12f) {
                choice.act = Act::Hug;
            } else {
                choice.act = Act::Talk;
                const float weights[kTalkCount] = {
                    0.5f + 3.0f * sociability,                       // говорит
                    1.0f,                                            // кивает
                    affinity > 0.2f ? 0.4f + 1.5f * sociability : 0.0f, // смеётся
                    0.5f + 2.0f * (1.0f - sociability),              // слушает
                };
                choice.variant = static_cast<std::uint32_t>(weighted(weights, kTalkCount, choiceUnit(epoch, 112)));
                choice.nervous = traits != nullptr ? traits->aggression : 0.5f;
            }
        }
        if (other) {
            faceTo(other->first, other->second);
            approach(other->first, other->second, choice.act == Act::Hug ? kHugDistance : kTalkDistance, 0.5f);
        } else if (state.threat) {
            faceTo(state.threatX, state.threatY);
        }
        break;
    }
    case Action::Share:
    case Action::Trade: {
        const auto other = mindTarget();
        if (other && std::hypot(other->first - x, other->second - y) < 4.0f) {
            choice.act = Act::Offer;
            choice.prop = Prop::Bundle;
            faceTo(other->first, other->second);
            approach(other->first, other->second, kTalkDistance, 0.5f);
        } else {
            choice.act = Act::Wait;
            choice.variant = gesture(true);
        }
        break;
    }
    case Action::Flee:
        choice.act = Act::Cower;
        break;
    case Action::Hunt:
        if (const Spot* carcass = nearestCorpse(false, 3.0f, kInvalidId)) {
            choice.act = Act::Butcher;
            faceTo(carcass->x, carcass->y);
            approach(carcass->x, carcass->y, 0.9f, 1.0f);
        } else {
            choice.act = Act::LookAround;
            choice.nervous = 0.0f;
        }
        break;
    case Action::Treat:
        choice.act = Act::Bandage;
        choice.prop = Prop::Bandage;
        break;
    case Action::Work: {
        const Profession profession =
            registry.all_of<Job>(entity) ? registry.get<Job>(entity).profession : Profession::None;
        std::optional<Rect> place;
        if (registry.all_of<Job>(entity)) {
            place = buildingRect(world, registry.get<Job>(entity).workplace);
        }
        switch (profession) {
        case Profession::Farmer: {
            // Весной и зимой копает, летом то копает, то косит, осенью косит.
            const float year = yearFraction_;
            const bool summer = year >= 0.25f && year < 0.5f;
            const bool autumn = year >= 0.5f && year < 0.75f;
            const bool reap = autumn || (summer && choiceUnit(epochOf(30.0f, 120), 121) < 0.5f);
            choice.act = reap ? Act::Reap : Act::Dig;
            choice.prop = reap ? Prop::Scythe : Prop::Shovel;
            break;
        }
        case Profession::Shopkeeper:
            choice.act = Act::Counter;
            choice.prop = Prop::Counter;
            if (place) {
                // Лицом на улицу, спиной к лавке.
                toWall(*place, true);
                choice.offX *= 0.3f;
                choice.offY *= 0.3f;
            }
            break;
        case Profession::Doctor:
            if (state.patient) {
                choice.act = Act::Examine;
                faceTo(state.patientX, state.patientY);
                approach(state.patientX, state.patientY, 0.8f, 0.5f);
            } else {
                choice.act = Act::Clipboard;
                choice.prop = Prop::Clipboard;
            }
            break;
        case Profession::PoliceOfficer:
            choice.act = Act::PatrolStand;
            break;
        case Profession::Teacher:
            choice.act = Act::Lecture;
            break;
        default:
            rest(true);
            break;
        }
        break;
    }
    case Action::SeeDoctor:
        if (hurt) {
            choice.act = Act::HoldWound;
        } else {
            choice.act = Act::Wait;
            choice.variant = gesture(true);
        }
        break;
    case Action::Wander:
    case Action::GoHome:
    case Action::Buy:
        rest(true);
        break;
    default:
        rest(false);
        break;
    }
}

void Animator::chooseZombie(const World& world, entt::entity entity, State& state, float x, float y, float speed,
                            Choice& choice) {
    const entt::registry& registry = world.registry();
    const auto& zombie = registry.get<Zombie>(entity);
    choice.style = static_cast<int>(state.seed % 4u);
    choice.attacks = true;
    if (clock_ - state.riseAt < pose::kRiseSeconds) {
        choice.act = Act::Rise;
        choice.attacks = false;
        return;
    }
    if (speed > kMoving) {
        choice.act = zombie.state == ZombieState::Chase ? Act::ZombieChase : Act::ZombieWalk;
        return;
    }
    if (const auto* combat = registry.try_get<Combat>(entity); combat != nullptr && combat->target != kInvalidId) {
        if (const auto target = positionOf(world, combat->target)) {
            choice.act = Act::ZombieReach;
            choice.face = true;
            choice.faceX = target->first;
            choice.faceY = target->second;
            return;
        }
    }
    // Ест тело рядом.
    gatherFrame(world);
    const Spot* body = nullptr;
    float bestDistance = 2.0f;
    for (const Spot& spot : corpses_) {
        const float d = std::hypot(spot.x - x, spot.y - y);
        if (spot.human && d < bestDistance && d > 1e-3f) {
            bestDistance = d;
            body = &spot;
        }
    }
    if (body != nullptr) {
        choice.act = Act::ZombieFeed;
        choice.face = true;
        choice.faceX = body->x;
        choice.faceY = body->y;
        const float move = std::max(0.0f, bestDistance - 0.75f);
        choice.offX = (body->x - x) / bestDistance * move;
        choice.offY = (body->y - y) / bestDistance * move;
        return;
    }
    // Ломится в дом, где прячутся люди.
    if (clock_ - state.probedAt >= kProbeSeconds || state.fresh) {
        state.probedAt = clock_;
        state.wall = false;
        if (const std::optional<Rect> wall = wallNear(world, x, y); wall && occupied_.contains(wall->id)) {
            state.wall = true;
            state.wallRect = *wall;
        }
    }
    if (state.wall && zombie.state != ZombieState::Roam && zombie.state != ZombieState::Follow) {
        if (const std::optional<WallPoint> p = wallPoint(state.wallRect, x, y)) {
            choice.act = Act::ZombieBang;
            choice.face = true;
            choice.faceX = p->x;
            choice.faceY = p->y;
            choice.offX = p->x + p->nx * 0.45f - x;
            choice.offY = p->y + p->ny * 0.45f - y;
            return;
        }
    }
    choice.act = Act::ZombieStand;
}

void Animator::chooseAnimal(const World& world, entt::entity entity, State& state, float /*x*/, float /*y*/,
                            float speed, Choice& choice) {
    const entt::registry& registry = world.registry();
    const auto& animal = registry.get<Animal>(entity);
    if (speed > kMoving) {
        choice.act = Act::AnimalMove;
        choice.variant = animal.state == AnimalState::Charge ? 1u : 0u;
        return;
    }
    if (clock_ - state.probedAt >= kProbeSeconds || state.fresh) {
        state.probedAt = clock_;
        state.threat = false;
        if (const auto* awareness = registry.try_get<Awareness>(entity)) {
            for (std::size_t i = 0; i < awareness->count; ++i) {
                const Contact& contact = awareness->contacts[i];
                if (contact.tick + kAlertTicks >= tick_) {
                    state.threat = true;
                    state.threatX = contact.x;
                    state.threatY = contact.y;
                    break;
                }
            }
        }
    }
    if (state.threat) {
        choice.act = animal.kind == AnimalKind::Hare ? Act::SitUp : Act::Listen;
        choice.look = true;
        choice.lookX = state.threatX;
        choice.lookY = state.threatY;
        return;
    }
    const float length = 5.0f + 4.0f * unitOf(state.seed, 130);
    const auto epoch =
        static_cast<std::uint32_t>(std::floor((clock_ + length * unitOf(state.seed, 131)) / length));
    const float weights[] = {6.0f, 1.5f, 1.2f, animal.kind == AnimalKind::Hare ? 1.5f : 0.0f};
    switch (weighted(weights, 4, unitOf(state.seed ^ epoch, 132))) {
    case 1:
        choice.act = Act::Listen;
        choice.variant = epoch;
        break;
    case 2:
        choice.act = Act::Scratch;
        break;
    case 3:
        choice.act = Act::SitUp;
        break;
    default:
        choice.act = Act::Graze;
        break;
    }
}

const Figure& Animator::animate(const World& world, entt::entity entity, EntityId id, float x, float y, float vx,
                                float vy, const Placement* indoor) {
    auto [it, inserted] = states_.try_emplace(id);
    if (inserted) {
        it->second = std::make_unique<State>();
    }
    State& state = *it->second;
    if (!state.fresh && state.seen == frame_) {
        return state.figure;
    }
    const entt::registry& registry = world.registry();
    Figure& figure = state.figure;

    // Кто это сейчас и кем был в прошлом кадре.
    Figure::Body body = Figure::Body::Human;
    const auto* corpse = registry.try_get<Corpse>(entity);
    if (corpse != nullptr) {
        body = corpse->kind == CorpseKind::Animal ? Figure::Body::AnimalCorpse : Figure::Body::HumanCorpse;
    } else if (registry.all_of<Zombie>(entity)) {
        body = Figure::Body::Zombie;
    } else if (registry.all_of<Animal>(entity)) {
        body = Figure::Body::Animal;
    }
    if (state.fresh) {
        state.seed = mix32(id);
        figure.heading = static_cast<float>(state.seed % 6283u) / 1000.0f;
        figure.gait.phase = static_cast<float>((state.seed >> 12) % 6283u) / 1000.0f;
        state.lastX = x;
        state.lastY = y;
        if (const auto* combat = registry.try_get<Combat>(entity)) {
            state.ready = combat->ready;
        }
        if (const auto* weapon = registry.try_get<Weapon>(entity)) {
            state.ammo = weapon->ammo;
        }
    } else if (body != figure.body) {
        const bool wasAlive = figure.body == Figure::Body::Human || figure.body == Figure::Body::Zombie;
        if (wasAlive && body == Figure::Body::HumanCorpse) {
            state.fallAt = clock_;
            state.fallFrom = figure.human;
        } else if (figure.body == Figure::Body::HumanCorpse && body == Figure::Body::Zombie) {
            state.riseAt = clock_;
        } else if (figure.body == Figure::Body::Animal && body == Figure::Body::AnimalCorpse) {
            state.fallAt = clock_;
        }
    }
    figure.body = body;

    // Удары и выстрелы: Combat::ready сдвигается вперёд при каждом.
    if (const auto* combat = registry.try_get<Combat>(entity)) {
        if (!state.fresh && combat->ready > state.ready) {
            state.attackAt = clock_;
        }
        state.ready = combat->ready;
    }
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        if (!state.fresh && isFirearm(weapon->kind) && state.ammo > 0 && weapon->ammo == 0) {
            state.reloadAt = clock_; // кончились патроны — шарит по карманам
        }
        state.ammo = weapon->ammo;
    }

    // Шаг: фаза растёт с пройденным на экране путём — ноги не скользят ни на
    // какой скорости времени.
    const float speed = std::hypot(vx, vy);
    float stride = 1.6f;
    if (const auto* animal = registry.try_get<Animal>(entity)) {
        stride = animal->kind == AnimalKind::Deer ? 2.2f : animal->kind == AnimalKind::Boar ? 1.4f : 0.9f;
    }
    const float moved = std::hypot(x - state.lastX, y - state.lastY);
    if (!state.fresh && moved < 6.0f && speed > kMoving) {
        figure.gait.phase = std::fmod(figure.gait.phase + kTau * moved / stride, 2.0f * kTau);
    }
    figure.gait.swing = std::clamp(speed / (0.9f * stride), 0.0f, 1.3f);
    state.lastX = x;
    state.lastY = y;

    Choice choice;
    switch (body) {
    case Figure::Body::Human:
        chooseHuman(world, entity, state, x, y, speed, choice, indoor);
        break;
    case Figure::Body::Zombie:
        chooseZombie(world, entity, state, x, y, speed, choice);
        break;
    case Figure::Body::Animal:
        chooseAnimal(world, entity, state, x, y, speed, choice);
        break;
    case Figure::Body::HumanCorpse:
        choice.act = clock_ - state.fallAt < kFallSeconds ? Act::Fall : Act::Corpse;
        if (corpse->turnsAt != 0 && corpse->turnsAt > tick_ && corpse->turnsAt - tick_ < kTwitchTicks) {
            choice.twitch = 1.0f - static_cast<float>(corpse->turnsAt - tick_) / static_cast<float>(kTwitchTicks);
        }
        break;
    case Figure::Body::AnimalCorpse:
        choice.act = Act::Carcass;
        break;
    }

    // Поворот: на ходу — по движению (или к цели, если пятится и целится), на месте — к цели.
    const float turn = kTurnRate * dt_ * std::clamp(rate_, 1.0f, 20.0f);
    const auto turnTo = [&](float wanted) {
        figure.heading += std::clamp(angleDiff(figure.heading, wanted), -turn, turn);
        figure.heading = std::remainder(figure.heading, kTau);
    };
    const bool corpseBody = body == Figure::Body::HumanCorpse || body == Figure::Body::AnimalCorpse;
    if (indoor != nullptr) {
        figure.heading = indoor->heading; // место в комнате задаёт, куда смотреть
    } else if (!corpseBody && choice.act != Act::Rise) {
        if (speed > kMoving && !(choice.face && choice.faceMoving)) {
            turnTo(std::atan2(vx, vy));
        } else if (choice.face) {
            turnTo(headingTo(x, y, choice.faceX, choice.faceY));
        }
    }

    // Сдвиг к стене, воде, телу — шагом; на ходу — сразу к позиции симуляции.
    {
        float tx = choice.offX;
        float ty = choice.offY;
        const float length = std::hypot(tx, ty);
        if (length > kMaxOffset) {
            tx *= kMaxOffset / length;
            ty *= kMaxOffset / length;
        }
        const float step = (speed > kMoving ? 3.0f : kOffsetSpeed) * dt_ * std::clamp(rate_, 1.0f, 20.0f);
        const float dx = tx - state.offX;
        const float dy = ty - state.offY;
        const float d = std::hypot(dx, dy);
        if (d <= step || d < 1e-4f) {
            state.offX = tx;
            state.offY = ty;
        } else {
            state.offX += dx / d * step;
            state.offY += dy / d * step;
        }
        if (indoor != nullptr) {
            state.offX = 0.0f; // в комнате — ровно на своём месте
            state.offY = 0.0f;
        }
        figure.offsetX = state.offX;
        figure.offsetY = state.offY;
    }

    // Ключ позы сменился — переход из того, что было на экране.
    if (!state.keyed || choice.act != state.act || choice.variant != state.variant) {
        const bool newAct = !state.keyed || choice.act != state.act;
        if (state.keyed) {
            state.from = figure.human;
            state.animalFrom = figure.animal;
            state.blendAt = clock_;
            state.blendLength = blendLength(state.act, choice.act);
            state.via = lying(state.act) != lying(choice.act) && choice.act != Act::Fall && state.act != Act::Rise &&
                        body != Figure::Body::HumanCorpse;
        }
        if (newAct) {
            ++state.starts;
            state.since = clock_;
        }
        state.act = choice.act;
        state.variant = choice.variant;
        state.variantSince = clock_;
        state.keyed = true;
    }
    const float t = clock_ - state.variantSince;
    const float blendT = state.fresh ? 1.0f : pose::smooth((clock_ - state.blendAt) / state.blendLength);

    figure.prop = choice.prop;
    figure.bag = choice.bag;
    figure.aiming = choice.aiming;
    figure.fall = 1.0f;

    if (body == Figure::Body::Animal || body == Figure::Body::AnimalCorpse) {
        AnimalPose target;
        switch (choice.act) {
        case Act::AnimalMove:
            target.charge = choice.variant == 1u ? 1.0f : 0.0f;
            break;
        case Act::Graze:
            target = pose::graze(clock_ - state.since, state.seed);
            break;
        case Act::Listen: {
            float yaw = 0.0f;
            if (choice.look) {
                yaw = angleDiff(figure.heading, headingTo(x, y, choice.lookX, choice.lookY));
            } else {
                yaw = (unitOf(state.seed, choice.variant) - 0.5f) * 1.6f;
            }
            target = pose::listenAnimal(t, yaw);
            break;
        }
        case Act::Scratch:
            target = pose::scratchAnimal(t);
            break;
        case Act::SitUp:
            target = pose::sitUpHare(t);
            break;
        default:
            break;
        }
        figure.animal = blendT >= 1.0f ? target : blend(state.animalFrom, target, blendT);
        if (body == Figure::Body::AnimalCorpse) {
            const float age = clock_ - state.fallAt;
            figure.fall = age < kAnimalFallSeconds ? std::pow(age / kAnimalFallSeconds, 2.0f) : 1.0f;
        }
    } else {
        HumanPose target;
        if (choice.act == Act::Rise) {
            target = pose::zombieRise((clock_ - state.riseAt) / pose::kRiseSeconds, state.seed, choice.style);
        } else if (choice.act == Act::Fall) {
            // Падает: тяжело, с ускорением.
            const float u = std::clamp((clock_ - state.fallAt) / kFallSeconds, 0.0f, 1.0f);
            target = blend(state.fallFrom, pose::corpse(state.seed), u * u);
        } else {
            target = humanPose(choice.act, choice.variant, t, figure.gait, state.seed, choice.grip, choice.nervous,
                               choice.style);
        }
        if (choice.look) {
            const float yaw = angleDiff(figure.heading, headingTo(x, y, choice.lookX, choice.lookY));
            target = pose::lookAt(target, yaw, 0.0f, 1.0f);
        }
        if (choice.attacks) {
            const float age = clock_ - state.attackAt;
            target = body == Figure::Body::Zombie ? pose::zombieAttack(target, age)
                                                  : pose::attack(target, age, choice.grip);
        }
        if (choice.tremble > 0.0f) {
            target = pose::tremble(target, clock_, choice.tremble);
        }
        if (choice.twitch > 0.0f) {
            target = pose::twitch(target, clock_, choice.twitch);
        }
        if (blendT >= 1.0f || choice.act == Act::Rise || choice.act == Act::Fall) {
            figure.human = target;
        } else if (state.via) {
            // Лёжа ↔ стоя — через сидячую позу.
            const HumanPose sitting = pose::sitGround(0.0f, state.seed);
            figure.human = blendT < 0.5f ? blend(state.from, sitting, pose::smooth(blendT * 2.0f))
                                         : blend(sitting, target, pose::smooth(blendT * 2.0f - 1.0f));
        } else {
            figure.human = blend(state.from, target, blendT);
        }
    }

    // Значок эмоции.
    figure.emote = Emote::None;
    if (const auto mark = marks_.find(id); mark != marks_.end() && !corpseBody) {
        const float age = clock_ - mark->second.at;
        if (age < kEmoteSeconds) {
            figure.emote = mark->second.emote;
            figure.emoteAge = age;
        }
    }

    state.fresh = false;
    state.seen = frame_;
    return figure;
}

std::optional<float> Animator::heading(EntityId id) const {
    const auto it = states_.find(id);
    if (it == states_.end() || it->second->fresh || frame_ - it->second->seen > 1) {
        return std::nullopt;
    }
    return it->second->figure.heading;
}

void Animator::forgetStale() {
    std::erase_if(states_, [this](const auto& entry) { return frame_ - entry.second->seen > kForgetFrames; });
    std::erase_if(marks_, [this](const auto& entry) { return clock_ - entry.second.at > kEmoteSeconds; });
}

} // namespace lw
