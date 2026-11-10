#include "sim/systems/ScenarioSystem.h"

#include "sim/Calendar.h"
#include "sim/Infection.h"
#include "sim/Names.h"
#include "sim/Scenario.h"
#include "sim/World.h"
#include "sim/systems/InfectionSystem.h"
#include "sim/systems/SocietySystem.h"

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
        // Школа закрылась — дети не «бросают работу» (этап 7.3); солдаты
        // уходят с армией или остаются — это пишет SocietySystem (этап 7.6).
        if (job.profession != Profession::Pupil && job.profession != Profession::Soldier &&
            keepsWorking(before, job.profession, t) &&
            !keepsWorking(after, job.profession, t)) {
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
        // Нулевой пациент (этап 7.5): никто, и он сам, ещё не знает.
        const EntityId zero = infectPatientZero(world, id, world.rng(RngStream::Scenario));
        registry.get<Scenario>(entity).patientZero = zero;
    }
    // Ответ власти и армии (этап 7.6): эвакуация, уход армии, последняя передача.
    onPhaseBegan(world, after);
}

// Есть ли среди жителей зараза (этап 7.5): заражённый, тело, которое встанет,
// зомби из жителей (с именем).
bool infectionAmongResidents(const World& world) {
    const entt::registry& registry = world.registry();
    for (const auto [entity, person, wounds] : registry.view<const Person, const Wounds>().each()) {
        if (isInfected(wounds)) {
            return true;
        }
    }
    for (const auto [entity, corpse] : registry.view<const Corpse>().each()) {
        if (corpse.kind == CorpseKind::Human && corpse.turnsAt > world.tick()) {
            return true;
        }
    }
    for (const auto [entity, zombie] : registry.view<const Zombie>().each()) {
        if (registry.all_of<Name>(entity)) {
            return true;
        }
    }
    return false;
}

// Вспышка и паника, раз в час: вирус приходит в край не один раз. Затишье
// (заразы среди жителей нет) дольше kReimportTicks — новый завозной случай.
void watchForReimport(World& world, entt::entity entity) {
    entt::registry& registry = world.registry();
    const WorldPhase phase = registry.get<Scenario>(entity).phase;
    if (phase != WorldPhase::Outbreak && phase != WorldPhase::Panic) {
        return;
    }
    const std::uint64_t tick = world.tick();
    if (infectionAmongResidents(world)) {
        registry.get<Scenario>(entity).quietSince = Scenario::kNever;
        return;
    }
    Scenario& scenario = registry.get<Scenario>(entity);
    if (scenario.quietSince == Scenario::kNever) {
        scenario.quietSince = tick;
        return;
    }
    if (tick >= scenario.quietSince + kReimportTicks) {
        scenario.quietSince = Scenario::kNever;
        infectPatientZero(world, registry.get<Id>(entity).value, world.rng(RngStream::Scenario), true);
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

bool phaseChangeDue(const World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return false;
    }
    const entt::entity entity = scenarioEntity(world);
    if (entity == entt::null) {
        return false;
    }
    const Scenario& scenario = world.registry().get<Scenario>(entity);
    if (scenario.phase == WorldPhase::Survival) {
        return false;
    }
    // Срок вышел (у мира — расписание вспышки).
    if (tick >= phaseDeadline(scenario)) {
        return true;
    }
    if (calendarAt(tick).minute != 0) {
        return false;
    }
    // Беда дошла до порога — раз в час.
    const int trigger = phaseTrigger(scenario);
    return trigger > 0 && tick >= phaseEarliest(scenario) && scenarioStats(world).casualties() >= trigger;
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
    if (phaseChangeDue(world)) {
        beginNextPhase(world, entity);
        return;
    }
    if (world.registry().get<Scenario>(entity).phase == WorldPhase::Survival || calendarAt(tick).minute != 0) {
        return;
    }
    // Вирус пришёл снова (этап 7.5).
    watchForReimport(world, entity);
}

} // namespace lw
