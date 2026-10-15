#include "sim/systems/NeedsSystem.h"

#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/World.h"

#include <algorithm>

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
constexpr float kHotTemperature = 22.0f; // °C
constexpr float kHotThirst = 1.3f;

// Страх тянется к целевому уровню: за час проходит такую долю разницы.
constexpr float kFearResponseHours = 1.0f;
// Темно (освещённость ниже этого), далеко от дома и никого рядом.
constexpr float kDarkLight = 0.15f;
constexpr float kDarkFear = 0.35f;
// Страх от плохого здоровья: при нуле здоровья — столько.
constexpr float kWoundFear = 0.5f;

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

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void updateNeeds(World& world, float minutes) {
    const float hours = minutes / 60.0f;
    const Environment environment = environmentAt(world);
    const bool dark = environment.light < kDarkLight;
    const bool hot = environment.temperature > kHotTemperature;
    entt::registry& registry = world.registry();

    auto view = registry.view<const Id, const Person, Needs, const Walker, const Position>();
    for (const auto [entity, id, person, needs, walker, position] : view.each()) {
        const bool walking = walker.moving();
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

        const float metabolism = person.asleep ? kSleepMetabolism : 1.0f;
        needs.hunger = clamp01(needs.hunger + hours / kHungerHours * metabolism);
        needs.thirst = clamp01(needs.thirst + hours / kThirstHours * metabolism *
                                                  (walking ? kWalkThirst : 1.0f) *
                                                  (hot ? kHotThirst : 1.0f));
        if (person.asleep) {
            needs.fatigue = clamp01(needs.fatigue -
                                    hours / (atHome ? kSleepHomeHours : kSleepOutsideHours));
        } else {
            needs.fatigue =
                clamp01(needs.fatigue + hours / kAwakeHours * (walking ? kWalkFatigue : 1.0f));
        }

        if (company) {
            needs.loneliness = clamp01(needs.loneliness - hours / kCompanyHours);
        } else if (!person.asleep) {
            needs.loneliness = clamp01(needs.loneliness + hours / kLonelyHours);
        }

        float damage = 0.0f;
        damage += needs.hunger >= 1.0f ? hours / kStarveHours : 0.0f;
        damage += needs.thirst >= 1.0f ? hours / kDehydrateHours : 0.0f;
        damage += needs.fatigue >= 1.0f ? hours / kExhaustHours : 0.0f;
        if (damage > 0.0f) {
            needs.health = clamp01(needs.health - damage);
        } else if (needs.hunger < kRecoverHungerMax && needs.thirst < kRecoverThirstMax &&
                   needs.fatigue < kRecoverFatigueMax) {
            needs.health = clamp01(needs.health + hours / kRecoverHours);
        }

        // Страх — после здоровья: раны пугают сразу.
        float fearTarget = kWoundFear * (1.0f - needs.health);
        if (dark && !atHome && !company) {
            fearTarget += kDarkFear;
        }
        needs.fear = clamp01(needs.fear + (fearTarget - needs.fear) *
                                              std::min(1.0f, hours / kFearResponseHours));
    }
}

void tickNeeds(World& world) {
    if (world.tick() % kTicksPerGameMinute == 0) {
        updateNeeds(world, 1.0f);
    }
}

} // namespace lw
