#include "sim/systems/LodSystem.h"

#include "sim/Components.h"
#include "sim/Infection.h"
#include "sim/Lod.h"
#include "sim/World.h"

#include <algorithm>
#include <vector>

namespace lw {

namespace {

// Существо, которому ставится уровень: собирается при разметке карты, чтобы
// не искать компоненты второй раз.
struct Creature {
    entt::entity entity;
    LodKind kind;
    bool active;
    float x;
    float y;
};

bool hasTarget(const Combat* combat) {
    return combat != nullptr && combat->target != kInvalidId;
}

} // namespace

void updateLod(World& world) {
    if (!world.config().lod) {
        return;
    }
    const std::uint64_t tick = world.tick();
    if (tick % kLodPeriodTicks != 0) {
        return;
    }
    entt::registry& registry = world.registry();
    ActivityMap& map = world.activityMap();
    map.clear();

    // Обход — в порядке хранения реестра (детерминирован), сам порядок на
    // результат не влияет: разметка — объединение битов, уровень у каждого свой.
    static thread_local std::vector<Creature> buffer;
    std::vector<Creature>& creatures = buffer;
    creatures.clear();

    // 1. Кто где. Люди — все живые с позицией (у эвакуированного её нет);
    // больной в жару вот-вот может умереть и встать — он для встречи как зомби.
    for (const auto [entity, person, position] : registry.view<const Person, const Position>().each()) {
        map.mark(ActivityMap::People, position.x, position.y);
        if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && isFeverish(*wounds)) {
            map.mark(ActivityMap::Undead, position.x, position.y);
        }
        const auto* mind = registry.try_get<Mind>(entity);
        const bool active =
            (mind != nullptr && keepsFull(mind->action)) || hasTarget(registry.try_get<Combat>(entity));
        if (active) {
            map.mark(ActivityMap::Active, position.x, position.y);
        }
        creatures.push_back({entity, LodKind::Person, active, position.x, position.y});
    }
    for (const auto [entity, zombie, position] : registry.view<const Zombie, const Position>().each()) {
        map.mark(ActivityMap::Undead, position.x, position.y);
        const bool hunting = zombie.state == ZombieState::Chase || hasTarget(registry.try_get<Combat>(entity));
        if (hunting) {
            map.mark(ActivityMap::Active, position.x, position.y);
        }
        // Идёт на шум — сам на полном уровне, соседей не будит: сбежавшиеся на
        // шум сбиваются в орду, а сколько стоять на месте и сколько быть
        // «возбуждённым», зомби отмеряет от своих решений — шагом в 16 с они
        // дольше стояли бы кучей, и орд собиралось бы больше.
        const bool active = hunting || zombie.state == ZombieState::Investigate;
        creatures.push_back({entity, LodKind::Zombie, active, position.x, position.y});
    }
    // Тело заражённого встанет: рядом с ним люди должны быть на полном уровне
    // заранее — встающий зомби появляется среди них без предупреждения.
    for (const auto [entity, corpse, position] : registry.view<const Corpse, const Position>().each()) {
        if (corpse.turnsAt > 0) {
            map.mark(ActivityMap::Undead, position.x, position.y);
        }
    }
    for (const auto [entity, animal, position] : registry.view<const Animal, const Position>().each()) {
        const bool active = animal.state == AnimalState::Flee || animal.state == AnimalState::Charge;
        if (active) {
            map.mark(ActivityMap::Active, position.x, position.y);
        }
        creatures.push_back({entity, LodKind::Animal, active, position.x, position.y});
    }
    for (const auto [entity, flock, position] : registry.view<const Flock, const Position>().each()) {
        creatures.push_back({entity, LodKind::Flock, false, position.x, position.y});
    }

    // 2. Окрестности.
    map.dilate(ActivityMap::People, ActivityMap::PeopleNear, ActivityMap::kEncounterRegions);
    map.dilate(ActivityMap::Undead, ActivityMap::UndeadNear, ActivityMap::kEncounterRegions);
    map.dilate(ActivityMap::Active, ActivityMap::ActiveNear, ActivityMap::kActiveRegions);
    map.dilate(ActivityMap::People, ActivityMap::PeopleFar, ActivityMap::kPopulatedRegions);
    map.setUpdated(tick);

    // 3. Уровни. Полный держится kFullHoldTicks после того, как причина ушла.
    for (const Creature& creature : creatures) {
        const DetailVerdict verdict =
            judgeDetail(creature.kind, creature.active, map.around(creature.x, creature.y));
        const Lod* lod = registry.try_get<Lod>(creature.entity);
        std::uint64_t fullUntil = lod != nullptr ? lod->fullUntil : 0;
        Detail level = verdict.level;
        LodCause cause = verdict.cause;
        if (level == Detail::Full) {
            fullUntil = std::max(fullUntil, tick + kFullHoldTicks);
        } else if (tick < fullUntil) {
            level = Detail::Full;
            cause = LodCause::Holdover;
        }
        setDetail(registry, creature.entity, level, cause, fullUntil);
    }
}

} // namespace lw
