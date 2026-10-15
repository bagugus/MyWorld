#include "sim/systems/UtilityAiSystem.h"

#include "sim/Calendar.h"
#include "sim/Places.h"
#include "sim/World.h"
#include "sim/systems/MovementSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace lw {

namespace {

// --- Время ---------------------------------------------------------------

// Пересмотр решения: в пути и за делом — раз в 10 минут, во сне — раз в 30.
constexpr int kThinkMinutes = 10;
constexpr int kSleepThinkMinutes = 30;
// Не нашлось пути — отдохнуть и подумать снова.
constexpr int kFailRestMinutes = 30;
// Сколько длится действие на месте, игровые минуты.
constexpr int kIdleMinMinutes = 15;
constexpr int kIdleMaxMinutes = 45;
constexpr int kEatMinutes = 20;
constexpr int kDrinkMinutes = 5;
constexpr int kDrinkAtWaterMinutes = 10;
constexpr int kScavengeMinutes = 30;
constexpr int kForageMinutes = 60;
constexpr int kSocializeMinMinutes = 30;
constexpr int kSocializeMaxMinutes = 60;
constexpr int kWanderMinMinutes = 20;
constexpr int kWanderMaxMinutes = 60;
constexpr int kGoHomeMinutes = 30;

// Ночь жаворонка — с 21:00 до 5:30; у совы сдвинута на столько минут.
constexpr int kEarlyBedtime = 21 * 60;
constexpr int kEarlyWakeTime = 5 * 60 + 30;
constexpr float kChronotypeShiftMinutes = 150.0f;
// В соседнее поселение гулять идут только до этого часа: до него несколько часов пути.
constexpr int kNeighbourLatestMinute = 11 * 60;

// --- Эффекты -------------------------------------------------------------

constexpr float kMeal = 0.4f;          // порция еды снимает столько голода
constexpr float kWaterPortion = 0.5f;  // порция воды — столько жажды
constexpr float kForageMeal = 0.25f;   // найденное в лесу и съеденное на месте
constexpr std::uint8_t kCarryFoodMax = 8;
constexpr std::uint8_t kCarryWaterMax = 6;
// Запасливый идёт за едой, когда дома (с несомым) меньше kStockLow порций.
constexpr int kStockLow = 2;
constexpr float kStockWeight = 0.25f;
// Поесть и попить из найденного сразу, если голод (жажда) больше этого.
constexpr float kSnackThreshold = 0.2f;

// --- Оценки --------------------------------------------------------------

// Масштаб расстояния, метры: на таком расстоянии место вдвое менее привлекательно.
constexpr float kNearScale = 1500.0f;
constexpr float kForageScale = 1000.0f;
constexpr float kSocialScale = 800.0f;
// Где искать места, метры.
constexpr float kWaterSearch = 2000.0f;
constexpr float kForageSearch = 1500.0f;
constexpr float kCompanySearch = 500.0f;
constexpr float kGatheringSearch = 2000.0f;
// Дальше этого от дома ночью ложатся спать на месте.
constexpr float kSleepHomeRange = 1500.0f;

constexpr float kEatWeight = 1.1f;
constexpr float kDrinkWeight = 1.2f;
constexpr float kScavengeWeight = 0.9f;
constexpr float kScavengeWaterWeight = 0.5f; // за водой проще к реке
constexpr float kForageWeight = 0.8f;
constexpr float kSocializeWeight = 0.6f;
constexpr float kIdleScore = 0.08f;
constexpr float kIdleHomeBonus = 0.06f; // дома отдыхать приятнее
constexpr float kIdleFatigue = 0.15f;   // уставший охотнее присядет
// Прогулка: основа и вклад любопытства; при выборе оценка умножается на
// «настроение» 0,5..1,5 — иначе любопытные гуляли бы всегда, а остальные никогда.
constexpr float kWanderBase = 0.04f;
constexpr float kWanderCuriosity = 0.14f;
constexpr float kWanderMoodMin = 0.5f;
constexpr float kWanderMoodMax = 1.5f;
constexpr float kNightSleepBase = 0.3f;
constexpr float kDayNapStart = 0.7f; // днём спят, только если усталость выше
constexpr float kNightPenalty = 0.4f; // поиск еды и сбор ночью
// Надбавка текущему действию при пересмотре; сон прервать труднее.
constexpr float kInertia = 0.15f;
constexpr float kSleepInertia = 0.35f;
// Случайный разброс оценок при выборе: ±5 %.
constexpr float kJitter = 0.05f;
// Проснуться, когда усталость ниже этого и ночь кончилась.
constexpr float kRested = 0.1f;

std::uint64_t minutes(int count) {
    return static_cast<std::uint64_t>(count) * kTicksPerGameMinute;
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float distance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

float nearness(float meters, float scale) {
    return scale / (scale + meters);
}

// Насколько нужда требует внимания: 0 до порога start, дальше растёт к 1
// быстрее к концу (u^1,5).
float urge(float value, float start) {
    const float u = clamp01((value - start) / (1.0f - start));
    return u * std::sqrt(u);
}

// Урожай леса по сезону: доля удачных часов сбора.
float forageYield(Season season) {
    switch (season) {
    case Season::Spring:
        return 0.4f;
    case Season::Summer:
        return 0.7f;
    case Season::Autumn:
        return 0.9f;
    case Season::Winter:
        return 0.05f;
    }
    return 0.0f;
}

Cell cellOf(const Position& position) {
    return Map::cellAt(position.x, position.y);
}

float cellX(Cell cell) {
    return Map::cellCenter(cell.x);
}

float cellY(Cell cell) {
    return Map::cellCenter(cell.y);
}

// Вариант действия: оценка и где его выполнять.
struct Option {
    float score = 0.0f;
    Cell spot;
    EntityId target = kInvalidId;
};

// Всё, что нужно для оценок одного человека.
struct Context {
    const World& world;
    entt::entity entity;
    EntityId id;
    const Person& person;
    const Needs& needs;
    const Traits& traits;
    const Carry& carry;
    Cell cell;
    float x;
    float y;
    CalendarTime time;
    bool night;
    // Дом: kInvalidId — нет.
    EntityId homeId = kInvalidId;
    Cell homeDoor;
    const Supplies* homeSupplies = nullptr;
    float homeDistance = 0.0f;
    bool atHome = false;
    // Поселение, где человек ищет еду и гуляет: где дом, у бездомного — ближайшее.
    EntityId settlement = kInvalidId;
};

Context makeContext(const World& world, entt::entity entity) {
    const entt::registry& registry = world.registry();
    const Position& position = registry.get<Position>(entity);
    const Person& person = registry.get<Person>(entity);
    const Traits* traits = registry.try_get<Traits>(entity);
    static const Traits kAverage{};
    static const Carry kEmpty{};
    const Carry* carry = registry.try_get<Carry>(entity);
    const CalendarTime time = calendarAt(world.tick());
    Context ctx{world,
                entity,
                registry.get<Id>(entity).value,
                person,
                registry.get<Needs>(entity),
                traits != nullptr ? *traits : kAverage,
                carry != nullptr ? *carry : kEmpty,
                cellOf(position),
                position.x,
                position.y,
                time,
                false};
    ctx.night = isPersonalNight(ctx.traits.chronotype, time.minuteOfDay);
    if (const entt::entity home = world.find(person.home); home != entt::null) {
        const Building& building = registry.get<Building>(home);
        ctx.homeId = person.home;
        ctx.homeDoor = building.door;
        ctx.homeSupplies = registry.try_get<Supplies>(home);
        ctx.homeDistance = distance(ctx.x, ctx.y, cellX(building.door), cellY(building.door));
        ctx.atHome = ctx.cell == building.door;
        ctx.settlement = building.settlement;
    } else {
        ctx.settlement = world.places().nearestSettlement(ctx.x, ctx.y);
    }
    return ctx;
}

// Порог, с которого нужда зовёт к действию: запасливый ест и пьёт раньше.
float needStart(const Context& ctx) {
    return 0.25f - 0.3f * (ctx.traits.prudence - 0.5f);
}

Option here(const Context& ctx, float score) {
    return {score, ctx.cell, kInvalidId};
}

Option atHome(const Context& ctx, float score) {
    return {score * nearness(ctx.homeDistance, kNearScale), ctx.homeDoor, ctx.homeId};
}

Option better(const Option& a, const Option& b) {
    return b.score > a.score ? b : a;
}

Option evalIdle(const Context& ctx) {
    return here(ctx, kIdleScore + (ctx.atHome ? kIdleHomeBonus : 0.0f) +
                         kIdleFatigue * ctx.needs.fatigue);
}

Option evalSleep(const Context& ctx) {
    const float fatigue = ctx.needs.fatigue;
    float score = 0.0f;
    if (ctx.night) {
        score = kNightSleepBase + (1.0f - kNightSleepBase) * fatigue;
    } else if (fatigue > kDayNapStart) {
        const float u = (fatigue - kDayNapStart) / (1.0f - kDayNapStart);
        score = 0.9f * u * u;
    }
    if (score <= 0.0f) {
        return {};
    }
    // Спать — дома; если дом далеко, а уже ночь, — где застала.
    if (ctx.homeId != kInvalidId && ctx.homeDistance <= kSleepHomeRange) {
        return {score, ctx.homeDoor, ctx.homeId};
    }
    return here(ctx, score);
}

Option evalEat(const Context& ctx) {
    const float need = kEatWeight * urge(ctx.needs.hunger, needStart(ctx));
    if (need <= 0.0f) {
        return {};
    }
    if (ctx.carry.food > 0) {
        return here(ctx, need);
    }
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food > 0) {
        return atHome(ctx, need);
    }
    return {};
}

Option evalDrink(const Context& ctx) {
    const float need = kDrinkWeight * urge(ctx.needs.thirst, needStart(ctx));
    if (need <= 0.0f) {
        return {};
    }
    if (ctx.carry.water > 0) {
        return here(ctx, need);
    }
    Option best;
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->water > 0) {
        best = atHome(ctx, need);
    }
    // К воде — только если она заметно ближе дома (или дома воды нет).
    if (best.score < need * 0.9f) {
        const World& world = ctx.world;
        if (const auto spot = world.places().nearestWater(world.nav(), ctx.cell, kWaterSearch)) {
            const float d = distance(ctx.x, ctx.y, cellX(*spot), cellY(*spot));
            best = better(best, {need * nearness(d, kNearScale), *spot, kInvalidId});
        }
    }
    return best;
}

Option evalScavenge(const Context& ctx) {
    const bool foodAtHand =
        ctx.carry.food > 0 || (ctx.homeSupplies != nullptr && ctx.homeSupplies->food > 0);
    const bool waterAtHand =
        ctx.carry.water > 0 || (ctx.homeSupplies != nullptr && ctx.homeSupplies->water > 0);
    const float start = needStart(ctx);
    float need = 0.0f;
    if (!foodAtHand) {
        need = std::max(need, kScavengeWeight * urge(ctx.needs.hunger, start));
    }
    if (!waterAtHand) {
        need = std::max(need, kScavengeWaterWeight * urge(ctx.needs.thirst, start));
    }
    // Запасливый идёт за едой, пока дома кончается, а не когда уже голоден;
    // несомое домой тоже считается, с полными руками не ходят.
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food + ctx.carry.food < kStockLow &&
        !ctx.night) {
        need = std::max(need, kStockWeight * ctx.traits.prudence);
    }
    const Places::SettlementPlace* settlement = ctx.world.places().settlement(ctx.settlement);
    if (need <= 0.0f || settlement == nullptr) {
        return {};
    }
    if (ctx.night) {
        need *= kNightPenalty;
    }
    // Здание выберется при выполнении; пока — расстояние до центра поселения.
    const float d = distance(ctx.x, ctx.y, settlement->x, settlement->y);
    return {need * nearness(d, kNearScale), settlement->center, kInvalidId};
}

Option evalForage(const Context& ctx) {
    if (ctx.carry.food > 0) {
        return {};
    }
    float need = kForageWeight * urge(ctx.needs.hunger, needStart(ctx)) *
                 forageYield(ctx.time.season);
    if (need <= 0.0f) {
        return {};
    }
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food > 0) {
        need *= 0.4f; // дома есть еда — в лес незачем
    }
    if (ctx.night) {
        need *= kNightPenalty;
    }
    const World& world = ctx.world;
    const auto spot = world.places().nearestForage(world.nav(), ctx.cell, kForageSearch);
    if (!spot) {
        return {};
    }
    const float d = distance(ctx.x, ctx.y, cellX(*spot), cellY(*spot));
    return {need * nearness(d, kForageScale), *spot, kInvalidId};
}

Option evalSocialize(const Context& ctx) {
    if (ctx.night) {
        return {};
    }
    const float lonely = ctx.needs.loneliness;
    const float need =
        kSocializeWeight * lonely * std::sqrt(lonely) * (0.5f + ctx.traits.sociability);
    if (need <= 0.0f) {
        return {};
    }
    const World& world = ctx.world;
    const entt::registry& registry = world.registry();
    // К ближайшему, кто не спит; никого рядом — на площадь своего поселения.
    const auto other = world.spatial().nearest(
        ctx.x, ctx.y, kCompanySearch, [&](const SpatialHit& hit) {
            const Person* person = registry.try_get<Person>(hit.entity);
            return hit.id != ctx.id && person != nullptr && !person->asleep;
        });
    if (other) {
        const float d = std::sqrt(other->distanceSq);
        return {need * nearness(d, kSocialScale), Map::cellAt(other->x, other->y), kInvalidId};
    }
    if (const auto* settlement = world.places().settlement(ctx.settlement)) {
        const float d = distance(ctx.x, ctx.y, settlement->x, settlement->y);
        if (d <= kGatheringSearch) {
            return {need * nearness(d, kSocialScale), settlement->center, kInvalidId};
        }
    }
    return {};
}

Option evalWander(const Context& ctx) {
    if (ctx.night || ctx.world.places().settlement(ctx.settlement) == nullptr) {
        return {};
    }
    // Цель выберется при выполнении.
    return here(ctx, kWanderBase + kWanderCuriosity * ctx.traits.curiosity);
}

Option evalGoHome(const Context& ctx) {
    if (ctx.homeId == kInvalidId || ctx.atHome) {
        return {};
    }
    const float score = ctx.needs.fear * (1.2f - 0.6f * ctx.traits.bravery);
    return {score, ctx.homeDoor, ctx.homeId};
}

Option evaluate(const Context& ctx, Action action) {
    switch (action) {
    case Action::Idle:
        return evalIdle(ctx);
    case Action::Sleep:
        return evalSleep(ctx);
    case Action::Eat:
        return evalEat(ctx);
    case Action::Drink:
        return evalDrink(ctx);
    case Action::Scavenge:
        return evalScavenge(ctx);
    case Action::Forage:
        return evalForage(ctx);
    case Action::Socialize:
        return evalSocialize(ctx);
    case Action::Wander:
        return evalWander(ctx);
    case Action::GoHome:
        return evalGoHome(ctx);
    case Action::Count:
        break;
    }
    return {};
}

constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

Action actionAt(std::size_t index) {
    return static_cast<Action>(index);
}

struct Candidate {
    Action action = Action::Idle;
    Option option;
};

// Сколько действий пробовать начать за одно решение.
constexpr int kStartAttempts = 3;

// --- Выбор цели при выполнении -------------------------------------------

bool remembered(const Mind& mind, EntityId building) {
    return std::find(std::begin(mind.searched), std::end(mind.searched), building) !=
           std::end(mind.searched);
}

void remember(Mind& mind, EntityId building) {
    mind.searched[mind.searchedNext] = building;
    mind.searchedNext = static_cast<std::uint8_t>((mind.searchedNext + 1) % Mind::kSearchedMemory);
}

// Здание, где ещё не искали: из трёх случайных — ближайшее. Сначала в своём
// поселении, потом в соседних.
std::optional<Places::BuildingPlace> pickScavengeTarget(const Context& ctx, const Mind& mind,
                                                        Rng& rng) {
    const Places& places = ctx.world.places();
    const Places::SettlementPlace* own = places.settlement(ctx.settlement);
    if (own == nullptr) {
        return std::nullopt;
    }
    std::vector<const Places::SettlementPlace*> order{own};
    for (const EntityId neighbour : own->neighbours) {
        order.push_back(places.settlement(neighbour));
    }
    std::vector<const Places::BuildingPlace*> candidates;
    for (const Places::SettlementPlace* settlement : order) {
        candidates.clear();
        for (const Places::BuildingPlace& building : settlement->buildings) {
            if (building.id != ctx.homeId && !remembered(mind, building.id)) {
                candidates.push_back(&building);
            }
        }
        if (candidates.empty()) {
            continue;
        }
        const Places::BuildingPlace* best = nullptr;
        float bestDistance = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const Places::BuildingPlace* pick =
                candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))];
            const float d = distance(ctx.x, ctx.y, cellX(pick->door), cellY(pick->door));
            if (best == nullptr || d < bestDistance) {
                best = pick;
                bestDistance = d;
            }
        }
        return *best;
    }
    return std::nullopt;
}

// Дверь случайного здания своего поселения, утром любопытный идёт и к соседям.
std::optional<Cell> pickWanderTarget(const Context& ctx, Rng& rng) {
    const Places& places = ctx.world.places();
    const Places::SettlementPlace* settlement = places.settlement(ctx.settlement);
    if (settlement == nullptr) {
        return std::nullopt;
    }
    if (ctx.time.minuteOfDay < kNeighbourLatestMinute && !settlement->neighbours.empty() &&
        rng.chance(0.1f + 0.3f * ctx.traits.curiosity)) {
        const EntityId id = settlement->neighbours[rng.below(
            static_cast<std::uint32_t>(settlement->neighbours.size()))];
        if (const auto* neighbour = places.settlement(id); !neighbour->buildings.empty()) {
            settlement = neighbour;
        }
    }
    if (settlement->buildings.empty()) {
        return std::nullopt;
    }
    return settlement->buildings[rng.below(static_cast<std::uint32_t>(settlement->buildings.size()))]
        .door;
}

// --- Выполнение ----------------------------------------------------------

class Brain {
public:
    Brain(World& world, entt::entity entity, Rng& rng)
        : world_(world),
          registry_(world.registry()),
          entity_(entity),
          rng_(rng),
          tick_(world.tick()),
          id_(registry_.get<Id>(entity).value),
          mind_(registry_.get<Mind>(entity)),
          person_(registry_.get<Person>(entity)),
          needs_(registry_.get<Needs>(entity)) {}

    void update() {
        const Walker& walker = registry_.get<Walker>(entity_);
        if (mind_.phase == ActionPhase::Travel) {
            if (!walker.moving()) {
                arrive();
            } else if (tick_ >= mind_.nextThink) {
                reconsider();
            }
            return;
        }
        if (mind_.action == Action::Sleep) {
            if (tick_ >= mind_.nextThink) {
                const Context ctx = makeContext(world_, entity_);
                if (!ctx.night && needs_.fatigue < kRested) {
                    finish();
                    think();
                } else {
                    reconsider();
                }
            }
            return;
        }
        if (tick_ >= mind_.until) {
            finish();
            think();
        } else if (tick_ >= mind_.nextThink) {
            reconsider();
        }
    }

private:
    // Выбрать лучшее действие и начать его. Не начинается (некуда идти, нет
    // пути) — следующее по оценке; ничего не вышло — отдых.
    void think() {
        const Context ctx = makeContext(world_, entity_);
        std::array<Candidate, kActionCount> candidates;
        for (std::size_t i = 0; i < kActionCount; ++i) {
            Option option = evaluate(ctx, actionAt(i));
            option.score *= 1.0f + rng_.range(-kJitter, kJitter);
            if (actionAt(i) == Action::Wander) {
                option.score *= rng_.range(kWanderMoodMin, kWanderMoodMax);
            }
            candidates[i] = {actionAt(i), option};
        }
        if (startBest(ctx, candidates, 0.0f)) {
            return;
        }
        fail();
    }

    // Пересмотр посреди действия: сменить, если другое заметно важнее. Если
    // ни одно из более важных не начинается, текущее продолжается.
    void reconsider() {
        const Context ctx = makeContext(world_, entity_);
        const float current = evaluate(ctx, mind_.action).score;
        const float inertia = mind_.action == Action::Sleep ? kSleepInertia : kInertia;
        float keep = current > 0.0f ? current + inertia : 0.0f;
        // Прогулку и отдых ничто не держит, кроме начатого.
        if (mind_.action == Action::Wander || mind_.action == Action::Idle) {
            keep = std::max(keep, mind_.score + inertia);
        }
        // Уснувший спит, пока не отдохнёт, даже если уснул днём: иначе дремота
        // обрывалась бы, едва усталость опустится ниже порога.
        if (person_.asleep) {
            keep = std::max(keep, kNightSleepBase + inertia);
        }
        std::array<Candidate, kActionCount> candidates;
        for (std::size_t i = 0; i < kActionCount; ++i) {
            candidates[i] = {actionAt(i), actionAt(i) == mind_.action ? Option{}
                                                                       : evaluate(ctx, actionAt(i))};
        }
        if (startBest(ctx, candidates, keep)) {
            return;
        }
        // Неудачная попытка проложить путь останавливает человека: тогда
        // начатое не продолжить, нужно решать заново.
        if (mind_.phase == ActionPhase::Travel && !registry_.get<Walker>(entity_).moving()) {
            think();
            return;
        }
        mind_.nextThink = tick_ + minutes(mind_.action == Action::Sleep && mind_.phase ==
                                                                               ActionPhase::Perform
                                              ? kSleepThinkMinutes
                                              : kThinkMinutes);
    }

    // Начать лучшее из действий с оценкой выше threshold; пробуется не больше
    // kStartAttempts — каждая попытка может стоить поиска пути.
    bool startBest(const Context& ctx, std::array<Candidate, kActionCount>& candidates,
                   float threshold) {
        // Устойчивая сортировка: при равных оценках — порядок перечисления Action.
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate& a, const Candidate& b) {
                             return a.option.score > b.option.score;
                         });
        int attempts = 0;
        for (const Candidate& candidate : candidates) {
            if (candidate.option.score <= threshold || attempts == kStartAttempts) {
                break;
            }
            ++attempts;
            if (tryStart(ctx, candidate.action, candidate.option)) {
                return true;
            }
        }
        return false;
    }

    // Начать действие. false — некуда идти или нет пути; тогда Mind не меняется
    // (но идущий человек останавливается: так устроен startWalk).
    bool tryStart(const Context& ctx, Action action, Option option) {
        if (action == Action::Scavenge) {
            const auto building = pickScavengeTarget(ctx, mind_, rng_);
            if (!building) {
                return false;
            }
            option.target = building->id;
            option.spot = building->door;
        } else if (action == Action::Wander) {
            const auto door = pickWanderTarget(ctx, rng_);
            if (!door) {
                return false;
            }
            option.spot = *door;
        }

        bool here = option.spot == ctx.cell;
        if (!here && !startWalk(world_, entity_, option.spot)) {
            if (action != Action::Sleep) {
                return false;
            }
            // До дома не дойти — спать там, где застала ночь.
            option.spot = ctx.cell;
            option.target = kInvalidId;
            here = true;
        }
        if (here) {
            stopWalking();
        }
        person_.asleep = false;
        mind_.action = action;
        mind_.score = option.score;
        mind_.target = option.target;
        mind_.spot = option.spot;
        mind_.failures = 0;
        if (here) {
            perform();
        } else {
            mind_.phase = ActionPhase::Travel;
            mind_.nextThink = tick_ + minutes(kThinkMinutes);
        }
        return true;
    }

    // Ничего не начать: постоять и подумать снова.
    void fail() {
        ++mind_.failures;
        stopWalking();
        person_.asleep = false;
        mind_.action = Action::Idle;
        mind_.score = kIdleScore;
        mind_.target = kInvalidId;
        mind_.spot = cellOf(registry_.get<Position>(entity_));
        mind_.phase = ActionPhase::Perform;
        mind_.until = tick_ + minutes(kFailRestMinutes);
        mind_.nextThink = mind_.until;
    }

    void arrive() {
        const Cell cell = cellOf(registry_.get<Position>(entity_));
        // Дома — выложить найденное в запасы.
        if (const entt::entity home = world_.find(person_.home); home != entt::null) {
            if (cell == registry_.get<Building>(home).door) {
                Carry& carry = registry_.get<Carry>(entity_);
                if (auto* supplies = registry_.try_get<Supplies>(home)) {
                    if (carry.food > 0 || carry.water > 0) {
                        world_.record(ChronicleKind::BroughtHome, id_, person_.home, carry.food,
                                      carry.water);
                    }
                    supplies->food = static_cast<std::uint16_t>(supplies->food + carry.food);
                    supplies->water = static_cast<std::uint16_t>(supplies->water + carry.water);
                    carry = {};
                }
            }
        }
        // Остановился не там (путь оборвали) — действие здесь не выполнить:
        // нельзя есть из домашних запасов вдали от дома.
        if (cell != mind_.spot) {
            think();
            return;
        }
        perform();
    }

    void perform() {
        mind_.phase = ActionPhase::Perform;
        int duration = 0;
        switch (mind_.action) {
        case Action::Idle:
            duration = rng_.rangeInt(kIdleMinMinutes, kIdleMaxMinutes);
            break;
        case Action::Sleep:
            person_.asleep = true;
            // Ночлег не дома — событие; для бездомного это обычная ночь.
            if (mind_.target == kInvalidId && person_.home != kInvalidId) {
                world_.record(ChronicleKind::SleptOutside, id_, kInvalidId, mind_.spot.x,
                              mind_.spot.y);
            }
            mind_.until = std::numeric_limits<std::uint64_t>::max();
            mind_.nextThink = tick_ + minutes(kSleepThinkMinutes);
            return;
        case Action::Eat:
            duration = kEatMinutes;
            break;
        case Action::Drink:
            duration = mind_.target == kInvalidId && registry_.get<Carry>(entity_).water == 0
                           ? kDrinkAtWaterMinutes
                           : kDrinkMinutes;
            break;
        case Action::Scavenge:
            duration = kScavengeMinutes;
            break;
        case Action::Forage:
            duration = kForageMinutes;
            break;
        case Action::Socialize:
            duration = rng_.rangeInt(kSocializeMinMinutes, kSocializeMaxMinutes);
            break;
        case Action::Wander:
            duration = rng_.rangeInt(kWanderMinMinutes, kWanderMaxMinutes);
            break;
        case Action::GoHome:
            duration = kGoHomeMinutes;
            break;
        case Action::Count:
            break;
        }
        mind_.until = tick_ + minutes(duration);
        mind_.nextThink = std::min(mind_.until, tick_ + minutes(kThinkMinutes));
    }

    void stopWalking() {
        Walker& walker = registry_.get<Walker>(entity_);
        walker.waypoints.clear();
        walker.next = 0;
    }

    // Действие выполнено: его результат.
    void finish() {
        person_.asleep = false;
        Carry& carry = registry_.get<Carry>(entity_);
        Supplies* supplies = nullptr;
        if (const entt::entity building = world_.find(mind_.target); building != entt::null) {
            supplies = registry_.try_get<Supplies>(building);
        }
        switch (mind_.action) {
        case Action::Eat:
            if (carry.food > 0) {
                --carry.food;
                needs_.hunger = clamp01(needs_.hunger - kMeal);
            } else if (supplies != nullptr && supplies->food > 0) {
                --supplies->food;
                needs_.hunger = clamp01(needs_.hunger - kMeal);
                if (supplies->food == 0) {
                    world_.record(ChronicleKind::AteLastFood, id_, mind_.target);
                }
            }
            break;
        case Action::Drink:
            if (carry.water > 0) {
                --carry.water;
                needs_.thirst = clamp01(needs_.thirst - kWaterPortion);
            } else if (mind_.target == kInvalidId) {
                needs_.thirst = 0.0f; // у реки или озера — вдоволь
            } else if (supplies != nullptr && supplies->water > 0) {
                --supplies->water;
                needs_.thirst = clamp01(needs_.thirst - kWaterPortion);
                if (supplies->water == 0) {
                    world_.record(ChronicleKind::DrankLastWater, id_, mind_.target);
                }
            }
            break;
        case Action::Scavenge:
            remember(mind_, mind_.target);
            if (supplies != nullptr) {
                takeSupplies(*supplies, carry);
            } else {
                world_.record(ChronicleKind::Scavenged, id_, mind_.target);
            }
            break;
        case Action::Forage:
            forage(carry);
            break;
        default:
            break;
        }
    }

    // Найденное: что-то съесть и выпить сразу, остальное — с собой, сколько
    // позволяют жадность и запасливость.
    void takeSupplies(Supplies& supplies, Carry& carry) {
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float greed = traits != nullptr ? traits->greed : 0.5f;
        const float prudence = traits != nullptr ? traits->prudence : 0.5f;
        const int wantFood = 1 + static_cast<int>(std::lround(2.0f * greed + 2.0f * prudence));
        const int wantWater = 1 + static_cast<int>(std::lround(greed + prudence));
        int food = std::min<int>(supplies.food, wantFood);
        int water = std::min<int>(supplies.water, wantWater);
        supplies.food = static_cast<std::uint16_t>(supplies.food - food);
        supplies.water = static_cast<std::uint16_t>(supplies.water - water);
        world_.record(ChronicleKind::Scavenged, id_, mind_.target, food, water);
        if (food > 0 && needs_.hunger > kSnackThreshold) {
            --food;
            needs_.hunger = clamp01(needs_.hunger - kMeal);
        }
        if (water > 0 && needs_.thirst > kSnackThreshold) {
            --water;
            needs_.thirst = clamp01(needs_.thirst - kWaterPortion);
        }
        // Что не унести, остаётся в здании.
        const int carryFood = std::min(food, kCarryFoodMax - carry.food);
        const int carryWater = std::min(water, kCarryWaterMax - carry.water);
        supplies.food = static_cast<std::uint16_t>(supplies.food + food - carryFood);
        supplies.water = static_cast<std::uint16_t>(supplies.water + water - carryWater);
        carry.food = static_cast<std::uint8_t>(carry.food + carryFood);
        carry.water = static_cast<std::uint8_t>(carry.water + carryWater);
    }

    void forage(Carry& carry) {
        const float yield = forageYield(calendarAt(tick_).season);
        if (!rng_.chance(yield)) {
            return;
        }
        needs_.hunger = clamp01(needs_.hunger - kForageMeal);
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float prudence = traits != nullptr ? traits->prudence : 0.5f;
        if (carry.food < kCarryFoodMax && rng_.chance(prudence * yield)) {
            ++carry.food;
        }
    }

    World& world_;
    entt::registry& registry_;
    entt::entity entity_;
    Rng& rng_;
    std::uint64_t tick_;
    EntityId id_;
    Mind& mind_;
    Person& person_;
    Needs& needs_;
};

} // namespace

bool isPersonalNight(float chronotype, int minuteOfDay) {
    const int shift = static_cast<int>(chronotype * kChronotypeShiftMinutes);
    const int bedtime = (kEarlyBedtime + shift) % (24 * 60);
    const int wake = kEarlyWakeTime + shift;
    return bedtime > wake ? minuteOfDay >= bedtime || minuteOfDay < wake
                          : minuteOfDay >= bedtime && minuteOfDay < wake;
}

ActionScores evaluateActions(const World& world, entt::entity person) {
    ActionScores scores{};
    const Context ctx = makeContext(world, person);
    for (std::size_t i = 0; i < kActionCount; ++i) {
        scores[i] = evaluate(ctx, actionAt(i)).score;
    }
    return scores;
}

void updateUtilityAi(World& world) {
    Rng& rng = world.rng(RngStream::Behavior);
    // Порядок — порядок хранения в реестре: он детерминирован. Компоненты по ходу
    // не добавляются и не удаляются, поэтому обход не ломается.
    auto view = world.registry().view<Person, Mind, Needs, const Walker, const Position>();
    for (const entt::entity entity : view) {
        Brain(world, entity, rng).update();
    }
}

} // namespace lw
