#include "sim/systems/UtilityAiSystem.h"

#include "sim/Calendar.h"
#include "sim/Items.h"
#include "sim/Perception.h"
#include "sim/Places.h"
#include "sim/Weapons.h"
#include "sim/World.h"
#include "sim/systems/MovementSystem.h"
#include "sim/systems/ZombieSystem.h"

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
// Бегство: добежав, постоять и оглядеться; в пути — решать заново каждую минуту.
constexpr int kFleeMinMinutes = 1;
constexpr int kFleeMaxMinutes = 2;
constexpr int kFleeThinkMinutes = 1;
// Бегство позже этого после прошлого — новый случай для летописи.
constexpr int kFleeEpisodeMinutes = 60;

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
// Сколько еды (с мясом) и воды человек берёт с собой — меньше, чем позволяет
// вес (kCarryCapacity): не всё, что найдено, нужно унести.
constexpr int kCarryFoodMax = 8;
constexpr int kCarryWaterMax = 6;
// Снаряжение (этап 5.6): бинтов и лекарств при себе — не больше этого; сколько
// именно, решает запасливость (этап 5.7): от kBandageMin до kBandageMin + kBandageSpread.
constexpr int kBandageMin = 1;
constexpr float kBandageSpread = 3.0f;
constexpr int kMedicineMin = 1;
constexpr float kMedicineSpread = 2.0f;
// Перевязаться — при кровотечении сильнее этого; лекарство — при здоровье ниже.
constexpr float kTreatBleeding = 0.03f;
constexpr float kMedicineBelow = 0.6f;
constexpr float kMedicineHeal = 0.25f;
constexpr int kTreatMinutes = 3;
// Тело с вещами ближе этого обыскивают раньше зданий, метры.
constexpr float kBodySearch = 300.0f;
// Запасливый идёт за едой, когда дома (с несомым) меньше kStockLow порций.
constexpr int kStockLow = 2;
constexpr float kStockWeight = 0.25f;
// Каждый обыск подряд без еды умножает желание искать еду в зданиях на это, но
// не ниже kEmptySearchFloor: обобранная округа гонит в лес, а отчаявшийся всё равно ищет.
constexpr float kEmptySearchDecay = 0.6f;
constexpr float kEmptySearchFloor = 0.25f;
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
// К знакомым с симпатией не ниже kVisitAffinity идут дальше (этап 6.1).
constexpr float kFriendSearch = 1500.0f;
constexpr float kVisitAffinity = 0.2f;
constexpr float kGatheringSearch = 2000.0f;
// Дальше этого от дома ночью ложатся спать на месте.
constexpr float kSleepHomeRange = 1500.0f;

constexpr float kEatWeight = 1.1f;
constexpr float kDrinkWeight = 1.2f;
constexpr float kScavengeWeight = 0.9f;
constexpr float kScavengeWaterWeight = 0.5f; // за водой проще к реке
constexpr float kForageWeight = 0.8f;
constexpr float kSocializeWeight = 0.6f;
// Теплота к человеку — множитель оценки общения: 1 + kFriendWarmth × симпатия ×
// знакомство (незнакомец — 1, близкий друг — до 1,6), не ниже kMinWarmth.
constexpr float kFriendWarmth = 0.6f;
constexpr float kMinWarmth = 0.3f;
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

// Бегство от зомби: оценка — угроза × (kFleeBase − kFleeBravery × смелость).
// Бегом — от угрозы не слабее kFleeMinThreat (зомби на виду ближе ~45 м,
// слышен ближе ~30 м, гонится); от дальнего — отходит шагом.
constexpr float kFleeBase = 1.3f;
constexpr float kFleeBravery = 0.6f;
constexpr float kFleeMinThreat = 0.25f;
// Опасные места — где видел или слышал зомби (Mind::dangers): помнятся
// kDangerMemoryMinutes, места ближе kDangerRadius к любому из них (и пути,
// которые подводят к нему ближе, чем человек сейчас) оцениваются в
// kDangerPenalty раз ниже. Внутри зоны — отойти (оценка kWaryWeight у самого
// места, к краю зоны — ноль). Зомби, замеченный ближе kDangerMergeCells к уже
// известному месту, обновляет его, а не занимает новое.
constexpr int kDangerMemoryMinutes = 240;
constexpr int kDangerMergeCells = 8;
constexpr float kDangerSlack = 5.0f;
constexpr float kDangerPenalty = 0.15f;
constexpr float kWaryWeight = 0.5f;
// Бегом — во столько раз быстрее шага.
constexpr float kRunPace = 1.8f;
// Бежать на столько метров прочь от угроз; домой — если дом ближе
// kFleeHomeRange и в ту же сторону (косинус угла не меньше kFleeHomeAlignment).
constexpr float kFleeDistance = 80.0f;
constexpr float kFleeHomeRange = 400.0f;
constexpr float kFleeHomeAlignment = 0.5f;
// Направления бегства: прямо прочь, затем на 40° и 80° в стороны (cos, sin).
constexpr float kFleeTurns[][2] = {
    {1.0f, 0.0f}, {0.766f, 0.643f}, {0.766f, -0.643f}, {0.174f, 0.985f}, {0.174f, -0.985f}};

// Бой с зомби (этап 5.4): оценка — угроза × (kFightBase + сила оружия) ×
// решимость × здоровье / толпа. Решимость — kResolveBase + смелость и
// агрессия; каждый следующий зомби делит оценку на 1 + kCrowdPenalty.
constexpr float kFightBase = 0.6f;
constexpr float kResolveBase = 0.3f;
constexpr float kResolveBravery = 0.6f;
constexpr float kResolveAggression = 0.3f;
constexpr float kCrowdPenalty = 0.5f;
constexpr float kFightWounded = 0.6f; // при нуле здоровья решимости меньше на столько
// Бой держится, пока угроза есть; затянулся без неё — кончился.
constexpr int kFightMinutes = 2;

// Охота (этап 5.4): с ружьём и патронами, когда голоден, к ближайшему зверю не
// дальше kHuntSearch — люди знают, где он ходит (следы).
constexpr float kHuntWeight = 0.8f;
constexpr float kHuntSearch = 1200.0f;
constexpr float kHuntScale = 1000.0f;
constexpr int kHuntMinutes = 180;     // сколько выслеживать
constexpr int kButcherMinutes = 10;   // разделать тушу
constexpr float kStalkPace = 0.8f;    // крадётся: тише шаги
constexpr float kTrackRegoal = 12.0f; // зверь ушёл от цели пути дальше — идти за ним заново

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
    const Inventory& carry; // что несёт с собой
    std::uint8_t emptySearches;
    Cell cell;
    float x;
    float y;
    CalendarTime time;
    bool night;
    // Дом: kInvalidId — нет.
    EntityId homeId = kInvalidId;
    Cell homeDoor;
    const Inventory* homeSupplies = nullptr; // запасы дома
    float homeDistance = 0.0f;
    bool atHome = false;
    // Поселение, где человек ищет еду и гуляет: где дом, у бездомного — ближайшее.
    EntityId settlement = kInvalidId;
    // Зомби, которых человек сейчас замечает.
    Threat threat;
    // Где недавно замечал зомби (Mind::dangers), пока помнит; метры.
    std::array<float, Mind::kDangerMemory> dangerX{};
    std::array<float, Mind::kDangerMemory> dangerY{};
    std::size_t dangerCount = 0;
    // Оружие в руках (этап 5.4).
    Weapon weapon{};
    // Кровотечение (этап 5.6: перевязка).
    float bleeding = 0.0f;
    // Кого знает (этап 6.1); nullptr — никого.
    const Relations* relations = nullptr;
};

Context makeContext(const World& world, entt::entity entity) {
    const entt::registry& registry = world.registry();
    const Position& position = registry.get<Position>(entity);
    const Person& person = registry.get<Person>(entity);
    const Traits* traits = registry.try_get<Traits>(entity);
    static const Traits kAverage{};
    static const Inventory kEmpty{};
    const Inventory* carry = registry.try_get<Inventory>(entity);
    const Mind* mind = registry.try_get<Mind>(entity);
    const CalendarTime time = calendarAt(world.tick());
    Context ctx{world,
                entity,
                registry.get<Id>(entity).value,
                person,
                registry.get<Needs>(entity),
                traits != nullptr ? *traits : kAverage,
                carry != nullptr ? *carry : kEmpty,
                mind != nullptr ? mind->emptySearches : std::uint8_t{0},
                cellOf(position),
                position.x,
                position.y,
                time,
                false};
    ctx.night = isPersonalNight(ctx.traits.chronotype, time.minuteOfDay);
    ctx.threat = threatOf(world, entity);
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        ctx.weapon = *weapon;
    }
    if (const auto* wounds = registry.try_get<Wounds>(entity)) {
        ctx.bleeding = wounds->bleeding;
    }
    ctx.relations = registry.try_get<Relations>(entity);
    if (mind != nullptr) {
        for (const Danger& danger : mind->dangers) {
            if (world.tick() < danger.until) {
                ctx.dangerX[ctx.dangerCount] = cellX(danger.cell);
                ctx.dangerY[ctx.dangerCount] = cellY(danger.cell);
                ++ctx.dangerCount;
            }
        }
    }
    if (const entt::entity home = world.find(person.home); home != entt::null) {
        const Building& building = registry.get<Building>(home);
        ctx.homeId = person.home;
        ctx.homeDoor = building.door;
        ctx.homeSupplies = registry.try_get<Inventory>(home);
        ctx.homeDistance = distance(ctx.x, ctx.y, cellX(building.door), cellY(building.door));
        ctx.atHome = ctx.cell == building.door;
        ctx.settlement = building.settlement;
    } else {
        ctx.settlement = world.places().nearestSettlement(ctx.x, ctx.y);
    }
    return ctx;
}

// Расстояние от точки p до отрезка ab.
float segmentDistance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSq = dx * dx + dy * dy;
    const float along = lengthSq > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / lengthSq : 0.0f;
    const float t = std::clamp(along, 0.0f, 1.0f);
    return distance(px, py, ax + t * dx, ay + t * dy);
}

// Опасно ли идти в клетку spot: она у места, где недавно замечал зомби, или
// путь по прямой подводит к такому месту ближе, чем человек сейчас. Дом —
// укрытие: опасен только путь к нему.
bool risky(const Context& ctx, Cell spot) {
    const float sx = cellX(spot);
    const float sy = cellY(spot);
    const bool shelter = ctx.homeId != kInvalidId && spot == ctx.homeDoor;
    for (std::size_t i = 0; i < ctx.dangerCount; ++i) {
        const float dx = ctx.dangerX[i];
        const float dy = ctx.dangerY[i];
        if (!shelter && distance(sx, sy, dx, dy) < kDangerRadius) {
            return true;
        }
        const float self = distance(ctx.x, ctx.y, dx, dy);
        if (segmentDistance(dx, dy, ctx.x, ctx.y, sx, sy) <
            std::min(kDangerRadius, self) - kDangerSlack) {
            return true;
        }
    }
    return false;
}

// Ближайшее опасное место: номер в ctx и расстояние; ctx.dangerCount — нет.
std::size_t nearestDanger(const Context& ctx, float& nearest) {
    std::size_t best = ctx.dangerCount;
    for (std::size_t i = 0; i < ctx.dangerCount; ++i) {
        const float d = distance(ctx.x, ctx.y, ctx.dangerX[i], ctx.dangerY[i]);
        if (best == ctx.dangerCount || d < nearest) {
            best = i;
            nearest = d;
        }
    }
    return best;
}

// Запомнить, где замечен зомби: обновить место рядом или занять то, что
// забудется раньше всех (при равных — первое).
void rememberDanger(Mind& mind, Cell cell, std::uint64_t tick, std::uint64_t until) {
    std::size_t slot = 0;
    bool found = false;
    for (std::size_t i = 0; i < Mind::kDangerMemory && !found; ++i) {
        const Danger& danger = mind.dangers[i];
        if (danger.until > tick && std::abs(danger.cell.x - cell.x) <= kDangerMergeCells &&
            std::abs(danger.cell.y - cell.y) <= kDangerMergeCells) {
            slot = i;
            found = true;
        }
    }
    if (!found) {
        for (std::size_t i = 1; i < Mind::kDangerMemory; ++i) {
            if (mind.dangers[i].until < mind.dangers[slot].until) {
                slot = i;
            }
        }
    }
    mind.dangers[slot] = {cell, until};
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
    if (ctx.carry.food() > 0) {
        return here(ctx, need);
    }
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food() > 0) {
        return atHome(ctx, need);
    }
    return {};
}

Option evalDrink(const Context& ctx) {
    const float need = kDrinkWeight * urge(ctx.needs.thirst, needStart(ctx));
    if (need <= 0.0f) {
        return {};
    }
    if (ctx.carry.count(ItemKind::Water) > 0) {
        return here(ctx, need);
    }
    Option best;
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->count(ItemKind::Water) > 0) {
        best = atHome(ctx, need);
    }
    // К воде — только если она заметно ближе дома (или дома воды нет).
    if (best.score < need * 0.9f) {
        const World& world = ctx.world;
        // Безопасной воды нет — от жажды и к опасной (её оценка ниже, evaluate).
        auto spot = world.places().nearestWater(world.nav(), ctx.cell, kWaterSearch,
                                                [&ctx](Cell cell) { return !risky(ctx, cell); });
        if (!spot && ctx.dangerCount > 0) {
            spot = world.places().nearestWater(world.nav(), ctx.cell, kWaterSearch);
        }
        if (spot) {
            const float d = distance(ctx.x, ctx.y, cellX(*spot), cellY(*spot));
            best = better(best, {need * nearness(d, kNearScale), *spot, kInvalidId});
        }
    }
    return best;
}

Option evalScavenge(const Context& ctx) {
    const bool foodAtHand =
        ctx.carry.food() > 0 || (ctx.homeSupplies != nullptr && ctx.homeSupplies->food() > 0);
    const bool waterAtHand =
        ctx.carry.count(ItemKind::Water) > 0 ||
        (ctx.homeSupplies != nullptr && ctx.homeSupplies->count(ItemKind::Water) > 0);
    const float start = needStart(ctx);
    // Еды в обысках подряд не нашлось — округа обобрана, за едой тянет меньше.
    const float foodHope = std::max(
        kEmptySearchFloor, std::pow(kEmptySearchDecay, static_cast<float>(ctx.emptySearches)));
    float need = 0.0f;
    if (!foodAtHand) {
        need = std::max(need, kScavengeWeight * urge(ctx.needs.hunger, start) * foodHope);
    }
    if (!waterAtHand) {
        need = std::max(need, kScavengeWaterWeight * urge(ctx.needs.thirst, start));
    }
    // Запасливый идёт за едой, пока дома кончается, а не когда уже голоден;
    // несомое домой тоже считается, с полными руками не ходят.
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food() + ctx.carry.food() < kStockLow &&
        !ctx.night) {
        need = std::max(need, kStockWeight * ctx.traits.prudence * foodHope);
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
    if (ctx.carry.food() > 0) {
        return {};
    }
    // Сезон сказывается через запас леса (Resources): зимой он пуст.
    float need = kForageWeight * urge(ctx.needs.hunger, needStart(ctx));
    if (need <= 0.0f) {
        return {};
    }
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food() > 0) {
        need *= 0.4f; // дома есть еда — в лес незачем
    }
    if (ctx.night) {
        need *= kNightPenalty;
    }
    const World& world = ctx.world;
    const Resources& resources = world.resources();
    const auto spot = world.places().nearestForage(
        world.nav(), ctx.cell, kForageSearch,
        [&](Cell cell) { return resources.forestAt(cell) > 0 && !risky(ctx, cell); });
    if (!spot) {
        return {};
    }
    const float d = distance(ctx.x, ctx.y, cellX(*spot), cellY(*spot));
    return {need * nearness(d, kForageScale), *spot, kInvalidId};
}

// Насколько тянет к человеку: незнакомец — 1, друг — больше, неприятный —
// меньше; недруг — 0, к нему не идут.
float warmth(const Context& ctx, EntityId other) {
    const Bond* bond = ctx.relations != nullptr ? ctx.relations->find(other) : nullptr;
    if (bond == nullptr) {
        return 1.0f;
    }
    if (bond->hostile()) {
        return 0.0f;
    }
    return std::max(kMinWarmth, 1.0f + kFriendWarmth * bond->affinity * bond->familiarity);
}

// К людям: к другу или приятелю (их ищут дальше), к ближайшему, кто не спит,
// а никого рядом — на площадь своего поселения. Недругов обходят.
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
    const auto awake = [&registry](entt::entity entity) {
        const Person* person = registry.try_get<Person>(entity);
        return person != nullptr && !person->asleep;
    };
    Option best;
    if (ctx.relations != nullptr) {
        for (std::size_t i = 0; i < ctx.relations->count; ++i) {
            const Bond& bond = ctx.relations->bonds[i];
            const entt::entity other = world.find(bond.other);
            if (bond.affinity < kVisitAffinity || other == entt::null || !awake(other)) {
                continue;
            }
            const Position& at = registry.get<Position>(other);
            const float d = distance(ctx.x, ctx.y, at.x, at.y);
            if (d <= kFriendSearch) {
                best = better(best, {need * warmth(ctx, bond.other) * nearness(d, kSocialScale),
                                     Map::cellAt(at.x, at.y), bond.other});
            }
        }
    }
    const auto other = world.spatial().nearest(
        ctx.x, ctx.y, kCompanySearch, [&](const SpatialHit& hit) {
            return hit.id != ctx.id && awake(hit.entity) && warmth(ctx, hit.id) > 0.0f;
        });
    if (other) {
        const float d = std::sqrt(other->distanceSq);
        best = better(best, {need * warmth(ctx, other->id) * nearness(d, kSocialScale),
                             Map::cellAt(other->x, other->y), other->id});
    }
    // Площадь — если идти не к кому: пустая площадь рядом не лучше друга вдали.
    if (best.score > 0.0f) {
        return best;
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

// Куда бежать, выберется при выполнении. Дома не бегут, а прячутся.
Option evalFlee(const Context& ctx) {
    if (ctx.person.indoors) {
        return {};
    }
    const float timidity = kFleeBase - kFleeBravery * ctx.traits.bravery;
    if (ctx.threat.level >= kFleeMinThreat) {
        return here(ctx, ctx.threat.level * timidity);
    }
    // Зомби далеко или уже не видно, но был здесь — отойти.
    float d = 0.0f;
    if (nearestDanger(ctx, d) < ctx.dangerCount && d < kDangerRadius) {
        return here(ctx, kWaryWeight * (1.0f - d / kDangerRadius) * timidity);
    }
    return {};
}

// Драться с ближайшим зомби: с оружием, смело и против одного — охотнее.
// Дома не дерутся, а прячутся.
Option evalFight(const Context& ctx) {
    if (ctx.person.indoors || ctx.threat.nearest == kInvalidId ||
        ctx.threat.level < kFleeMinThreat) {
        return {};
    }
    const Traits& traits = ctx.traits;
    const float resolve = kResolveBase + kResolveBravery * traits.bravery +
                          kResolveAggression * traits.aggression;
    const float healthy = 1.0f - kFightWounded * (1.0f - ctx.needs.health);
    const float crowd = 1.0f + kCrowdPenalty * static_cast<float>(ctx.threat.count - 1);
    const float score = ctx.threat.level * (kFightBase + weaponPower(ctx.weapon)) * resolve *
                        healthy / crowd;
    return {score, ctx.cell, ctx.threat.nearest};
}

// Охота: голодный с ружьём идёт к ближайшему зверю. Где именно стрелять —
// решается по ходу (updateHunt).
Option evalHunt(const Context& ctx) {
    if (!isFirearm(ctx.weapon.kind) || ctx.weapon.ammo == 0 || ctx.carry.food() > 0) {
        return {};
    }
    float need = kHuntWeight * urge(ctx.needs.hunger, needStart(ctx));
    if (need <= 0.0f) {
        return {};
    }
    if (ctx.homeSupplies != nullptr && ctx.homeSupplies->food() > 0) {
        need *= 0.4f; // дома есть еда
    }
    if (ctx.night) {
        need *= kNightPenalty;
    }
    const World& world = ctx.world;
    const entt::registry& registry = world.registry();
    const auto game = world.spatial().nearest(ctx.x, ctx.y, kHuntSearch, [&](const SpatialHit& hit) {
        return registry.all_of<Animal>(hit.entity) && !risky(ctx, Map::cellAt(hit.x, hit.y));
    });
    if (!game) {
        return {};
    }
    const float d = std::sqrt(game->distanceSq);
    return {need * nearness(d, kHuntScale), Map::cellAt(game->x, game->y), game->id};
}

// Перевязаться, если течёт кровь и есть бинт; принять лекарство, если плохо
// и оно есть. На месте.
Option evalTreat(const Context& ctx) {
    if (ctx.bleeding > kTreatBleeding && ctx.carry.count(ItemKind::Bandage) > 0) {
        return here(ctx, std::min(1.0f, 0.45f + 2.0f * ctx.bleeding));
    }
    if (ctx.needs.health < kMedicineBelow && ctx.carry.count(ItemKind::Medicine) > 0) {
        return here(ctx, 0.6f * (1.0f - ctx.needs.health));
    }
    return {};
}

// Бегом, а не шагом: угроза настоящая.
bool mustRun(const Context& ctx) {
    return ctx.threat.level >= kFleeMinThreat;
}

Option evaluateRaw(const Context& ctx, Action action) {
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
    case Action::Flee:
        return evalFlee(ctx);
    case Action::Fight:
        return evalFight(ctx);
    case Action::Hunt:
        return evalHunt(ctx);
    case Action::Treat:
        return evalTreat(ctx);
    case Action::Count:
        break;
    }
    return {};
}

// Не идти туда, где недавно видел зомби, и не оставаться там. Без этого
// жажда раз за разом вела к реке мимо зомби, человек убегал, через минуту шёл
// снова и так и не напивался.
Option evaluate(const Context& ctx, Action action) {
    Option option = evaluateRaw(ctx, action);
    if (action != Action::Flee && action != Action::Fight && option.score > 0.0f &&
        risky(ctx, option.spot)) {
        option.score *= kDangerPenalty;
    }
    return option;
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

// Тело с вещами рядом, которое ещё не обыскивали, — первым (этап 5.6). Иначе
// здание, где ещё не искали: из трёх случайных — ближайшее. Сначала в своём
// поселении, потом в соседних.
std::optional<Places::BuildingPlace> pickScavengeTarget(const Context& ctx, const Mind& mind,
                                                        Rng& rng) {
    const World& world = ctx.world;
    const entt::registry& registry = world.registry();
    if (const auto body = world.spatial().nearest(
            ctx.x, ctx.y, kBodySearch, [&](const SpatialHit& hit) {
                const auto* corpse = registry.try_get<Corpse>(hit.entity);
                if (corpse == nullptr || corpse->kind == CorpseKind::Animal ||
                    remembered(mind, hit.id)) {
                    return false;
                }
                const auto* stuff = registry.try_get<Inventory>(hit.entity);
                const auto* weapon = registry.try_get<Weapon>(hit.entity);
                const bool loot = (stuff != nullptr && !stuff->empty()) ||
                                  (weapon != nullptr && weapon->kind != WeaponKind::Fists);
                const Cell cell = Map::cellAt(hit.x, hit.y);
                return loot && world.nav().passable(cell) && !risky(ctx, cell);
            })) {
        return Places::BuildingPlace{body->id, BuildingKind::House, Map::cellAt(body->x, body->y)};
    }
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
            if (building.id != ctx.homeId && !remembered(mind, building.id) &&
                !risky(ctx, building.door)) {
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
    const Cell door =
        settlement->buildings[rng.below(static_cast<std::uint32_t>(settlement->buildings.size()))]
            .door;
    if (risky(ctx, door)) {
        return std::nullopt;
    }
    return door;
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
        // Восприятие обновилось — не появился ли рядом зомби. Ждать пересмотра
        // через 10 минут нельзя: за это время зомби проходит сотню метров.
        if ((tick_ + id_) % kPerceptionIntervalTicks == 0) {
            const Threat threat = threatOf(world_, entity_);
            if (threat.nearest != kInvalidId) {
                rememberDanger(mind_, Map::cellAt(threat.nearestX, threat.nearestY), tick_,
                               tick_ + minutes(kDangerMemoryMinutes));
            }
            if (mind_.action != Action::Flee && mind_.action != Action::Fight &&
                threat.level >= kFleeMinThreat && react()) {
                return;
            }
        }
        // Бой и охота идут своим ходом: цель движется, а место действия — за ней.
        if (mind_.action == Action::Fight) {
            updateFight();
            return;
        }
        if (mind_.action == Action::Hunt) {
            updateHunt();
            return;
        }
        const Walker& walker = registry_.get<Walker>(entity_);
        if (mind_.phase == ActionPhase::Travel) {
            if (!walker.moving()) {
                arrive();
            } else if (tick_ >= mind_.nextThink) {
                // Бегущий решает заново, только если путь стал вести к зомби:
                // новый путь начинается с центра своей клетки, и частые
                // перепрокладки держали бы плывущего на месте.
                if (mind_.action == Action::Flee) {
                    if (risky(makeContext(world_, entity_), mind_.spot)) {
                        think();
                    } else {
                        mind_.nextThink = tick_ + minutes(kFleeThinkMinutes);
                    }
                } else {
                    reconsider();
                }
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

    // Зомби рядом: бросить дело и бежать или драться — что важнее, — если это
    // важнее текущего с обычной надбавкой. Надбавка сна здесь не действует —
    // угроза будит.
    bool react() {
        if (person_.indoors) {
            return false; // дома прячется
        }
        const Context ctx = makeContext(world_, entity_);
        const Option flee = evalFlee(ctx);
        const Option fight = evalFight(ctx);
        const bool brave = fight.score > flee.score;
        const Option& best = brave ? fight : flee;
        float current = evaluate(ctx, mind_.action).score;
        if (mind_.action == Action::Wander || mind_.action == Action::Idle) {
            current = std::max(current, mind_.score);
        }
        if (best.score <= current + kInertia) {
            return false;
        }
        return tryStart(ctx, brave ? Action::Fight : Action::Flee, best);
    }

    // Бой: раз в обновление восприятия — с кем драться, подойти или стоять и
    // бить (стрелять); не бежать ли. Угрозы нет — бой кончился.
    void updateFight() {
        Combat& combat = registry_.get<Combat>(entity_);
        if ((tick_ + id_) % kPerceptionIntervalTicks != 0 && tick_ < mind_.until) {
            return;
        }
        const Context ctx = makeContext(world_, entity_);
        const Option fight = evalFight(ctx);
        if (ctx.threat.nearest == kInvalidId || fight.score <= 0.0f) {
            combat.target = kInvalidId;
            finish();
            think();
            return;
        }
        const Option flee = evalFlee(ctx);
        if (flee.score > fight.score + kInertia && tryStart(ctx, Action::Flee, flee)) {
            return;
        }
        // С кем бьётся — пока тот жив и на виду; иначе с ближайшим.
        const Awareness& awareness = registry_.get<Awareness>(entity_);
        EntityId target = combat.target;
        const Contact* contact = target != kInvalidId ? awareness.find(target) : nullptr;
        const entt::entity foe = world_.find(target);
        if (contact == nullptr || !isFresh(*contact, tick_) || foe == entt::null ||
            !registry_.all_of<Zombie>(foe)) {
            target = ctx.threat.nearest;
            contact = awareness.find(target);
        }
        if (contact == nullptr) {
            combat.target = kInvalidId;
            finish();
            think();
            return;
        }
        const AttackSpec& spec = attackSpec(ctx.weapon);
        const float d = distance(ctx.x, ctx.y, contact->x, contact->y);
        const Cell goal = Map::cellAt(contact->x, contact->y);
        if (spec.ranged) {
            // Стрелок стоит и ждёт, пока зомби подойдёт на верный выстрел.
            stopWalking();
            combat.target = contact->seen && d <= spec.reach ? target : kInvalidId;
        } else {
            combat.target = target;
            Walker& walker = registry_.get<Walker>(entity_);
            if (d <= spec.reach) {
                stopWalking();
            } else if (!walker.moving() || walker.waypoints.back() != goal) {
                walker.pace = 1.0f;
                startWalkDirect(world_, entity_, goal);
            }
        }
        mind_.target = target;
        mind_.spot = goal;
        mind_.score = fight.score;
        mind_.until = tick_ + minutes(kFightMinutes);
        person_.indoors = false;
    }

    // Охота: идти за зверем, увидев на верном выстреле — стоять и стрелять;
    // подстрелил — дойти до туши и разделать. Не вышло за отведённое время,
    // кончились патроны, зверь ушёл далеко — бросить и отдохнуть.
    void updateHunt() {
        Combat& combat = registry_.get<Combat>(entity_);
        Walker& walker = registry_.get<Walker>(entity_);
        if (mind_.phase == ActionPhase::Perform) { // разделывает тушу
            if (tick_ >= mind_.until) {
                finish();
                think();
            }
            return;
        }
        const bool look = (tick_ + id_) % kPerceptionIntervalTicks == 0;
        if (!look && walker.moving()) {
            return;
        }
        if (tick_ >= mind_.until) {
            giveUpHunt();
            return;
        }
        const entt::entity prey = world_.find(mind_.target);
        if (prey == entt::null) {
            giveUpHunt();
            return;
        }
        const Position& self = registry_.get<Position>(entity_);
        const Position& at = registry_.get<Position>(prey);
        const Cell goal = Map::cellAt(at.x, at.y);
        if (const auto* carcass = registry_.try_get<Corpse>(prey)) {
            combat.target = kInvalidId;
            if (carcass->kind != CorpseKind::Animal || carcass->meat == 0) {
                giveUpHunt();
                return;
            }
            const Cell cell = cellOf(self);
            if (std::abs(cell.x - goal.x) <= 1 && std::abs(cell.y - goal.y) <= 1) {
                stopWalking();
                mind_.spot = cell;
                walker.pace = 1.0f;
                perform(); // разделать
                return;
            }
            if (!walker.moving() || mind_.spot != goal) {
                mind_.spot = goal;
                walker.pace = 1.0f;
                if (!startWalk(world_, entity_, goal)) {
                    giveUpHunt();
                }
            }
            return;
        }
        const Weapon* weapon = registry_.try_get<Weapon>(entity_);
        if (!registry_.all_of<Animal>(prey) || weapon == nullptr || !isFirearm(weapon->kind) ||
            weapon->ammo == 0) {
            giveUpHunt();
            return;
        }
        const AttackSpec& spec = attackSpec(*weapon);
        const float d = distance(self.x, self.y, at.x, at.y);
        const Contact* contact = registry_.get<Awareness>(entity_).find(mind_.target);
        if (contact != nullptr && isFresh(*contact, tick_) && contact->seen && d <= spec.reach) {
            stopWalking();
            combat.target = mind_.target; // целится и стреляет
            return;
        }
        combat.target = kInvalidId;
        if (d > kHuntSearch * 1.5f) {
            giveUpHunt();
            return;
        }
        if (!walker.moving() ||
            distance(cellX(mind_.spot), cellY(mind_.spot), at.x, at.y) > kTrackRegoal) {
            mind_.spot = goal;
            walker.pace = kStalkPace;
            if (!startWalkDirect(world_, entity_, goal)) {
                giveUpHunt();
            }
        }
    }

    void giveUpHunt() {
        registry_.get<Combat>(entity_).target = kInvalidId;
        fail();
    }

    // Куда бежать: домой, если он в той стороне, иначе прочь от зомби (или от
    // места, где его видел) или чуть в сторону. Возвращает клетку, куда уже пошёл.
    std::optional<Cell> startFlee(const Context& ctx) {
        float awayX = ctx.threat.awayX;
        float awayY = ctx.threat.awayY;
        float d = 0.0f;
        if (const std::size_t i = nearestDanger(ctx, d); !mustRun(ctx) && i < ctx.dangerCount) {
            awayX = d > 1e-3f ? (ctx.x - ctx.dangerX[i]) / d : 0.0f;
            awayY = d > 1e-3f ? (ctx.y - ctx.dangerY[i]) / d : -1.0f;
        }
        if (ctx.homeId != kInvalidId && !ctx.atHome && ctx.homeDistance > 1.0f &&
            ctx.homeDistance <= kFleeHomeRange) {
            const float hx = (cellX(ctx.homeDoor) - ctx.x) / ctx.homeDistance;
            const float hy = (cellY(ctx.homeDoor) - ctx.y) / ctx.homeDistance;
            if (hx * awayX + hy * awayY >= kFleeHomeAlignment &&
                startWalk(world_, entity_, ctx.homeDoor)) {
                return ctx.homeDoor;
            }
        }
        const NavGrid& nav = world_.nav();
        for (const auto& turn : kFleeTurns) {
            const float dx = awayX * turn[0] - awayY * turn[1];
            const float dy = awayX * turn[1] + awayY * turn[0];
            const auto cell = nav.nearestPassable(
                Map::cellAt(ctx.x + dx * kFleeDistance, ctx.y + dy * kFleeDistance), 3);
            if (!cell || *cell == ctx.cell || !nav.connected(ctx.cell, *cell)) {
                continue;
            }
            if (startWalk(world_, entity_, *cell)) {
                return cell;
            }
        }
        return std::nullopt;
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
        } else if (action == Action::Fight) {
            option.spot = ctx.cell; // подойти к зомби — по ходу боя (updateFight)
            // В руки — лучшее, что есть: ружьё без патронов хуже топора.
            equipBest(registry_.get<Weapon>(entity_), registry_.get<Inventory>(entity_));
        }

        bool here = option.spot == ctx.cell;
        bool walking = false;
        if (action == Action::Flee) {
            const auto spot = startFlee(ctx);
            if (!spot) {
                return false;
            }
            option.spot = *spot;
            option.target = kInvalidId;
            here = false;
            walking = true;
            if (mustRun(ctx)) {
                if (mind_.fledAt == 0 || tick_ >= mind_.fledAt + minutes(kFleeEpisodeMinutes)) {
                    world_.record(ChronicleKind::Fled, id_, ctx.threat.nearest,
                                  person_.asleep ? 1 : 0);
                }
                mind_.fledAt = tick_;
            }
        }
        if (!here && !walking && !startWalk(world_, entity_, option.spot)) {
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
        registry_.get<Walker>(entity_).pace = action == Action::Flee && mustRun(ctx) ? kRunPace
                                              : action == Action::Hunt              ? kStalkPace
                                                                                    : 1.0f;
        registry_.get<Combat>(entity_).target =
            action == Action::Fight ? option.target : kInvalidId;
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
            person_.indoors = false;
            mind_.nextThink =
                tick_ + minutes(action == Action::Flee ? kFleeThinkMinutes : kThinkMinutes);
            if (action == Action::Hunt) {
                mind_.until = tick_ + minutes(kHuntMinutes); // сколько выслеживать
            }
        }
        return true;
    }

    // Дома, в укрытии: выполняет действие у своей двери.
    void updateShelter() {
        bool indoors = false;
        if (mind_.phase == ActionPhase::Perform) {
            if (const entt::entity home = world_.find(person_.home); home != entt::null) {
                indoors = mind_.spot == registry_.get<Building>(home).door;
            }
        }
        person_.indoors = indoors;
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
        updateShelter();
    }

    void arrive() {
        const Cell cell = cellOf(registry_.get<Position>(entity_));
        // Дома — выложить найденное в запасы.
        if (const entt::entity home = world_.find(person_.home); home != entt::null) {
            if (cell == registry_.get<Building>(home).door) {
                // Еду, мясо и воду — в запасы; снаряжение остаётся при себе.
                Inventory& carry = registry_.get<Inventory>(entity_);
                if (auto* stock = registry_.try_get<Inventory>(home)) {
                    const int food = carry.food();
                    const int water = carry.count(ItemKind::Water);
                    if (food > 0 || water > 0) {
                        world_.record(ChronicleKind::BroughtHome, id_, person_.home, food, water);
                    }
                    for (const ItemKind kind : {ItemKind::Food, ItemKind::Meat, ItemKind::Water}) {
                        stock->add(kind, carry.take(kind, 0xFFFF));
                    }
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
        updateShelter();
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
            duration = mind_.target == kInvalidId &&
                               registry_.get<Inventory>(entity_).count(ItemKind::Water) == 0
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
        case Action::Flee:
            duration = rng_.rangeInt(kFleeMinMinutes, kFleeMaxMinutes);
            break;
        case Action::Fight:
            duration = kFightMinutes;
            person_.indoors = false; // дерётся у порога — на виду
            break;
        case Action::Hunt:
            duration = kButcherMinutes;
            break;
        case Action::Treat:
            duration = kTreatMinutes;
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
        Inventory& carry = registry_.get<Inventory>(entity_);
        // Здание или тело действия.
        Inventory* stock = nullptr;
        if (const entt::entity target = world_.find(mind_.target); target != entt::null) {
            stock = registry_.try_get<Inventory>(target);
        }
        switch (mind_.action) {
        case Action::Eat:
            if (eatFrom(carry)) {
                needs_.hunger = clamp01(needs_.hunger - kMeal);
            } else if (stock != nullptr && eatFrom(*stock)) {
                needs_.hunger = clamp01(needs_.hunger - kMeal);
                if (stock->food() == 0) {
                    world_.record(ChronicleKind::AteLastFood, id_, mind_.target);
                }
            }
            break;
        case Action::Drink:
            if (carry.take(ItemKind::Water, 1) == 1) {
                needs_.thirst = clamp01(needs_.thirst - kWaterPortion);
            } else if (mind_.target == kInvalidId) {
                needs_.thirst = 0.0f; // у реки или озера — вдоволь
            } else if (stock != nullptr && stock->take(ItemKind::Water, 1) == 1) {
                needs_.thirst = clamp01(needs_.thirst - kWaterPortion);
                if (stock->count(ItemKind::Water) == 0) {
                    world_.record(ChronicleKind::DrankLastWater, id_, mind_.target);
                }
            }
            break;
        case Action::Treat:
            treat(carry);
            break;
        case Action::Scavenge:
            remember(mind_, mind_.target);
            if (stock != nullptr) {
                takeLoot(*stock, carry);
            } else {
                world_.record(ChronicleKind::Scavenged, id_, mind_.target);
                countSearch(false);
            }
            break;
        case Action::Forage:
            forage(carry);
            break;
        case Action::Hunt:
            if (mind_.phase == ActionPhase::Perform) {
                butcher(carry);
            }
            break;
        default:
            break;
        }
    }

    // Разделать тушу: голодный съедает порцию, остальное — с собой, сколько
    // унесёт; что не унести, остаётся в туше.
    void butcher(Inventory& carry) {
        const entt::entity prey = world_.find(mind_.target);
        auto* carcass = prey != entt::null ? registry_.try_get<Corpse>(prey) : nullptr;
        if (carcass == nullptr || carcass->meat == 0) {
            return;
        }
        int taken = 0;
        if (needs_.hunger > kSnackThreshold) {
            --carcass->meat;
            ++taken;
            needs_.hunger = clamp01(needs_.hunger - kMeal);
        }
        const int carried = std::max(0, std::min({static_cast<int>(carcass->meat),
                                                  kCarryFoodMax - carry.food(),
                                                  roomFor(carry, ItemKind::Meat)}));
        carcass->meat = static_cast<std::uint8_t>(carcass->meat - carried);
        carry.add(ItemKind::Meat, carried);
        taken += carried;
        world_.record(ChronicleKind::Butchered, id_, mind_.target, taken);
    }

    // Найденное в здании или на теле: еду и воду — сколько позволяют жадность и
    // запасливость (что-то съесть и выпить сразу), затем снаряжение (takeGear).
    // Что не унести, остаётся на месте.
    void takeLoot(Inventory& stock, Inventory& carry) {
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float greed = traits != nullptr ? traits->greed : 0.5f;
        const float prudence = traits != nullptr ? traits->prudence : 0.5f;
        const int wantFood = 1 + static_cast<int>(std::lround(2.0f * greed + 2.0f * prudence));
        const int wantWater = 1 + static_cast<int>(std::lround(greed + prudence));
        // Мясо — первым: оно портится.
        int meat = stock.take(ItemKind::Meat, wantFood);
        int food = stock.take(ItemKind::Food, wantFood - meat);
        int water = stock.take(ItemKind::Water, wantWater);
        const entt::entity source = world_.find(mind_.target);
        const bool body = source != entt::null && registry_.all_of<Corpse>(source);
        world_.record(body ? ChronicleKind::LootedBody : ChronicleKind::Scavenged, id_,
                      mind_.target, meat + food, water);
        countSearch(meat + food > 0);
        if (meat + food > 0 && needs_.hunger > kSnackThreshold) {
            --(meat > 0 ? meat : food);
            needs_.hunger = clamp01(needs_.hunger - kMeal);
        }
        if (water > 0 && needs_.thirst > kSnackThreshold) {
            --water;
            needs_.thirst = clamp01(needs_.thirst - kWaterPortion);
        }
        const auto keep = [&](ItemKind kind, int found, int cap) {
            const int kept = std::max(0, std::min({found, cap, roomFor(carry, kind)}));
            carry.add(kind, kept);
            stock.add(kind, found - kept);
        };
        keep(ItemKind::Meat, meat, kCarryFoodMax - carry.food());
        keep(ItemKind::Food, food, kCarryFoodMax - carry.food());
        keep(ItemKind::Water, water, kCarryWaterMax - carry.count(ItemKind::Water));
        takeGear(stock, carry, body ? source : entt::entity{entt::null});
    }

    // Снаряжение: оружие сильнее того, что в руках, патроны к своему оружию,
    // бинты и лекарства (запасливый — больше). Оружие в руках у
    // погибшего — тоже добыча. Новое оружие — в руки, прежнее остаётся на месте.
    void takeGear(Inventory& stock, Inventory& carry, entt::entity body) {
        if (body != entt::null) {
            if (auto* dropped = registry_.try_get<Weapon>(body);
                dropped != nullptr && dropped->kind != WeaponKind::Fists) {
                stock.add(itemOf(dropped->kind), 1);
                if (const ItemKind ammo = ammoFor(dropped->kind); ammo != ItemKind::Count) {
                    stock.add(ammo, dropped->ammo);
                }
                *dropped = {};
            }
        }
        const auto move = [&](ItemKind kind, int want) {
            const int moved = stock.take(kind, std::max(0, std::min(want, roomFor(carry, kind))));
            carry.add(kind, moved);
        };
        Weapon& weapon = registry_.get<Weapon>(entity_);
        // Лучшее оружие из найденного — если сильнее того, что в руках (с патронами).
        Weapon mine = weapon;
        if (const ItemKind ammo = ammoFor(mine.kind); ammo != ItemKind::Count) {
            mine.ammo = static_cast<std::uint16_t>(mine.ammo + carry.count(ammo));
        }
        ItemKind found = ItemKind::Count;
        float best = weaponPower(mine);
        for (std::size_t i = 0; i < kItemKinds; ++i) {
            const auto item = static_cast<ItemKind>(i);
            const WeaponKind kind = weaponOf(item);
            if (kind == WeaponKind::Fists || stock.count(item) == 0) {
                continue;
            }
            const ItemKind ammo = ammoFor(kind);
            const int rounds = ammo != ItemKind::Count ? stock.count(ammo) + carry.count(ammo) : 0;
            const float power = weaponPower(Weapon{kind, static_cast<std::uint16_t>(rounds)});
            if (power > best) {
                best = power;
                found = item;
            }
        }
        if (found != ItemKind::Count) {
            move(found, 1);
        }
        // Патроны ко всему своему оружию.
        for (const WeaponKind kind : {WeaponKind::Pistol, WeaponKind::Shotgun, WeaponKind::Rifle}) {
            if (weapon.kind == kind || carry.count(itemOf(kind)) > 0) {
                move(ammoFor(kind), 0xFFFF);
            }
        }
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float prudence = traits != nullptr ? traits->prudence : 0.5f;
        const int bandages = kBandageMin + static_cast<int>(std::lround(kBandageSpread * prudence));
        const int medicine = kMedicineMin + static_cast<int>(std::lround(kMedicineSpread * prudence));
        move(ItemKind::Bandage, bandages - carry.count(ItemKind::Bandage));
        move(ItemKind::Medicine, medicine - carry.count(ItemKind::Medicine));
        const WeaponKind before = weapon.kind;
        if (equipBest(weapon, carry)) {
            world_.record(ChronicleKind::FoundWeapon, id_, mind_.target,
                          static_cast<std::int32_t>(weapon.kind));
            // Прежнее — тяжёлое и хуже: оставить там, где нашёл новое.
            if (const ItemKind old = itemOf(before); old != ItemKind::Count) {
                stock.add(old, carry.take(old, 1));
            }
        }
    }

    // Перевязаться (бинт останавливает кровотечение) или принять лекарство.
    void treat(Inventory& carry) {
        Wounds* wounds = registry_.try_get<Wounds>(entity_);
        if (wounds != nullptr && wounds->bleeding > kTreatBleeding &&
            carry.take(ItemKind::Bandage, 1) == 1) {
            wounds->bleeding = 0.0f;
            world_.record(ChronicleKind::Treated, id_, kInvalidId, 0);
        } else if (needs_.health < kMedicineBelow && carry.take(ItemKind::Medicine, 1) == 1) {
            needs_.health = clamp01(needs_.health + kMedicineHeal);
            world_.record(ChronicleKind::Treated, id_, kInvalidId, 1);
        }
    }

    // Обыски подряд без еды: с каждым искать еду в зданиях хочется меньше.
    void countSearch(bool found) {
        if (found) {
            mind_.emptySearches = 0;
        } else if (mind_.emptySearches < 255) {
            ++mind_.emptySearches;
        }
    }

    // Сбор из запаса леса: порцию съесть на месте, запасливый уносит ещё одну.
    // Пока шёл, лес могли обобрать другие — тогда ничего.
    void forage(Inventory& carry) {
        Resources& resources = world_.resources();
        if (!resources.takeForest(mind_.spot)) {
            return;
        }
        needs_.hunger = clamp01(needs_.hunger - kForageMeal);
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float prudence = traits != nullptr ? traits->prudence : 0.5f;
        if (carry.food() < kCarryFoodMax && roomFor(carry, ItemKind::Food) > 0 &&
            resources.forestAt(mind_.spot) > 0 && rng_.chance(prudence)) {
            resources.takeForest(mind_.spot);
            carry.add(ItemKind::Food, 1);
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
