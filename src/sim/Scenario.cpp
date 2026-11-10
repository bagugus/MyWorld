#include "sim/Scenario.h"

#include "sim/Calendar.h"
#include "sim/Infection.h"
#include "sim/Names.h"
#include "sim/Work.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace lw {

namespace {

constexpr std::uint32_t bit(Profession profession) {
    return 1u << static_cast<unsigned>(profession);
}

// Солдаты (этап 7.6) есть в мире только с паники до коллапса, но служат, пока
// есть власть.
constexpr std::uint32_t kEveryone = bit(Profession::Shopkeeper) | bit(Profession::Farmer) |
                                    bit(Profession::Doctor) | bit(Profession::PoliceOfficer) |
                                    bit(Profession::Teacher) | bit(Profession::Pupil) |
                                    bit(Profession::Soldier);

// Правила по порядку WorldPhase. Числа и почему такие — docs/architecture.md,
// «Сценарий и фазы мира».
constexpr PhaseRules kRules[] = {
    {.name = "peace",
     .professions = kEveryone,
     .eveningHome = 0.2f},
    // Вспышка: правила мирные — никто ещё не понимает, что происходит. Зомби
    // — только свои: нулевой пациент и те, кого он заразил (этап 7.5).
    {.name = "outbreak",
     .professions = kEveryone,
     .eveningHome = 0.2f,
     .minHours = 12,
     .maxHours = 72,
     .trigger = 0.01f,
     .triggerMin = 3},
    // Паника: перебои с завозом, скупают впрок, школа закрыта, на работу
    // выходят те, кто держится за неё; вечером — по домам.
    {.name = "panic",
     .supply = 0.5f,
     .professions = kEveryone & ~bit(Profession::Teacher) & ~bit(Profession::Pupil),
     .dutyNeeded = 0.45f,
     .eveningHome = 0.35f,
     .stockpile = 2.5f,
     .groups = true,
     .eventsPerDay = {0.5f, 0.1f, 0.3f},
     .minHours = 24,
     .maxHours = 96,
     .trigger = 0.1f,
     .triggerMin = 8},
    // Коллапс: власть кончилась — ни завоза, ни денег, ни воды; в больнице и
    // на улицах остались самые стойкие врачи и полицейские. Кража — уже не
    // преступление, а то, что делают все. Беда пришла и к соседям: издалека
    // забредают зомби.
    {.name = "collapse",
     .supply = 0.0f,
     .economy = false,
     .tapWater = false,
     .professions = bit(Profession::Doctor) | bit(Profession::PoliceOfficer),
     .dutyNeeded = 0.7f,
     .lawful = false,
     .groups = true,
     .zombieShare = 0.15f,
     .arrivals = 1,
     .eventsPerDay = {0.3f, 0.35f, 0.35f},
     .minHours = 72,
     .maxHours = 72},
    // Выживание: на работу не ходит никто, каждый сам за себя или со своими.
    {.name = "survival",
     .supply = 0.0f,
     .economy = false,
     .tapWater = false,
     .lawful = false,
     .groups = true,
     .zombieShare = 0.4f,
     .arrivals = 2,
     .eventsPerDay = {0.2f, 0.15f, 0.1f}},
};
static_assert(std::size(kRules) == kWorldPhases);

// Без сценария (мир начат без мирной жизни) — приток как до этапа 7.4:
// раз в сутки, до WorldConfig::zombies.
constexpr int kPlainArrivals = 1;

std::uint64_t hoursAfter(std::uint64_t tick, int hours) {
    return tick + static_cast<std::uint64_t>(hours) * kTicksPerGameHour;
}

} // namespace

const PhaseRules& phaseRules(WorldPhase phase) {
    const auto index = static_cast<std::size_t>(phase);
    return kRules[index < std::size(kRules) ? index : std::size(kRules) - 1];
}

std::optional<WorldPhase> phaseByName(const char* name) {
    for (std::size_t i = 0; i < std::size(kRules); ++i) {
        if (std::strcmp(kRules[i].name, name) == 0) {
            return static_cast<WorldPhase>(i);
        }
    }
    return std::nullopt;
}

WorldPhase nextPhase(WorldPhase phase) {
    return phase == WorldPhase::Survival
               ? phase
               : static_cast<WorldPhase>(static_cast<std::size_t>(phase) + 1);
}

const Scenario* scenarioOf(const World& world) {
    // Сценарий один; если бы их было несколько, — с меньшим id.
    const Scenario* found = nullptr;
    EntityId foundId = kInvalidId;
    for (const auto [entity, id, scenario] : world.registry().view<const Id, const Scenario>().each()) {
        if (found == nullptr || id.value < foundId) {
            found = &scenario;
            foundId = id.value;
        }
    }
    return found;
}

EntityId scenarioIdOf(const World& world) {
    EntityId found = kInvalidId;
    for (const auto [entity, id, scenario] : world.registry().view<const Id, const Scenario>().each()) {
        if (found == kInvalidId || id.value < found) {
            found = id.value;
        }
    }
    return found;
}

bool isPatientZero(const World& world, EntityId id) {
    const Scenario* scenario = scenarioOf(world);
    return scenario != nullptr && id != kInvalidId && scenario->patientZero == id;
}

WorldPhase worldPhase(const World& world) {
    const Scenario* scenario = scenarioOf(world);
    return scenario != nullptr ? scenario->phase : WorldPhase::Survival;
}

const PhaseRules& rulesOf(const World& world) {
    return phaseRules(worldPhase(world));
}

float dutyOf(Profession profession, const Traits& t) {
    float duty = 0.0f;
    switch (profession) {
    case Profession::Doctor:
        duty = 0.3f + 0.4f * t.altruism + 0.3f * t.bravery;
        break;
    case Profession::PoliceOfficer:
        duty = 0.3f + 0.5f * t.bravery + 0.2f * t.leadership;
        break;
    case Profession::Shopkeeper:
        duty = 0.1f + 0.4f * t.greed + 0.3f * t.bravery;
        break;
    case Profession::Farmer:
        duty = 0.2f + 0.4f * t.prudence + 0.2f * t.bravery;
        break;
    case Profession::Teacher:
        duty = 0.2f + 0.3f * t.altruism + 0.3f * t.bravery;
        break;
    case Profession::Pupil:
        duty = 1.0f; // пока школа открыта, дети ходят на уроки
        break;
    case Profession::Soldier:
        duty = 0.3f + 0.4f * t.bravery + 0.3f * t.leadership; // дисциплина (этап 7.6)
        break;
    default:
        break;
    }
    return std::clamp(duty, 0.0f, 1.0f);
}

bool keepsWorking(WorldPhase phase, Profession profession, const Traits& traits) {
    const PhaseRules& rules = phaseRules(phase);
    return profession != Profession::None && (rules.professions & bit(profession)) != 0 &&
           dutyOf(profession, traits) >= rules.dutyNeeded;
}

bool keepsWorking(const World& world, entt::entity person) {
    const entt::registry& registry = world.registry();
    const auto* job = registry.try_get<Job>(person);
    if (job == nullptr) {
        return false;
    }
    static const Traits kAverage{};
    const auto* traits = registry.try_get<Traits>(person);
    return keepsWorking(worldPhase(world), job->profession, traits != nullptr ? *traits : kAverage);
}

std::vector<EntityId> mannedWorkplaces(const World& world) {
    const entt::registry& registry = world.registry();
    const WorldPhase phase = worldPhase(world);
    static const Traits kAverage{};
    std::vector<EntityId> manned;
    for (const auto [entity, person, job] : registry.view<const Person, const Job>().each()) {
        const auto* traits = registry.try_get<Traits>(entity);
        if (job.profession != Profession::Pupil &&
            keepsWorking(phase, job.profession, traits != nullptr ? *traits : kAverage)) {
            manned.push_back(job.workplace);
        }
    }
    std::sort(manned.begin(), manned.end());
    manned.erase(std::unique(manned.begin(), manned.end()), manned.end());
    return manned;
}

bool policeOnDuty(const World& world, entt::entity person) {
    const entt::registry& registry = world.registry();
    const auto* job = registry.try_get<Job>(person);
    const auto* mind = registry.try_get<Mind>(person);
    const auto* self = registry.try_get<Person>(person);
    // Солдат на посту (этап 7.6) — тоже: заступается за любого.
    return job != nullptr && mind != nullptr && self != nullptr &&
           (job->profession == Profession::PoliceOfficer || job->profession == Profession::Soldier) &&
           !self->asleep &&
           (mind->action == Action::Work || mind->action == Action::Fight) &&
           onShift(*job, calendarAt(world.tick())) && keepsWorking(world, person);
}

int zombieTarget(const World& world) {
    const int configured = world.config().zombies;
    const Scenario* scenario = scenarioOf(world);
    if (scenario == nullptr) {
        return configured;
    }
    const float share = phaseRules(scenario->phase).zombieShare;
    return std::max(configured,
                    static_cast<int>(std::lround(share * static_cast<float>(scenario->population))));
}

int arrivalsPerDay(const World& world) {
    const Scenario* scenario = scenarioOf(world);
    return scenario != nullptr ? phaseRules(scenario->phase).arrivals : kPlainArrivals;
}

ScenarioStats scenarioStats(const World& world) {
    const entt::registry& registry = world.registry();
    ScenarioStats stats;
    for (const auto [entity, person] : registry.view<const Person>().each()) {
        ++stats.alive;
        if (const auto* job = registry.try_get<Job>(entity); job != nullptr && job->profession == Profession::Soldier) {
            ++stats.soldiers;
        }
        if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && isInfected(*wounds)) {
            ++stats.infected;
            stats.sick += isVisiblyIll(*wounds) ? 1 : 0;
        }
    }
    for (const auto [entity, zombie] : registry.view<const Zombie>().each()) {
        ++stats.zombies;
        stats.residents += registry.all_of<Name>(entity) ? 1 : 0;
    }
    const Scenario* scenario = scenarioOf(world);
    const bool started = scenario != nullptr &&
                         scenario->began[static_cast<std::size_t>(WorldPhase::Outbreak)] != Scenario::kNever;
    for (const auto [entity, evacuated] : registry.view<const Evacuated>().each()) {
        const auto* job = registry.try_get<Job>(entity);
        stats.evacuated += job != nullptr && job->profession == Profession::Soldier ? 0 : 1;
    }
    // Солдаты пришли извне, а эвакуированные живы (этап 7.6): ни те, ни другие
    // не в счёт потерь.
    const int residents = stats.alive - stats.soldiers;
    stats.population = started ? scenario->population : residents;
    stats.lost = std::max(0, stats.population - residents - stats.evacuated);
    return stats;
}

std::uint64_t phaseEarliest(const Scenario& scenario) {
    const PhaseRules& rules = phaseRules(scenario.phase);
    if (rules.maxHours == 0) {
        return Scenario::kNever;
    }
    return hoursAfter(scenario.began[static_cast<std::size_t>(scenario.phase)], rules.minHours);
}

std::uint64_t phaseDeadline(const Scenario& scenario) {
    if (scenario.phase == WorldPhase::Peace) {
        return scenario.outbreakAt;
    }
    const PhaseRules& rules = phaseRules(scenario.phase);
    if (rules.maxHours == 0) {
        return Scenario::kNever;
    }
    return hoursAfter(scenario.began[static_cast<std::size_t>(scenario.phase)], rules.maxHours);
}

int phaseTrigger(const Scenario& scenario) {
    const PhaseRules& rules = phaseRules(scenario.phase);
    if (rules.trigger <= 0.0f) {
        return 0;
    }
    return std::max(rules.triggerMin, static_cast<int>(std::ceil(
                                          rules.trigger * static_cast<float>(scenario.population))));
}

} // namespace lw
