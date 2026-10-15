#include "sim/systems/CombatSystem.h"

#include "sim/Death.h"
#include "sim/Perception.h"
#include "sim/Weapons.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace lw {

namespace {

// Ближний удар достаёт чуть дальше досягаемости: решения принимаются по
// позициям полсекунды назад.
constexpr float kReachSlack = 0.3f;
// В идущего попасть из ружья труднее.
constexpr float kMovingTargetHit = 0.8f;
constexpr float kMovingSpeed = 0.05f; // м/с
// Ночью стреляют хуже, но не вслепую: свет добавляет к шансу не меньше этого.
constexpr float kMinLightHit = 0.3f;
// За пределом уверенного выстрела шанс падает до этой доли у предела оружия.
constexpr float kFarHit = 0.15f;
// Спящего бьют наверняка; дерущийся в ответ уворачивается.
constexpr float kAsleepHit = 0.95f;
constexpr float kDefendedHit = 0.75f;
// Человеку слабость и усталость мешают попадать в ближнем бою.
constexpr float kWeakHit = 0.4f;
constexpr float kTiredHit = 0.3f;
// Раненый кричит: слышно в стольких метрах.
constexpr float kScreamLoudness = 50.0f;
// Урон — средний ± столько.
constexpr float kDamageSpread = 0.3f;

struct Death {
    entt::entity entity;
    DeathCause cause;
    EntityId killer;
};

float distance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

bool moving(const entt::registry& registry, entt::entity entity) {
    const auto* velocity = registry.try_get<Velocity>(entity);
    return velocity != nullptr &&
           velocity->x * velocity->x + velocity->y * velocity->y >= kMovingSpeed * kMovingSpeed;
}

// Шанс выстрела: уверенно до reach, дальше падает до kFarHit у предела.
float rangeFactor(const AttackSpec& spec, float d) {
    if (d <= spec.reach) {
        return 1.0f - 0.3f * d / spec.reach;
    }
    const float t = std::clamp((d - spec.reach) / std::max(1.0f, spec.maxRange - spec.reach), 0.0f, 1.0f);
    return 0.7f + (kFarHit - 0.7f) * t;
}

std::uint64_t intervalTicks(float seconds) {
    return std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::lround(seconds * static_cast<float>(kTicksPerSecond))));
}

} // namespace

const AttackSpec* attackOf(const entt::registry& registry, entt::entity attacker) {
    // Обратившийся носит при себе оружие, но бьёт как зомби.
    if (registry.all_of<Zombie>(attacker)) {
        return &zombieAttack();
    }
    if (const auto* weapon = registry.try_get<Weapon>(attacker)) {
        return &attackSpec(*weapon);
    }
    if (const auto* animal = registry.try_get<Animal>(attacker);
        animal != nullptr && animal->kind == AnimalKind::Boar) {
        return &boarAttack();
    }
    return nullptr;
}

float* healthOf(entt::registry& registry, entt::entity entity) {
    if (auto* needs = registry.try_get<Needs>(entity)) {
        return &needs->health;
    }
    if (auto* zombie = registry.try_get<Zombie>(entity)) {
        return &zombie->health;
    }
    if (auto* animal = registry.try_get<Animal>(entity)) {
        return &animal->health;
    }
    return nullptr;
}

void updateCombat(World& world) {
    entt::registry& registry = world.registry();
    const std::uint64_t tick = world.tick();
    Rng& rng = world.rng(RngStream::Combat);
    std::optional<float> light;
    std::vector<Death> deaths;

    // Порядок — порядок хранения в реестре: он детерминирован. Погибшие
    // становятся телами после обхода: компоненты по ходу не снимаются.
    auto view = registry.view<const Id, Combat, const Position>();
    for (const auto [entity, id, combat, position] : view.each()) {
        if (combat.target == kInvalidId || tick < combat.ready) {
            continue;
        }
        if (const float* own = healthOf(registry, entity); own != nullptr && *own <= 0.0f) {
            continue; // погиб в этом тике
        }
        const entt::entity target = world.find(combat.target);
        float* health = target != entt::null && registry.all_of<Position>(target)
                            ? healthOf(registry, target)
                            : nullptr;
        if (health == nullptr) {
            combat.target = kInvalidId; // цели нет или это уже тело
            continue;
        }
        const AttackSpec* spec = attackOf(registry, entity);
        if (spec == nullptr || *health <= 0.0f) {
            continue;
        }
        const Position& at = registry.get<Position>(target);
        const float d = distance(position.x, position.y, at.x, at.y);
        if (!spec->ranged) {
            if (d > spec->reach + kReachSlack) {
                continue;
            }
        } else if (d > spec->maxRange ||
                   sightCost(world.map(), world.nav(), position.x, position.y, at.x, at.y,
                             spec->maxRange) > spec->maxRange) {
            continue;
        }

        combat.ready = tick + intervalTicks(spec->interval);
        float chance = spec->accuracy;
        if (spec->ranged) {
            Weapon& weapon = registry.get<Weapon>(entity);
            weapon.ammo = static_cast<std::uint16_t>(weapon.ammo - 1);
            world.emitSound(position.x, position.y, spec->loudness, SoundKind::Gunshot, id.value);
            if (!light) {
                light = std::clamp(senseConditions(world).sight, kMinLightHit, 1.0f);
            }
            chance *= rangeFactor(*spec, d) * *light;
            if (moving(registry, target)) {
                chance *= kMovingTargetHit;
            }
        } else {
            if (const auto* needs = registry.try_get<Needs>(entity)) {
                chance *= (1.0f - kWeakHit * (1.0f - needs->health)) *
                          (1.0f - kTiredHit * needs->fatigue);
            }
            if (isAsleep(registry, target)) {
                chance = std::max(chance, kAsleepHit);
            } else if (const auto* mind = registry.try_get<Mind>(target);
                       mind != nullptr && mind->action == Action::Fight) {
                chance *= kDefendedHit;
            }
        }
        if (!rng.chance(chance)) {
            continue;
        }

        // В голову — только зомби и дичи: людей пока никто не убивает с одного удара.
        const bool person = registry.all_of<Person>(target);
        const bool crit = !person && rng.chance(spec->critChance);
        const float damage =
            crit ? *health : spec->damage * rng.range(1.0f - kDamageSpread, 1.0f + kDamageSpread);
        *health = std::max(0.0f, *health - damage);

        if (person) {
            Wounds& wounds = registry.get<Wounds>(target);
            wounds.bleeding += spec->bleeding;
            wounds.hurtAt = tick;
            wounds.hurtBy = id.value;
            if (registry.all_of<Zombie>(entity) && rng.chance(kBiteChance)) {
                ++wounds.bites;
                wounds.bleeding += kBiteBleeding;
                wounds.infection = std::min(1.0f, wounds.infection + kBiteInfection);
                world.record(ChronicleKind::Bitten, combat.target, id.value);
            }
            world.emitSound(at.x, at.y, kScreamLoudness, SoundKind::Scream, combat.target);
        } else if (auto* animal = registry.try_get<Animal>(target)) {
            animal->hurtAt = tick;
            animal->hurtBy = id.value;
        }

        if (*health <= 0.0f) {
            if (const auto* weapon = registry.try_get<Weapon>(entity);
                weapon != nullptr && registry.all_of<Person>(entity)) {
                if (registry.all_of<Zombie>(target)) {
                    world.record(ChronicleKind::KilledZombie, id.value, combat.target,
                                 static_cast<std::int32_t>(weapon->kind));
                } else if (const auto* animal = registry.try_get<Animal>(target)) {
                    world.record(ChronicleKind::Hunted, id.value, combat.target,
                                 static_cast<std::int32_t>(animal->kind));
                }
            }
            deaths.push_back({target, DeathCause::Wounds, id.value});
            combat.target = kInvalidId;
        }
    }

    for (const Death& death : deaths) {
        die(world, death.entity, death.cause, death.killer);
    }
}

} // namespace lw
