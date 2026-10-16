#include "sim/systems/ScenarioSystem.h"

#include "sim/Calendar.h"
#include "sim/Population.h"
#include "sim/Scenario.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Сущность сценария с меньшим id; entt::null — сценария нет.
entt::entity scenarioEntity(const World& world) {
    const EntityId id = scenarioIdOf(world);
    return id != kInvalidId ? world.find(id) : entt::entity{entt::null};
}

// Кто из работников бросает работу при переходе от фазы before к after:
// работал в before и не выйдет в after. По возрастанию id.
void recordQuitters(World& world, WorldPhase before, WorldPhase after) {
    const entt::registry& registry = world.registry();
    static const Traits kAverage{};
    std::vector<std::pair<EntityId, EntityId>> quitters; // человек, место работы
    for (const auto [entity, id, person, job] :
         registry.view<const Id, const Person, const Job>().each()) {
        const auto* traits = registry.try_get<Traits>(entity);
        const Traits& t = traits != nullptr ? *traits : kAverage;
        if (keepsWorking(before, job.profession, t) && !keepsWorking(after, job.profession, t)) {
            quitters.emplace_back(id.value, job.workplace);
        }
    }
    std::sort(quitters.begin(), quitters.end());
    for (const auto& [person, workplace] : quitters) {
        world.record(ChronicleKind::LeftWork, person, workplace, static_cast<std::int32_t>(after));
    }
}

// Начать следующую фазу.
void beginNextPhase(World& world, entt::entity entity) {
    entt::registry& registry = world.registry();
    Scenario& scenario = registry.get<Scenario>(entity);
    const WorldPhase before = scenario.phase;
    const WorldPhase after = nextPhase(before);
    if (after == before) {
        return;
    }
    if (after == WorldPhase::Outbreak) {
        scenario.population = static_cast<std::int32_t>(registry.view<const Person>().size());
    }
    scenario.phase = after;
    scenario.began[static_cast<std::size_t>(after)] = world.tick();
    const EntityId id = registry.get<Id>(entity).value;
    world.record(ChronicleKind::PhaseBegan, id, kInvalidId, static_cast<std::int32_t>(after),
                 scenarioStats(world).casualties());
    recordQuitters(world, before, after);
    if (after == WorldPhase::Outbreak) {
        // Первые заражённые. Позиции остальных с начала тика не менялись:
        // индекс узнаёт только о пришедших.
        Rng& rng = world.rng(RngStream::Scenario);
        spawnZombieGroup(world, rng.rangeInt(kFirstInfectedMin, kFirstInfectedMax), rng);
        world.syncSpatial();
    }
}

} // namespace

void createScenario(World& world) {
    entt::registry& registry = world.registry();
    const entt::entity entity = world.create();
    Scenario scenario;
    scenario.began.fill(Scenario::kNever);
    scenario.began[static_cast<std::size_t>(WorldPhase::Peace)] = world.tick();
    const double days = world.config().outbreakDays;
    if (days > 0.0) {
        scenario.outbreakAt =
            world.tick() + static_cast<std::uint64_t>(std::llround(days * kTicksPerGameDay));
    }
    scenario.population = static_cast<std::int32_t>(registry.view<const Person>().size());
    registry.emplace<Scenario>(entity, scenario);
    world.record(ChronicleKind::PhaseBegan, registry.get<Id>(entity).value, kInvalidId,
                 static_cast<std::int32_t>(WorldPhase::Peace), 0);
}

void enterPhase(World& world, WorldPhase phase) {
    const entt::entity entity = scenarioEntity(world);
    if (entity == entt::null) {
        return;
    }
    while (world.registry().get<Scenario>(entity).phase < phase) {
        beginNextPhase(world, entity);
    }
}

void updateScenario(World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return;
    }
    const entt::entity entity = scenarioEntity(world);
    if (entity == entt::null) {
        return;
    }
    const Scenario& scenario = world.registry().get<Scenario>(entity);
    if (scenario.phase == WorldPhase::Survival) {
        return;
    }
    // Срок вышел (у мира — расписание вспышки).
    if (tick >= phaseDeadline(scenario)) {
        beginNextPhase(world, entity);
        return;
    }
    // Беда дошла до порога — раз в час.
    const int trigger = phaseTrigger(scenario);
    if (trigger > 0 && calendarAt(tick).minute == 0 && tick >= phaseEarliest(scenario) &&
        scenarioStats(world).casualties() >= trigger) {
        beginNextPhase(world, entity);
    }
}

} // namespace lw
