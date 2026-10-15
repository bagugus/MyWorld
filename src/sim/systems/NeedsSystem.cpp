#include "sim/systems/NeedsSystem.h"

#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Death.h"
#include "sim/Perception.h"
#include "sim/World.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Часы до предела (или до нуля) — так константы проще читать и подбирать.
constexpr float kHungerHours = 48.0f;
constexpr float kThirstHours = 30.0f;
constexpr float kAwakeHours = 18.0f;
constexpr float kSleepHomeHours = 7.0f;
constexpr float kSleepOutsideHours = 10.0f;
constexpr float kLonelyHours = 36.0f;
constexpr float kCompanyHours = 3.0f;
// Во сне голод и жажда растут медленнее, в пути и в жару — быстрее.
constexpr float kSleepMetabolism = 0.5f;
constexpr float kWalkThirst = 1.3f;
constexpr float kWalkFatigue = 1.15f;
// Бег (темп выше обычного) выматывает и сушит сильнее.
constexpr float kRunFatigue = 4.0f;
constexpr float kRunThirst = 2.0f;
constexpr float kHotTemperature = 22.0f; // °C
constexpr float kHotThirst = 1.3f;

// Страх тянется к целевому уровню: за час проходит такую долю разницы.
constexpr float kFearResponseHours = 1.0f;
// Темно (освещённость ниже этого), далеко от дома и никого рядом.
constexpr float kDarkLight = 0.15f;
constexpr float kDarkFear = 0.35f;
// Страх от плохого здоровья: при нуле здоровья — столько.
constexpr float kWoundFear = 0.5f;
// Страх от зомби рядом (Threat::level = 1) и как быстро он накатывает, часы.
constexpr float kThreatFear = 0.8f;
constexpr float kThreatResponseHours = 0.1f;

// Выстрел, услышанный недавно, пугает: столько к уровню страха.
constexpr float kGunshotFear = 0.3f;
// Кровотечение (Wounds) за столько часов уменьшается на всю свою величину
// (по чуть-чуть каждую минуту — примерно вдвое за час); слабее этого — остановилось.
constexpr float kClotHours = 1.5f;
constexpr float kBleedingStopped = 0.005f;

// Компания — люди не дальше этого, метры.
constexpr float kCompanyRadius = 20.0f;

// Здоровье: часы от полного до нуля на пределе нужды и время восстановления.
constexpr float kStarveHours = 72.0f;
constexpr float kDehydrateHours = 36.0f;
constexpr float kExhaustHours = 240.0f;
constexpr float kRecoverHours = 48.0f;
// Нужды ниже этого не мешают выздоравливать.
constexpr float kRecoverHungerMax = 0.8f;
constexpr float kRecoverThirstMax = 0.8f;
constexpr float kRecoverFatigueMax = 0.9f;

// Пороги здоровья для летописи.
constexpr float kWeakHealth = 0.5f;
constexpr float kDyingHealth = 0.2f;

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

// Значимые перемены нужд — в летопись: пересечение порога, а не состояние,
// поэтому каждое событие пишется один раз.
void recordChanges(World& world, EntityId id, const Needs& before, const Needs& after) {
    if (before.hunger < 1.0f && after.hunger >= 1.0f) {
        world.record(ChronicleKind::Starving, id);
    }
    if (before.thirst < 1.0f && after.thirst >= 1.0f) {
        world.record(ChronicleKind::Dehydrated, id);
    }
    if (before.fatigue < 1.0f && after.fatigue >= 1.0f) {
        world.record(ChronicleKind::Exhausted, id);
    }
    if (before.health >= kWeakHealth && after.health < kWeakHealth) {
        world.record(ChronicleKind::Weakened, id);
    }
    if (before.health >= kDyingHealth && after.health < kDyingHealth) {
        world.record(ChronicleKind::Dying, id);
    }
    if (before.health < kWeakHealth && after.health >= kWeakHealth) {
        world.record(ChronicleKind::Recovered, id);
    }
}

} // namespace

void updateNeeds(World& world, float minutes) {
    const float hours = minutes / 60.0f;
    const Environment environment = environmentAt(world);
    const bool dark = environment.light < kDarkLight;
    const bool hot = environment.temperature > kHotTemperature;
    entt::registry& registry = world.registry();

    // Истёкшие кровью становятся телами после обхода: компоненты по ходу не снимаются.
    std::vector<std::pair<entt::entity, EntityId>> bledOut;
    auto view = registry.view<const Id, const Person, Needs, const Walker, const Position>();
    for (const auto [entity, id, person, needs, walker, position] : view.each()) {
        const Traits* traits = registry.try_get<Traits>(entity);
        const Traits character = traits != nullptr ? *traits : Traits{};
        const bool walking = walker.moving();
        const bool running = walking && walker.pace > 1.0f;
        bool atHome = false;
        if (const entt::entity home = world.find(person.home); home != entt::null) {
            atHome = Map::cellAt(position.x, position.y) == registry.get<Building>(home).door;
        }
        bool company = false;
        world.spatial().forEachInRadius(position.x, position.y, kCompanyRadius,
                                        [&](const SpatialHit& hit) {
                                            company = company || (hit.id != id.value &&
                                                                  registry.all_of<Person>(hit.entity));
                                        });

        const Needs before = needs;
        const float metabolism = person.asleep ? kSleepMetabolism : 1.0f;
        needs.hunger = clamp01(needs.hunger + hours / kHungerHours * metabolism);
        needs.thirst = clamp01(needs.thirst + hours / kThirstHours * metabolism *
                                                  (walking ? kWalkThirst : 1.0f) *
                                                  (running ? kRunThirst : 1.0f) *
                                                  (hot ? kHotThirst : 1.0f));
        if (person.asleep) {
            needs.fatigue = clamp01(needs.fatigue -
                                    hours / (atHome ? kSleepHomeHours : kSleepOutsideHours));
        } else {
            needs.fatigue = clamp01(needs.fatigue + hours / kAwakeHours *
                                                        (walking ? kWalkFatigue : 1.0f) *
                                                        (running ? kRunFatigue : 1.0f));
        }

        // Общительному одиночество тяжелее (до 1,6 раза), нелюдимому — легче (от 0,4).
        const float sociability = 0.4f + 1.2f * character.sociability;
        if (company) {
            needs.loneliness = clamp01(needs.loneliness - hours / kCompanyHours);
        } else if (!person.asleep) {
            needs.loneliness = clamp01(needs.loneliness + hours / kLonelyHours * sociability);
        }

        float damage = 0.0f;
        damage += needs.hunger >= 1.0f ? hours / kStarveHours : 0.0f;
        damage += needs.thirst >= 1.0f ? hours / kDehydrateHours : 0.0f;
        damage += needs.fatigue >= 1.0f ? hours / kExhaustHours : 0.0f;
        // Кровотечение отнимает здоровье и само понемногу останавливается.
        Wounds* wounds = registry.try_get<Wounds>(entity);
        const bool bleeding = wounds != nullptr && wounds->bleeding > 0.0f;
        if (bleeding) {
            damage += wounds->bleeding * hours;
            wounds->bleeding -= wounds->bleeding * std::min(1.0f, hours / kClotHours);
            if (wounds->bleeding < kBleedingStopped) {
                wounds->bleeding = 0.0f;
            }
        }
        if (damage > 0.0f) {
            needs.health = clamp01(needs.health - damage);
        } else if (needs.hunger < kRecoverHungerMax && needs.thirst < kRecoverThirstMax &&
                   needs.fatigue < kRecoverFatigueMax) {
            needs.health = clamp01(needs.health + hours / kRecoverHours);
        }

        // Страх — после здоровья: раны пугают сразу.
        // Смелый боится вдвое меньше среднего, трус — в полтора раза больше.
        const float timidity = 1.5f - character.bravery;
        float fearTarget = kWoundFear * (1.0f - needs.health) * timidity;
        if (dark && !atHome && !company) {
            fearTarget += kDarkFear * timidity;
        }
        // Зомби рядом пугают сразу, а не за час.
        const Threat threat = threatOf(world, entity);
        fearTarget += kThreatFear * threat.level * timidity;
        if (const auto* awareness = registry.try_get<Awareness>(entity)) {
            for (std::size_t i = 0; i < awareness->count; ++i) {
                const Contact& contact = awareness->contacts[i];
                if (contact.heard == SoundKind::Gunshot && isFresh(contact, world.tick())) {
                    fearTarget += kGunshotFear * timidity;
                    break;
                }
            }
        }
        const bool alarm = threat.level > 0.0f && fearTarget > needs.fear;
        const float response = alarm ? kThreatResponseHours : kFearResponseHours;
        needs.fear =
            clamp01(needs.fear + (fearTarget - needs.fear) * std::min(1.0f, hours / response));
        recordChanges(world, id.value, before, needs);
        if (bleeding && needs.health <= 0.0f) {
            bledOut.emplace_back(entity, wounds->hurtBy);
        }
    }
    for (const auto& [entity, killer] : bledOut) {
        die(world, entity, DeathCause::Bleeding, killer);
    }
}

void tickNeeds(World& world) {
    if (world.tick() % kTicksPerGameMinute == 0) {
        updateNeeds(world, 1.0f);
    }
}

} // namespace lw
