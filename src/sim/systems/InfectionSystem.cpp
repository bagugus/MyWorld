#include "sim/systems/InfectionSystem.h"

#include "sim/Family.h"
#include "sim/Infection.h"
#include "sim/Work.h"
#include "sim/World.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Вес жителя в выборе нулевого пациента: чем больше людей вокруг, тем вероятнее.
// Живущих одних не выбирают, если есть другие: нулевой пациент, которому
// некого заразить, оставил бы вспышку пустой.
constexpr float kZeroBase = 1.0f;
constexpr float kZeroPerHousemate = 1.0f;
constexpr float kZeroEmployed = 1.5f;
constexpr float kZeroTown = 1.0f;

// Заразное: больной или тело заражённого, которое ещё встанет.
struct Source {
    EntityId id = kInvalidId;
    entt::entity entity = entt::null;
    float shedding = 0.0f;
    bool body = false;
};

// Стоит у своей двери (дома).
bool atHomeDoor(const entt::registry& registry, const World& world, entt::entity entity,
                const Person& person) {
    const entt::entity home = world.find(person.home);
    if (home == entt::null) {
        return false;
    }
    const Position& at = registry.get<Position>(entity);
    return Map::cellAt(at.x, at.y) == registry.get<Building>(home).door;
}

// В больнице: лечится (на месте) или врач на смене (на месте).
bool inHospital(const entt::registry& registry, entt::entity entity, bool& patient) {
    const auto* mind = registry.try_get<Mind>(entity);
    if (mind == nullptr || mind->phase != ActionPhase::Perform) {
        return false;
    }
    if (mind->action == Action::SeeDoctor) {
        patient = true;
        return true;
    }
    const auto* job = registry.try_get<Job>(entity);
    return mind->action == Action::Work && job != nullptr && job->profession == Profession::Doctor;
}

// Лицом к лицу: один занят другим — разговор, подарок, обмен.
bool faceToFace(const Mind* mind, EntityId other) {
    if (mind == nullptr || mind->target != other) {
        return false;
    }
    return mind->action == Action::Socialize || mind->action == Action::Share ||
           mind->action == Action::Trade;
}

// Как незаражённый target рядом с заразным source: путь заражения.
InfectionRoute routeBetween(const World& world, const Source& source, entt::entity target,
                            EntityId targetId) {
    const entt::registry& registry = world.registry();
    if (source.body) {
        const auto* walker = registry.try_get<Walker>(target);
        return walker != nullptr && walker->moving() ? InfectionRoute::None : InfectionRoute::Body;
    }
    const Person& sick = registry.get<Person>(source.entity);
    const Person& person = registry.get<Person>(target);
    if (sick.home != kInvalidId && sick.home == person.home &&
        atHomeDoor(registry, world, source.entity, sick) && atHomeDoor(registry, world, target, person)) {
        return InfectionRoute::Household;
    }
    bool sickPatient = false;
    bool patient = false;
    if (inHospital(registry, source.entity, sickPatient) && inHospital(registry, target, patient) &&
        (sickPatient || patient)) {
        return InfectionRoute::Care;
    }
    if (faceToFace(registry.try_get<Mind>(source.entity), targetId) ||
        faceToFace(registry.try_get<Mind>(target), source.id)) {
        return InfectionRoute::Contact;
    }
    return InfectionRoute::Nearby;
}

float rateOf(InfectionRoute route) {
    switch (route) {
    case InfectionRoute::Household:
        return kHouseholdRate;
    case InfectionRoute::Care:
        return kCareRate;
    case InfectionRoute::Contact:
        return kContactRate;
    case InfectionRoute::Nearby:
        return kNearbyRate;
    case InfectionRoute::Body:
        return kBodyRate;
    default:
        return 0.0f;
    }
}

} // namespace

void tickInfection(World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return;
    }
    entt::registry& registry = world.registry();
    std::vector<Source> sources;
    for (const auto [entity, id, person, wounds] : registry.view<const Id, const Person, const Wounds>().each()) {
        if (const float shedding = contagiousness(wounds); shedding > 0.0f) {
            sources.push_back({id.value, entity, shedding, false});
        }
    }
    for (const auto [entity, id, corpse] : registry.view<const Id, const Corpse>().each()) {
        if (corpse.kind == CorpseKind::Human && corpse.turnsAt > tick) {
            sources.push_back({id.value, entity, kBodyShedding, true});
        }
    }
    if (sources.empty()) {
        return;
    }
    std::sort(sources.begin(), sources.end(),
              [](const Source& a, const Source& b) { return a.id < b.id; });

    Rng& rng = world.rng(RngStream::Outbreak);
    std::vector<SpatialHit> hits;
    for (const Source& source : sources) {
        const Position& at = registry.get<Position>(source.entity);
        world.spatial().queryRadius(at.x, at.y, kContactRadius, hits);
        std::sort(hits.begin(), hits.end(),
                  [](const SpatialHit& a, const SpatialHit& b) { return a.id < b.id; });
        for (const SpatialHit& hit : hits) {
            if (hit.id == source.id || !registry.all_of<Person>(hit.entity) ||
                isInfected(registry, hit.entity)) {
                continue;
            }
            const InfectionRoute route = routeBetween(world, source, hit.entity, hit.id);
            const float chance = rateOf(route) * source.shedding / 60.0f;
            if (chance > 0.0f && rng.chance(chance)) {
                infect(world, hit.entity, route, source.id);
            }
        }
    }
}

EntityId infectPatientZero(World& world, EntityId scenario, Rng& rng, bool imported) {
    const entt::registry& registry = world.registry();
    // Сколько людей живёт в каждом доме (только для поиска).
    std::unordered_map<EntityId, int> residents;
    for (const auto [entity, person] : registry.view<const Person>().each()) {
        if (person.home != kInvalidId) {
            ++residents[person.home];
        }
    }
    std::vector<std::pair<EntityId, float>> candidates;
    std::vector<std::pair<EntityId, float>> alone;
    for (const auto [entity, id, person] : registry.view<const Id, const Person>().each()) {
        const entt::entity home = world.find(person.home);
        if (home == entt::null || isMinor(registry, entity) || isInfected(registry, entity)) {
            continue;
        }
        const int housemates = residents[person.home] - 1;
        float weight = kZeroBase + kZeroPerHousemate * static_cast<float>(housemates);
        weight += isEmployed(registry.try_get<Job>(entity)) ? kZeroEmployed : 0.0f;
        const entt::entity settlement = world.find(registry.get<Building>(home).settlement);
        if (settlement != entt::null && registry.get<Settlement>(settlement).kind == SettlementKind::Town) {
            weight += kZeroTown;
        }
        (housemates > 0 ? candidates : alone).emplace_back(id.value, weight);
    }
    if (candidates.empty()) {
        candidates = std::move(alone);
    }
    if (candidates.empty()) {
        return kInvalidId;
    }
    std::sort(candidates.begin(), candidates.end());
    float total = 0.0f;
    for (const auto& [id, weight] : candidates) {
        total += weight;
    }
    float roll = rng.range(0.0f, total);
    EntityId chosen = candidates.back().first;
    for (const auto& [id, weight] : candidates) {
        if (roll < weight) {
            chosen = id;
            break;
        }
        roll -= weight;
    }
    const entt::entity patient = world.find(chosen);
    infect(world, patient, InfectionRoute::Origin, scenario);
    // Заразился раньше, чем это заметили: скрытый период уже идёт.
    world.registry().get<Wounds>(patient).infection = kPatientZeroInfection;
    world.record(ChronicleKind::PatientZero, chosen, scenario, imported ? 1 : 0);
    return chosen;
}

} // namespace lw
