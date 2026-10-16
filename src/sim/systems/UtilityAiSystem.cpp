#include "sim/systems/UtilityAiSystem.h"

#include "sim/Calendar.h"
#include "sim/Items.h"
#include "sim/Memory.h"
#include "sim/Perception.h"
#include "sim/Places.h"
#include "sim/Weapons.h"
#include "sim/World.h"
#include "sim/Relations.h"
#include "sim/systems/GroupSystem.h"
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
// К знакомым с симпатией не ниже kVisitAffinity идут дальше (этап 6.2).
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
// К товарищу по группе тянет ещё на столько (этап 6.4).
constexpr float kGroupWarmth = 0.3f;
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
// Зомби напал на товарища по группе или близкого (этап 6.4) — драться охотнее:
// оценка × (1 + kDefendBonus × верность), верность — kGroupLoyalty у товарища
// плюс близость, не больше 1.
constexpr float kDefendBonus = 0.8f;
constexpr float kGroupLoyalty = 0.5f;
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

// --- Социальные действия (этап 6.5) --------------------------------------
// Совесть: жильё знакомых (не недругов) и товарищей не обыскивают, порядочный
// (альтруизм не ниже kHonestAltruism) не трогает и жильё незнакомцев;
// отчаявшийся (голод не ниже kDesperate) — любое.
constexpr float kHonestAltruism = 0.6f;
constexpr float kDesperate = 0.9f;
// Поделиться с тем, кто голоден или хочет пить (не меньше kNeedyLevel), ранен
// или плох, а у него нет ни при себе, ни дома, — если он дорог: близкий (не
// меньше kCloseEnough), товарищ по группе или тот, кому должен (долг не меньше
// kOweEnough). Оценка — kShareWeight × нужда × щедрость × near(d, kShareScale),
// щедрость — kGenerosityBase + альтруизм и долг, близость, товарищ, минус жадность.
constexpr float kShareWeight = 0.8f;
constexpr float kShareRange = 1000.0f;
constexpr float kShareScale = 600.0f;
constexpr float kNeedyLevel = 0.5f;
constexpr float kOweEnough = 0.1f;
constexpr float kGenerosityBase = 0.2f;
constexpr int kShareMinutes = 5;
// Отдал тому, кому был должен, — долг меньше на столько.
constexpr float kRepaid = 0.3f;
// Встреча с человеком: ближе kMeetRange — отдать или меняться; на руки — не
// дальше kHandRange; ушёл от места встречи дальше max(kFollowRegoal, 10 %
// расстояния) — путь заново.
constexpr float kMeetRange = 4.0f;
constexpr float kHandRange = 12.0f;
constexpr float kFollowRegoal = 8.0f;
// Обмен — с тем, кого знает (знакомство не меньше kTradeFamiliarity) и кому
// доверяет (не меньше kTradeTrust), до kTradeRange. Оценка — kTradeWeight ×
// нужда × (0,6 + 0,4 × доверие) × near(d, kTradeScale): сделка с тем, у кого
// нужное точно есть, не хуже обыска наугад. Честная сделка —
// шаг навстречу: доверие и симпатия обоих растут.
constexpr float kTradeWeight = 1.3f;
constexpr float kTradeScale = 1000.0f;
constexpr float kTradeRange = 800.0f;
constexpr float kTradeFamiliarity = 0.3f;
constexpr float kTradeTrust = 0.1f;
constexpr int kTradeMinutes = 10;
constexpr float kTradeTrustGain = 0.05f;
constexpr float kTradeAffinityGain = 0.03f;
// Хозяин дома меняется и домашними запасами, оставив домашним столько еды и
// воды; бинтов и лекарств хочет про запас — меньше стольких.
constexpr int kTradeHomeKeep = 4;
constexpr int kTradeWantBandages = 3;
constexpr int kTradeWantMedicine = 2;
// Предательство — обокрасть дом того, кто доверяет (kBetrayTrust), пока его
// нет или он спит. Оценка — kStealWeight × желание × (0,5 + подлость) × near(d),
// подлость — жадность − 0,5 × альтруизм − kStealConscience − kStealLoyalty ×
// знакомство × симпатия (к кому нет тёплых чувств, того не жалко, как бы тот
// ни доверял). Желание — голод, если еды нет, иначе kGreedTemptation × жадность.
constexpr float kStealWeight = 0.9f;
constexpr float kStealConscience = 0.1f;
constexpr float kStealLoyalty = 0.6f;
constexpr float kGreedTemptation = 0.35f;
constexpr int kStealMinFood = 3;
constexpr int kStealMinutes = 15;
// Сбежать из группы с её запасами: вожак не мил (симпатия ниже
// kDesertAffinity), на базе есть что взять; верность товарищам — kGroupFaith.
constexpr float kDesertAffinity = 0.1f;
constexpr int kDesertMinFood = 4;
constexpr float kDesertTemptation = 0.6f;
constexpr float kGroupFaith = 0.3f;
// Месть — обидчику, к которому обида не меньше kAvengeGrudge, живому и не
// дальше kAvengeRange. Оценка — kAvengeWeight × ярость × нрав ×
// (1 − 0,5 × альтруизм) × смелость × near(d, kAvengeScale); ярость — обида /
// kAvengeRage, не больше 1 (на пороге — уже заметна), нрав — 0,2 +
// 0,8 × агрессия, смелость — от смелости и того, чьё оружие сильнее.
constexpr float kAvengeWeight = 1.0f;
constexpr float kAvengeGrudge = 0.35f;
constexpr float kAvengeRage = 0.7f;
constexpr float kAvengeRange = 1500.0f;
constexpr float kAvengeScale = 1000.0f;
constexpr float kAvengeMinHealth = 0.6f;   // раненый не мстит
constexpr float kAvengeQuitHealth = 0.35f; // сам изранен — бросает
constexpr int kAvengeMinutes = 120;        // сколько искать
constexpr float kAvengePace = 1.2f;        // идёт быстрым шагом
// Избил (здоровье обидчика не выше kBeatenHealth) — отомстил, если не хотел
// убить; хотел (обида × (0,5 + агрессия) не меньше kMurderous) — бьёт до смерти.
constexpr float kBeatenHealth = 0.5f;
constexpr float kMurderous = 0.9f;
// Отомстив, обида уходит: избил — остаётся kRelief, убил — ничего. Не вышло
// (убежал, не нашёл) — остаётся kFrustration: остывает понемногу.
constexpr float kRelief = 0.4f;
constexpr float kFrustration = 0.85f;
// Обидчик убегает дальше этого — догонять бесполезно, метры.
constexpr float kAvengeLostRange = 30.0f;

std::uint64_t minutes(int count) {
    return static_cast<std::uint64_t>(count) * kTicksPerGameMinute;
}

// Хочет ли мститель убить обидчика: обида × (0,5 + агрессия) не меньше kMurderous.
bool murderous(float grudge, float aggression) {
    return grudge * (0.5f + aggression) >= kMurderous;
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
    // Кого знает (этап 6.2); nullptr — никого.
    const Relations* relations = nullptr;
    // Группа (этап 6.4); kInvalidId — сам по себе.
    EntityId group = kInvalidId;
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
    ctx.group = person.group;
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

// Товарищ ли other по группе (этап 6.4).
bool groupMate(const Context& ctx, EntityId other) {
    if (ctx.group == kInvalidId) {
        return false;
    }
    const entt::entity entity = ctx.world.find(other);
    const auto* person = entity != entt::null ? ctx.world.registry().try_get<Person>(entity) : nullptr;
    return person != nullptr && person->group == ctx.group;
}

// Насколько тянет к человеку: незнакомец — 1, друг — больше, неприятный —
// меньше; недруг — 0, к нему не идут. К товарищу по группе — ещё теплее.
float warmth(const Context& ctx, EntityId other) {
    const float mate = groupMate(ctx, other) ? kGroupWarmth : 0.0f;
    const Bond* bond = ctx.relations != nullptr ? ctx.relations->find(other) : nullptr;
    if (bond == nullptr) {
        return 1.0f + mate;
    }
    if (bond->hostile()) {
        return 0.0f;
    }
    return std::max(kMinWarmth, 1.0f + kFriendWarmth * bond->affinity * bond->familiarity) + mate;
}

// Насколько человек верен other — для заступничества: товарищ по группе и
// близость, 0..1.
float loyaltyTo(const Context& ctx, EntityId other) {
    const Bond* bond = ctx.relations != nullptr ? ctx.relations->find(other) : nullptr;
    const float close = bond != nullptr && !bond->hostile() ? closeness(*bond) : 0.0f;
    return std::min(1.0f, close + (groupMate(ctx, other) ? kGroupLoyalty : 0.0f));
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

// Куда бежать, выберется при выполнении. Дома не бегут, а прячутся — разве
// что бьёт человек: от него за дверью не укрыться (этап 6.5).
Option evalFlee(const Context& ctx) {
    if (ctx.person.indoors && ctx.threat.assailant == kInvalidId) {
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

// Драться с ближайшим зомби (или человеком, который бьёт его или того, кто
// дорог, — этап 6.5): с оружием, смело и против одного — охотнее. Дома от
// зомби не дерутся, а прячутся.
Option evalFight(const Context& ctx) {
    if ((ctx.person.indoors && ctx.threat.assailant == kInvalidId) ||
        ctx.threat.nearest == kInvalidId || ctx.threat.level < kFleeMinThreat) {
        return {};
    }
    const Traits& traits = ctx.traits;
    const float resolve = kResolveBase + kResolveBravery * traits.bravery +
                          kResolveAggression * traits.aggression;
    const float healthy = 1.0f - kFightWounded * (1.0f - ctx.needs.health);
    const float crowd = 1.0f + kCrowdPenalty * static_cast<float>(ctx.threat.count - 1);
    float score = ctx.threat.level * (kFightBase + weaponPower(ctx.weapon)) * resolve *
                  healthy / crowd;
    // Зомби напал на своего — заступиться (этап 6.4).
    const entt::registry& registry = ctx.world.registry();
    if (const entt::entity foe = ctx.world.find(ctx.threat.nearest); foe != entt::null) {
        EntityId victim = kInvalidId;
        if (const auto* zombie = registry.try_get<Zombie>(foe); zombie != nullptr &&
            zombie->state == ZombieState::Chase) {
            victim = zombie->target;
        }
        if (const auto* combat = registry.try_get<Combat>(foe);
            victim == kInvalidId && combat != nullptr) {
            victim = combat->target;
        }
        if (victim != kInvalidId && victim != ctx.id) {
            score *= 1.0f + kDefendBonus * loyaltyTo(ctx, victim);
        }
    }
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

// --- Социальные действия (этап 6.5) ------------------------------------------

// Живой человек с таким id; entt::null — нет.
entt::entity personAt(const World& world, EntityId id) {
    const entt::entity entity = world.find(id);
    return entity != entt::null && world.registry().all_of<Person>(entity) ? entity
                                                                           : entt::entity{entt::null};
}

// Запасы в доме человека; nullptr — дома нет.
const Inventory* homeStock(const World& world, entt::entity person) {
    const entt::entity home = world.find(world.registry().get<Person>(person).home);
    return home != entt::null ? world.registry().try_get<Inventory>(home) : nullptr;
}

// Подарок: что дать, сколько и насколько это нужно получателю (0..1).
// ItemKind::Food — еда вообще: при передаче сначала мясо (портится).
struct Gift {
    ItemKind kind = ItemKind::Count;
    int count = 0;
    float need = 0.0f;
    bool fromHome = false; // своего при себе нет — взять из домашних запасов (стоит у двери)
};

// Сколько вида kind человек (ctx) может отдать из того, что несёт, не обделив себя.
int spareCarried(const Context& ctx, ItemKind kind) {
    switch (kind) {
    case ItemKind::Food:
        return ctx.carry.food() - (ctx.needs.hunger > 0.6f ? 1 : 0);
    case ItemKind::Water:
        return ctx.carry.count(ItemKind::Water) - (ctx.needs.thirst > 0.6f ? 1 : 0);
    case ItemKind::Bandage:
        return ctx.carry.count(ItemKind::Bandage) - (ctx.bleeding > kTreatBleeding ? 1 : 0);
    case ItemKind::Medicine:
        return ctx.carry.count(ItemKind::Medicine) - (ctx.needs.health < kMedicineBelow ? 1 : 0);
    default:
        break;
    }
    return 0;
}

// Сколько еды или воды можно взять из дома, оставив себе две порции; только у двери.
int spareAtHome(const Context& ctx, ItemKind kind) {
    if (!ctx.atHome || ctx.homeSupplies == nullptr) {
        return 0;
    }
    if (kind == ItemKind::Food) {
        return ctx.homeSupplies->food() - 2;
    }
    return kind == ItemKind::Water ? ctx.homeSupplies->count(ItemKind::Water) - 2 : 0;
}

// Что ctx может дать receiver: самое нужное тому из того, что у него есть.
Gift giftFor(const Context& ctx, entt::entity receiver) {
    const entt::registry& registry = ctx.world.registry();
    const Needs* needs = registry.try_get<Needs>(receiver);
    const Inventory* carry = registry.try_get<Inventory>(receiver);
    if (needs == nullptr || carry == nullptr) {
        return {};
    }
    const Inventory* stock = homeStock(ctx.world, receiver);
    const auto* wounds = registry.try_get<Wounds>(receiver);
    Gift best;
    const auto consider = [&](ItemKind kind, int wanted, float need) {
        if (need <= best.need) {
            return;
        }
        const int carried = spareCarried(ctx, kind);
        const int stored = carried > 0 ? 0 : spareAtHome(ctx, kind);
        const int count = std::min(wanted, std::max(carried, stored));
        if (count > 0) {
            best = {kind, count, need, carried <= 0};
        }
    };
    if (needs->hunger >= kNeedyLevel && carry->food() == 0 && (stock == nullptr || stock->food() == 0)) {
        consider(ItemKind::Food, 2, urge(needs->hunger, kNeedyLevel));
    }
    if (needs->thirst >= kNeedyLevel && carry->count(ItemKind::Water) == 0 &&
        (stock == nullptr || stock->count(ItemKind::Water) == 0)) {
        consider(ItemKind::Water, 2, urge(needs->thirst, kNeedyLevel));
    }
    if (wounds != nullptr && wounds->bleeding > kTreatBleeding && carry->count(ItemKind::Bandage) == 0) {
        consider(ItemKind::Bandage, 1, std::min(1.0f, 0.45f + 2.0f * wounds->bleeding));
    }
    if (needs->health < kMedicineBelow && carry->count(ItemKind::Medicine) == 0) {
        consider(ItemKind::Medicine, 1, 0.6f * (1.0f - needs->health));
    }
    return best;
}

// Поделиться с тем, кто дорог и в беде: отнести ему еду, воду, бинт, лекарство.
Option evalShare(const Context& ctx) {
    if (ctx.relations == nullptr || ctx.night) {
        return {};
    }
    const entt::registry& registry = ctx.world.registry();
    const Traits& traits = ctx.traits;
    const float kindness = kGenerosityBase + 0.6f * traits.altruism - 0.3f * traits.greed;
    Option best;
    for (std::size_t i = 0; i < ctx.relations->count; ++i) {
        const Bond& bond = ctx.relations->bonds[i];
        if (bond.hostile()) {
            continue;
        }
        const bool mate = groupMate(ctx, bond.other);
        const float close = closeness(bond);
        if (!mate && close < kCloseEnough && bond.debt < kOweEnough) {
            continue;
        }
        const entt::entity other = personAt(ctx.world, bond.other);
        if (other == entt::null) {
            continue;
        }
        const Position& at = registry.get<Position>(other);
        const float d = distance(ctx.x, ctx.y, at.x, at.y);
        if (d > kShareRange) {
            continue;
        }
        const Gift gift = giftFor(ctx, other);
        if (gift.count == 0) {
            continue;
        }
        const float generosity = std::clamp(
            kindness + bond.debt + 0.5f * close + (mate ? kGroupFaith : 0.0f), 0.0f, 1.2f);
        best = better(best, {kShareWeight * gift.need * generosity * nearness(d, kShareScale),
                             Map::cellAt(at.x, at.y), bond.other});
    }
    return best;
}

// Ценность предмета в порциях еды — для обмена.
float itemValue(ItemKind kind) {
    switch (kind) {
    case ItemKind::Food:
        return 1.0f;
    case ItemKind::Water:
    case ItemKind::Meat:
        return 0.8f;
    case ItemKind::Bandage:
        return 1.5f;
    case ItemKind::Medicine:
        return 3.0f;
    case ItemKind::PistolAmmo:
        return 0.25f;
    case ItemKind::ShotgunShells:
        return 0.35f;
    case ItemKind::RifleAmmo:
        return 0.4f;
    case ItemKind::Knife:
        return 2.0f;
    case ItemKind::Axe:
        return 3.0f;
    case ItemKind::Pistol:
        return 8.0f;
    case ItemKind::Shotgun:
        return 10.0f;
    case ItemKind::Rifle:
        return 12.0f;
    case ItemKind::Count:
        break;
    }
    return 0.0f;
}

// Есть ли у человека ружьё под эти патроны — в руках или в инвентаре.
bool ownsGunFor(const Weapon& weapon, const Inventory& carry, ItemKind ammo) {
    for (const WeaponKind kind : {WeaponKind::Pistol, WeaponKind::Shotgun, WeaponKind::Rifle}) {
        if (ammoFor(kind) == ammo && (weapon.kind == kind || carry.count(itemOf(kind)) > 0)) {
            return true;
        }
    }
    return false;
}

// Домашние запасы, из которых человек меняется: только когда он дома, у своей
// двери; иначе nullptr — меняется тем, что при себе.
const Inventory* tradeHome(const World& world, entt::entity person) {
    return world.registry().get<Person>(person).indoors ? homeStock(world, person) : nullptr;
}

// Сколько вида kind человек отдаст в обмен — сверх того, что оставит себе. home
// — домашние запасы (tradeHome): из дома отдаёт, оставив домашним kTradeHomeKeep
// еды и воды.
int spareFor(const entt::registry& registry, entt::entity person, ItemKind kind,
             const Inventory* home = nullptr) {
    const Inventory& carry = registry.get<Inventory>(person);
    const Weapon& weapon = registry.get<Weapon>(person);
    const int count = carry.count(kind) + (home != nullptr ? home->count(kind) : 0);
    switch (kind) {
    case ItemKind::Food:
    case ItemKind::Meat:
    case ItemKind::Water:
        return count - (home != nullptr ? kTradeHomeKeep : 2);
    case ItemKind::Bandage:
    case ItemKind::Medicine:
        return count - 1;
    case ItemKind::PistolAmmo:
    case ItemKind::ShotgunShells:
    case ItemKind::RifleAmmo:
        return ownsGunFor(weapon, carry, kind) ? 0 : count;
    default:
        return count; // оружие в инвентаре — лишнее: в руках другое
    }
}

// Нужен ли человеку предмет kind: того, что нужно, мало или нет (бинтов и
// лекарств хочется про запас — меньше kTradeWantBandages и kTradeWantMedicine),
// оружие — сильнее своего. home — домашние запасы (tradeHome): считаются вместе
// с тем, что при себе.
bool welcomes(const entt::registry& registry, entt::entity person, ItemKind kind,
              const Inventory* home = nullptr) {
    const Inventory& carry = registry.get<Inventory>(person);
    const Weapon& weapon = registry.get<Weapon>(person);
    const auto total = [&](ItemKind item) {
        return carry.count(item) + (home != nullptr ? home->count(item) : 0);
    };
    switch (kind) {
    case ItemKind::Food:
    case ItemKind::Meat:
        return carry.food() + (home != nullptr ? home->food() : 0) < 2;
    case ItemKind::Water:
        return total(ItemKind::Water) < 2;
    case ItemKind::Bandage:
        return total(kind) < kTradeWantBandages;
    case ItemKind::Medicine:
        return total(kind) < kTradeWantMedicine;
    case ItemKind::PistolAmmo:
    case ItemKind::ShotgunShells:
    case ItemKind::RifleAmmo:
        return ownsGunFor(weapon, carry, kind) && weapon.ammo + carry.count(kind) < 15;
    default:
        break;
    }
    const WeaponKind offered = weaponOf(kind);
    if (offered == WeaponKind::Fists) {
        return false;
    }
    const ItemKind ammo = ammoFor(offered);
    const int rounds = ammo != ItemKind::Count ? carry.count(ammo) : 0;
    return weaponPower(Weapon{offered, static_cast<std::uint16_t>(rounds)}) > weaponPower(weapon);
}

// Чего человеку не хватает больше всего: вид, сколько и как сильно (0..1).
// ItemKind::Food — еда вообще (припасы или мясо).
struct Want {
    ItemKind kind = ItemKind::Count;
    int count = 0;
    float need = 0.0f;
};

Want wantOf(const Context& ctx) {
    Want want;
    const auto consider = [&](ItemKind kind, int count, float need) {
        if (need > want.need) {
            want = {kind, count, need};
        }
    };
    const bool homeFood = ctx.homeSupplies != nullptr && ctx.homeSupplies->food() > 0;
    const bool homeWater = ctx.homeSupplies != nullptr && ctx.homeSupplies->count(ItemKind::Water) > 0;
    if (ctx.carry.food() == 0 && !homeFood) {
        consider(ItemKind::Food, 2, urge(ctx.needs.hunger, needStart(ctx)));
    }
    if (ctx.carry.count(ItemKind::Water) == 0 && !homeWater) {
        consider(ItemKind::Water, 2, urge(ctx.needs.thirst, needStart(ctx)));
    }
    if (ctx.needs.health < kMedicineBelow && ctx.carry.count(ItemKind::Medicine) == 0) {
        consider(ItemKind::Medicine, 1, 0.6f * (1.0f - ctx.needs.health));
    }
    if (ctx.bleeding > kTreatBleeding && ctx.carry.count(ItemKind::Bandage) == 0) {
        consider(ItemKind::Bandage, 1, std::min(1.0f, 0.45f + 2.0f * ctx.bleeding));
    }
    if (const ItemKind ammo = ammoFor(ctx.weapon.kind);
        ammo != ItemKind::Count && ctx.weapon.ammo + ctx.carry.count(ammo) < 3) {
        consider(ammo, 8, 0.3f);
    }
    return want;
}

// Что other даст на нужду want: еду — сначала припасы, потом мясо. Count — нечего.
ItemKind provides(const World& world, entt::entity other, ItemKind want) {
    const entt::registry& registry = world.registry();
    const Inventory* home = tradeHome(world, other);
    if (want == ItemKind::Food) {
        return spareFor(registry, other, ItemKind::Food, home) > 0   ? ItemKind::Food
               : spareFor(registry, other, ItemKind::Meat, home) > 0 ? ItemKind::Meat
                                                                    : ItemKind::Count;
    }
    return spareFor(registry, other, want, home) > 0 ? want : ItemKind::Count;
}

// Что предложить other за want: из своего лишнего — самое ценное, что тому
// нужно. Count — нечего.
ItemKind offerFor(const World& world, entt::entity self, entt::entity other, ItemKind want) {
    const entt::registry& registry = world.registry();
    const Inventory* home = tradeHome(world, other);
    ItemKind best = ItemKind::Count;
    for (std::size_t i = 0; i < kItemKinds; ++i) {
        const auto kind = static_cast<ItemKind>(i);
        const bool same = kind == want || (want == ItemKind::Food && kind == ItemKind::Meat);
        if (same || spareFor(registry, self, kind) <= 0 || !welcomes(registry, other, kind, home)) {
            continue;
        }
        if (best == ItemKind::Count || itemValue(kind) > itemValue(best)) {
            best = kind;
        }
    }
    return best;
}

// Обменять лишнее на нужное с тем, кому доверяет.
Option evalTrade(const Context& ctx) {
    if (ctx.relations == nullptr || ctx.night) {
        return {};
    }
    const Want want = wantOf(ctx);
    if (want.need <= 0.0f) {
        return {};
    }
    const entt::registry& registry = ctx.world.registry();
    Option best;
    for (std::size_t i = 0; i < ctx.relations->count; ++i) {
        const Bond& bond = ctx.relations->bonds[i];
        if (bond.hostile() || bond.familiarity < kTradeFamiliarity || bond.trust < kTradeTrust) {
            continue;
        }
        const entt::entity other = personAt(ctx.world, bond.other);
        if (other == entt::null || registry.get<Person>(other).asleep) {
            continue;
        }
        const Position& at = registry.get<Position>(other);
        const float d = distance(ctx.x, ctx.y, at.x, at.y);
        if (d > kTradeRange || provides(ctx.world, other, want.kind) == ItemKind::Count ||
            offerFor(ctx.world, ctx.entity, other, want.kind) == ItemKind::Count) {
            continue;
        }
        best = better(best, {kTradeWeight * want.need * (0.6f + 0.4f * bond.trust) *
                                 nearness(d, kTradeScale),
                             Map::cellAt(at.x, at.y), bond.other});
    }
    return best;
}

// Предательство: обокрасть дом того, кто доверяет, пока его нет или он спит;
// или сбежать из группы, прихватив запасы базы, если вожак не мил.
Option evalSteal(const Context& ctx) {
    if (ctx.relations == nullptr) {
        return {};
    }
    const Traits& traits = ctx.traits;
    const float wickedness = traits.greed - 0.5f * traits.altruism - kStealConscience;
    if (wickedness <= 0.0f) {
        return {};
    }
    const World& world = ctx.world;
    const entt::registry& registry = world.registry();
    const bool fed = ctx.carry.food() > 0 || (ctx.homeSupplies != nullptr && ctx.homeSupplies->food() > 0);
    const float desire =
        std::max(fed ? 0.0f : urge(ctx.needs.hunger, needStart(ctx)), kGreedTemptation * traits.greed);
    Option best;
    for (std::size_t i = 0; i < ctx.relations->count; ++i) {
        const Bond& bond = ctx.relations->bonds[i];
        const entt::entity other = personAt(world, bond.other);
        if (other == entt::null || groupMate(ctx, bond.other)) {
            continue; // своих по группе — только сбежав (ниже)
        }
        const auto* theirs = registry.try_get<Relations>(other);
        const Bond* back = theirs != nullptr ? theirs->find(ctx.id) : nullptr;
        if (back == nullptr || back->trust < kBetrayTrust) {
            continue;
        }
        const Person& owner = registry.get<Person>(other);
        const entt::entity home = world.find(owner.home);
        if (home == entt::null || owner.home == ctx.homeId || (owner.indoors && !owner.asleep)) {
            continue; // хозяин дома и не спит
        }
        const auto* stock = registry.try_get<Inventory>(home);
        const float wicked =
            wickedness - kStealLoyalty * bond.familiarity * std::max(0.0f, bond.affinity);
        if (stock == nullptr || stock->food() < kStealMinFood || wicked <= 0.0f) {
            continue;
        }
        const Cell door = registry.get<Building>(home).door;
        const float d = distance(ctx.x, ctx.y, cellX(door), cellY(door));
        best = better(best,
                      {kStealWeight * desire * (0.5f + wicked) * nearness(d, kNearScale), door, owner.home});
    }
    // Сбежать из группы с запасами базы.
    if (const entt::entity band = world.find(ctx.group);
        band != entt::null && ctx.homeSupplies != nullptr &&
        ctx.homeSupplies->food() >= kDesertMinFood) {
        const EntityId leader = registry.get<Group>(band).leader;
        const Bond* toLeader = ctx.relations->find(leader);
        const float liking = toLeader != nullptr ? toLeader->affinity : 0.0f;
        const float wicked = wickedness - kGroupFaith - std::max(0.0f, liking);
        if (leader != ctx.id && liking < kDesertAffinity && wicked > 0.0f) {
            best = better(best, {kStealWeight * kDesertTemptation * traits.greed * wicked *
                                     nearness(ctx.homeDistance, kNearScale),
                                 ctx.homeDoor, ctx.homeId});
        }
    }
    return best;
}

// Месть: обидчик жив и недалеко, обида сильна, нрав горячий, а сам не трус и
// не ранен.
Option evalAvenge(const Context& ctx) {
    if (ctx.relations == nullptr || ctx.needs.health < kAvengeMinHealth) {
        return {};
    }
    const entt::registry& registry = ctx.world.registry();
    const Traits& traits = ctx.traits;
    const float temper = (0.2f + 0.8f * traits.aggression) * (1.0f - 0.5f * traits.altruism);
    Option best;
    for (std::size_t i = 0; i < ctx.relations->count; ++i) {
        const Bond& bond = ctx.relations->bonds[i];
        if (bond.grudge < kAvengeGrudge) {
            continue;
        }
        const entt::entity other = personAt(ctx.world, bond.other);
        if (other == entt::null) {
            continue;
        }
        const Position& at = registry.get<Position>(other);
        const float d = distance(ctx.x, ctx.y, at.x, at.y);
        if (d > kAvengeRange) {
            continue;
        }
        // Уже избит, а убивать не хочет — отомщено: после расправы обида
        // (× kRelief) может остаться выше порога, но второй раз не бьют.
        if (const auto* needs = registry.try_get<Needs>(other);
            needs != nullptr && needs->health <= kBeatenHealth &&
            !murderous(bond.grudge, traits.aggression)) {
            continue;
        }
        const auto* theirs = registry.try_get<Weapon>(other);
        const float courage = std::clamp(
            (0.5f + traits.bravery) * (0.4f + weaponPower(ctx.weapon)) /
                (0.4f + (theirs != nullptr ? weaponPower(*theirs) : 0.15f)),
            0.3f, 1.5f);
        const float rage = std::min(1.0f, bond.grudge / kAvengeRage);
        best = better(best, {kAvengeWeight * rage * temper * courage *
                                 nearness(d, kAvengeScale),
                             Map::cellAt(at.x, at.y), bond.other});
    }
    return best;
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
    case Action::Share:
        return evalShare(ctx);
    case Action::Trade:
        return evalTrade(ctx);
    case Action::Steal:
        return evalSteal(ctx);
    case Action::Avenge:
        return evalAvenge(ctx);
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
    // Совесть (этап 6.5): чьё это жильё. Жильцы — по дому, затем по id.
    std::vector<std::pair<EntityId, EntityId>> residents;
    for (const auto [entity, id, person] : registry.view<const Id, const Person>().each()) {
        if (person.home != kInvalidId && id.value != ctx.id) {
            residents.emplace_back(person.home, id.value);
        }
    }
    std::sort(residents.begin(), residents.end());
    const bool desperate = ctx.needs.hunger >= kDesperate;
    const bool honest = ctx.traits.altruism >= kHonestAltruism;
    const auto allowed = [&](EntityId building) {
        if (desperate) {
            return true;
        }
        auto it = std::lower_bound(residents.begin(), residents.end(),
                                   std::pair<EntityId, EntityId>{building, 0});
        for (; it != residents.end() && it->first == building; ++it) {
            const Bond* bond = ctx.relations != nullptr ? ctx.relations->find(it->second) : nullptr;
            if (honest || groupMate(ctx, it->second) || (bond != nullptr && !bond->hostile())) {
                return false; // живут знакомые или свои; порядочный не тронет никого
            }
        }
        return true;
    };
    std::vector<const Places::SettlementPlace*> order{own};
    for (const EntityId neighbour : own->neighbours) {
        order.push_back(places.settlement(neighbour));
    }
    std::vector<const Places::BuildingPlace*> candidates;
    for (const Places::SettlementPlace* settlement : order) {
        candidates.clear();
        for (const Places::BuildingPlace& building : settlement->buildings) {
            if (building.id != ctx.homeId && !remembered(mind, building.id) &&
                !risky(ctx, building.door) && allowed(building.id)) {
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
        if (mind_.action == Action::Avenge) {
            updateAvenge();
            return;
        }
        const Walker& walker = registry_.get<Walker>(entity_);
        if (mind_.phase == ActionPhase::Travel) {
            // К человеку (поделиться, обменяться): он может уйти — идти за ним.
            if ((mind_.action == Action::Share || mind_.action == Action::Trade) &&
                (tick_ + id_) % kPerceptionIntervalTicks == 0 && follow()) {
                return;
            }
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
        const Context ctx = makeContext(world_, entity_);
        if (person_.indoors && ctx.threat.assailant == kInvalidId) {
            return false; // дома прячется: бьёт не человек
        }
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
        if (contact == nullptr || !isFresh(*contact, tick_) || !isFoe(foe)) {
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

    // С кем драться: зомби или человек, который бьёт людей (этап 6.5).
    bool isFoe(entt::entity entity) const {
        if (entity == entt::null) {
            return false;
        }
        if (registry_.all_of<Zombie>(entity)) {
            return true;
        }
        const auto* combat = registry_.try_get<Combat>(entity);
        return combat != nullptr && registry_.all_of<Person>(entity) &&
               personAt(world_, combat->target) != entt::null;
    }

    // Идти к человеку (поделиться, обменяться): дошёл — выполнять, он ушёл от
    // места встречи — путь заново. true — решение сменилось.
    bool follow() {
        const entt::entity other = personAt(world_, mind_.target);
        if (other == entt::null) {
            fail();
            return true;
        }
        const Position& self = registry_.get<Position>(entity_);
        const Position& at = registry_.get<Position>(other);
        const float d = distance(self.x, self.y, at.x, at.y);
        if (d <= kMeetRange) {
            stopWalking();
            mind_.spot = cellOf(self);
            perform();
            return true;
        }
        if (distance(cellX(mind_.spot), cellY(mind_.spot), at.x, at.y) >
            std::max(kFollowRegoal, 0.1f * d)) {
            mind_.spot = Map::cellAt(at.x, at.y);
            if (!startWalk(world_, entity_, mind_.spot)) {
                fail();
                return true;
            }
        }
        return false;
    }

    // Месть (этап 6.5): идти к обидчику и, догнав, бить (стрелять). Избил, а
    // убивать не хотел, или убил — отомстил. Не нашёл за отведённое время, сам
    // изранен, обидчик убежал — бросить.
    void updateAvenge() {
        Combat& combat = registry_.get<Combat>(entity_);
        Walker& walker = registry_.get<Walker>(entity_);
        const bool look = (tick_ + id_) % kPerceptionIntervalTicks == 0;
        if (!look && walker.moving()) {
            return;
        }
        const entt::entity foe = world_.find(mind_.target);
        if (foe == entt::null || !registry_.all_of<Person>(foe)) {
            const auto* corpse = foe != entt::null ? registry_.try_get<Corpse>(foe) : nullptr;
            if (corpse != nullptr && corpse->killer == id_) {
                avenged(true);
            } else {
                giveUpAvenge(); // погиб не от его руки или встал зомби
            }
            return;
        }
        if (tick_ >= mind_.until || needs_.health < kAvengeQuitHealth) {
            giveUpAvenge();
            return;
        }
        const Position& self = registry_.get<Position>(entity_);
        const Position& at = registry_.get<Position>(foe);
        const float d = distance(self.x, self.y, at.x, at.y);
        const Mind* theirs = registry_.try_get<Mind>(foe);
        if (d > kAvengeRange * 1.5f ||
            (theirs != nullptr && theirs->action == Action::Flee && d > kAvengeLostRange)) {
            giveUpAvenge();
            return;
        }
        if (const auto* needs = registry_.try_get<Needs>(foe);
            needs != nullptr && needs->health <= kBeatenHealth && !murderous()) {
            avenged(false);
            return;
        }
        const AttackSpec& spec = attackSpec(registry_.get<Weapon>(entity_));
        const Contact* contact = registry_.get<Awareness>(entity_).find(mind_.target);
        const bool seen = contact != nullptr && isFresh(*contact, tick_) && contact->seen;
        // Вплотную бьют и того, кого не видно: дома от мстителя не спрятаться.
        // Стрелять в того, кто заперся, нельзя — уйти ни с чем.
        if (spec.ranged && !seen && d <= kMeetRange) {
            giveUpAvenge();
            return;
        }
        if (spec.ranged ? seen && d <= spec.reach : d <= spec.reach) {
            stopWalking();
            combat.target = mind_.target;
            person_.indoors = false;
            return;
        }
        combat.target = kInvalidId;
        if (!walker.moving() ||
            distance(cellX(mind_.spot), cellY(mind_.spot), at.x, at.y) > std::max(kFollowRegoal, 0.1f * d)) {
            mind_.spot = Map::cellAt(at.x, at.y);
            walker.pace = kAvengePace;
            if (!startWalkDirect(world_, entity_, mind_.spot)) {
                giveUpAvenge();
            }
        }
    }

    // Хочет ли убить того, кому мстит.
    bool murderous() const {
        const auto* relations = registry_.try_get<Relations>(entity_);
        const Bond* bond = relations != nullptr ? relations->find(mind_.target) : nullptr;
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float aggression = traits != nullptr ? traits->aggression : 0.5f;
        return bond != nullptr && lw::murderous(bond->grudge, aggression);
    }

    // Отомстил: обида уходит (избил — остаётся kRelief, убил — ничего).
    void avenged(bool killed) {
        world_.record(ChronicleKind::Avenged, id_, mind_.target, killed ? 1 : 0);
        if (auto* relations = registry_.try_get<Relations>(entity_)) {
            if (Bond* bond = relations->find(mind_.target)) {
                bond->grudge = killed ? 0.0f : bond->grudge * kRelief;
            }
        }
        registry_.get<Combat>(entity_).target = kInvalidId;
        finish();
        think();
    }

    // Не вышло: обида понемногу остывает, иначе мстил бы раз за разом.
    void giveUpAvenge() {
        if (auto* relations = registry_.try_get<Relations>(entity_)) {
            if (Bond* bond = relations->find(mind_.target)) {
                bond->grudge *= kFrustration;
            }
        }
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
        } else if (action == Action::Fight || action == Action::Avenge) {
            if (action == Action::Fight) {
                option.spot = ctx.cell; // подойти к зомби — по ходу боя (updateFight)
            }
            // В руки — лучшее, что есть: ружьё без патронов хуже топора.
            equipBest(registry_.get<Weapon>(entity_), registry_.get<Inventory>(entity_));
        } else if (action == Action::Share) {
            // Своего при себе нет — взять из домашних запасов: стоит у двери.
            const entt::entity other = personAt(world_, option.target);
            const Gift gift = other != entt::null ? giftFor(ctx, other) : Gift{};
            if (gift.count == 0) {
                return false;
            }
            if (gift.fromHome) {
                Inventory& carry = registry_.get<Inventory>(entity_);
                Inventory& stock = registry_.get<Inventory>(world_.find(ctx.homeId));
                for (int i = 0; i < gift.count; ++i) {
                    const ItemKind kind = gift.kind != ItemKind::Food ? gift.kind
                                          : stock.count(ItemKind::Meat) > 0 ? ItemKind::Meat
                                                                             : ItemKind::Food;
                    if (roomFor(carry, kind) > 0 && stock.take(kind, 1) == 1) {
                        carry.add(kind, 1);
                    }
                }
            }
        }

        bool here = option.spot == ctx.cell;
        bool walking = false;
        if (action == Action::Avenge) {
            here = false; // идти за обидчиком — по ходу (updateAvenge)
            walking = true;
            stopWalking();
        }
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
                    // b = 1 — бежал от человека (этап 6.5).
                    world_.record(ChronicleKind::Fled, id_, ctx.threat.nearest,
                                  person_.asleep ? 1 : 0,
                                  personAt(world_, ctx.threat.nearest) != entt::null ? 1 : 0);
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
                                              : action == Action::Avenge            ? kAvengePace
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
            } else if (action == Action::Avenge) {
                mind_.until = tick_ + minutes(kAvengeMinutes); // сколько искать обидчика
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
        case Action::Share:
            duration = kShareMinutes;
            break;
        case Action::Trade:
            duration = kTradeMinutes;
            break;
        case Action::Steal:
            duration = kStealMinutes;
            break;
        case Action::Avenge:
            duration = kFightMinutes;
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
                takeLoot(*stock, carry, false);
            } else {
                world_.record(ChronicleKind::Scavenged, id_, mind_.target);
                countSearch(false);
            }
            break;
        case Action::Steal:
            if (stock != nullptr) {
                const int taken = takeLoot(*stock, carry, true);
                // Обокрал свою базу — сбежал из группы (этап 6.5).
                if (mind_.target == person_.home && person_.group != kInvalidId) {
                    desertGroup(world_, entity_, taken);
                }
            }
            break;
        case Action::Share:
            handOver(carry);
            break;
        case Action::Trade:
            trade(carry);
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
    // Вор (steal, этап 6.5) берёт всё, что унесёт. Что не унести, остаётся на
    // месте. Возвращает, сколько вещей взято.
    int takeLoot(Inventory& stock, Inventory& carry, bool steal) {
        const Traits* traits = registry_.try_get<Traits>(entity_);
        const float greed = traits != nullptr ? traits->greed : 0.5f;
        const float prudence = traits != nullptr ? traits->prudence : 0.5f;
        const int wantFood =
            steal ? kCarryFoodMax : 1 + static_cast<int>(std::lround(2.0f * greed + 2.0f * prudence));
        const int wantWater =
            steal ? kCarryWaterMax : 1 + static_cast<int>(std::lround(greed + prudence));
        const Inventory before = stock;
        // Мясо — первым: оно портится.
        int meat = stock.take(ItemKind::Meat, wantFood);
        int food = stock.take(ItemKind::Food, wantFood - meat);
        int water = stock.take(ItemKind::Water, wantWater);
        const entt::entity source = world_.find(mind_.target);
        const bool body = source != entt::null && registry_.all_of<Corpse>(source);
        if (steal) {
            // Хозяин — живой жилец с меньшим id; своя база — без хозяина (сбежал).
            EntityId owner = kInvalidId;
            for (const auto [entity, id, person] : registry_.view<const Id, const Person>().each()) {
                if (person.home == mind_.target && entity != entity_ &&
                    (owner == kInvalidId || id.value < owner)) {
                    owner = id.value;
                }
            }
            world_.record(ChronicleKind::Stole, id_, mind_.target, meat + food, water, owner);
        } else {
            world_.record(body ? ChronicleKind::LootedBody : ChronicleKind::Scavenged, id_,
                          mind_.target, meat + food, water);
            countSearch(meat + food > 0);
        }
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
        int taken = 0;
        for (std::size_t i = 0; i < kItemKinds; ++i) {
            taken += std::max(0, static_cast<int>(before.items[i]) - stock.items[i]);
        }
        // Хозяева, которые это видели, запомнят вора (этап 6.1).
        if (!body) {
            witnessTheft(world_, entity_, mind_.target, taken);
        }
        return taken;
    }

    // Отдать тому, кому нёс (этап 6.5): если он рядом и ему всё ещё нужно.
    // Получивший обязан; отдавший тому, кому был должен, — вернул долг.
    void handOver(Inventory& carry) {
        const entt::entity other = personAt(world_, mind_.target);
        if (other == entt::null) {
            return;
        }
        const Position& self = registry_.get<Position>(entity_);
        const Position& at = registry_.get<Position>(other);
        if (distance(self.x, self.y, at.x, at.y) > kHandRange) {
            return; // разминулись
        }
        const Gift gift = giftFor(makeContext(world_, entity_), other);
        if (gift.count == 0 || gift.fromHome) {
            return; // уже не нужно или отдать нечего
        }
        Inventory& theirs = registry_.get<Inventory>(other);
        int given = 0;
        for (int i = 0; i < gift.count; ++i) {
            const ItemKind kind = gift.kind != ItemKind::Food ? gift.kind
                                  : carry.count(ItemKind::Meat) > 0 ? ItemKind::Meat
                                                                     : ItemKind::Food;
            if (roomFor(theirs, kind) < 1 || carry.take(kind, 1) != 1) {
                break;
            }
            theirs.add(kind, 1);
            ++given;
        }
        if (given == 0) {
            return;
        }
        bool repaid = false;
        if (auto* relations = registry_.try_get<Relations>(entity_)) {
            if (Bond* bond = relations->find(mind_.target); bond != nullptr && bond->debt > 0.0f) {
                bond->debt = std::max(0.0f, bond->debt - kRepaid);
                repaid = true;
            }
        }
        world_.record(ChronicleKind::Shared, id_, mind_.target,
                      static_cast<std::int32_t>(gift.kind) + (repaid ? 256 : 0), given);
        experience(world_, other, MemoryKind::Helped, id_, kInvalidId, given);
        witnessDeed(world_, MemoryKind::Helped, entity_, mind_.target, kInvalidId);
    }

    // Обменяться с тем, к кому шёл (этап 6.5): своё лишнее — на то, чего не
    // хватает, по ценности; сделка — шаг навстречу для обоих.
    void trade(Inventory& carry) {
        const entt::entity other = personAt(world_, mind_.target);
        if (other == entt::null || registry_.get<Person>(other).asleep) {
            return;
        }
        const Position& self = registry_.get<Position>(entity_);
        const Position& at = registry_.get<Position>(other);
        if (distance(self.x, self.y, at.x, at.y) > kHandRange) {
            return;
        }
        const Want want = wantOf(makeContext(world_, entity_));
        const ItemKind get = want.need > 0.0f ? provides(world_, other, want.kind) : ItemKind::Count;
        const ItemKind give = get != ItemKind::Count ? offerFor(world_, entity_, other, want.kind)
                                                     : ItemKind::Count;
        if (give == ItemKind::Count) {
            return;
        }
        // Хозяин дома меняется и домашними запасами: отдаёт сначала то, что при
        // себе, потом из дома, полученное кладёт домой.
        Inventory& theirs = registry_.get<Inventory>(other);
        const Person& host = registry_.get<Person>(other);
        const entt::entity house = host.indoors ? world_.find(host.home) : entt::null;
        Inventory* theirHome = house != entt::null ? registry_.try_get<Inventory>(house) : nullptr;
        Inventory& theirPlace = theirHome != nullptr ? *theirHome : theirs;
        int getCount =
            std::min({want.count, spareFor(registry_, other, get, theirHome), roomFor(carry, get)});
        int giveCount = static_cast<int>(
            std::ceil(static_cast<float>(getCount) * itemValue(get) / itemValue(give)));
        const int spare = spareFor(registry_, entity_, give);
        if (giveCount > spare) {
            giveCount = spare;
            getCount = std::min(getCount, static_cast<int>(static_cast<float>(giveCount) *
                                                           itemValue(give) / itemValue(get)));
        }
        if (getCount < 1 || giveCount < 1 ||
            (theirHome == nullptr && roomFor(theirs, give) < giveCount)) {
            return;
        }
        const int handed = theirs.take(get, getCount);
        carry.add(get, handed);
        if (theirHome != nullptr) {
            carry.add(get, theirHome->take(get, getCount - handed));
        }
        theirPlace.add(give, carry.take(give, giveCount));
        equipBest(registry_.get<Weapon>(entity_), carry);
        equipBest(registry_.get<Weapon>(other), theirs);
        // Честная сделка — шаг навстречу.
        const auto warm = [&](entt::entity who, EntityId whom) {
            auto* relations = registry_.try_get<Relations>(who);
            Bond* bond = relations != nullptr ? relations->find(whom) : nullptr;
            if (bond != nullptr) {
                bond->trust = std::min(1.0f, bond->trust + kTradeTrustGain);
                bond->affinity = std::min(1.0f, bond->affinity + kTradeAffinityGain);
                refreshBondFlags(world_, registry_.get<Id>(who).value, *bond);
            }
        };
        warm(entity_, mind_.target);
        warm(other, id_);
        world_.record(ChronicleKind::Traded, id_, mind_.target,
                      static_cast<std::int32_t>(get) | (getCount << 8),
                      static_cast<std::int32_t>(give) | (giveCount << 8));
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
