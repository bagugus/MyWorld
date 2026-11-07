#include "sim/systems/FamilySystem.h"

#include "sim/Family.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <vector>

namespace lw {

namespace {

// Чужой взрослый берёт ребёнка, если оценка (близость к ребёнку + доля
// альтруизма) не ниже kAdoptScore, а близость — не ниже kAdoptCloseness.
constexpr float kAdoptCloseness = 0.2f;
constexpr float kAdoptAltruism = 0.5f;
constexpr float kAdoptScore = 0.5f;
// Ребёнок обязан тому, кто взял его к себе.
constexpr float kTakenInDebt = 0.4f;

// Живой человек с таким id; entt::null — нет.
entt::entity livingPerson(const World& world, EntityId id) {
    const entt::entity entity = world.find(id);
    return entity != entt::null && world.registry().all_of<Person>(entity) ? entity
                                                                           : entt::entity{entt::null};
}

// Порядок родни при выборе опекуна: меньше — раньше; Count — не берёт.
int kinRank(KinKind kind) {
    switch (kind) {
    case KinKind::Parent:
        return 0;
    case KinKind::Sibling:
        return 1;
    case KinKind::Grandparent:
        return 2;
    case KinKind::Count:
    case KinKind::Child:
    case KinKind::Grandchild:
        return 4;
    default:
        return 3;
    }
}

} // namespace

EntityId chooseGuardian(const World& world, entt::entity child) {
    const entt::registry& registry = world.registry();
    const auto* kin = registry.try_get<Kin>(child);
    const EntityId childId = registry.get<Id>(child).value;
    // Родня — по порядку, при равных — меньший id.
    EntityId best = kInvalidId;
    int bestRank = 4;
    for (std::size_t i = 0; kin != nullptr && i < kin->count; ++i) {
        const Relative& relative = kin->relatives[i];
        const entt::entity other = livingPerson(world, relative.id);
        const int rank = kinRank(relative.kind);
        if (other == entt::null || isMinor(registry, other) || rank >= 4) {
            continue;
        }
        if (rank < bestRank || (rank == bestRank && relative.id < best)) {
            best = relative.id;
            bestRank = rank;
        }
    }
    if (best != kInvalidId) {
        return best;
    }
    // Чужой взрослый, кто знает ребёнка и добр к нему (и кого ребёнок не
    // невзлюбил).
    const auto* own = registry.try_get<Relations>(child);
    float bestScore = kAdoptScore;
    for (const auto [entity, id, person, relations] :
         registry.view<const Id, const Person, const Relations>().each()) {
        if (id.value == childId || isMinor(registry, entity)) {
            continue;
        }
        const Bond* bond = relations.find(childId);
        if (bond == nullptr || bond->hostile() || closeness(*bond) < kAdoptCloseness) {
            continue;
        }
        if (const Bond* back = own != nullptr ? own->find(id.value) : nullptr; back != nullptr && back->hostile()) {
            continue;
        }
        const auto* traits = registry.try_get<Traits>(entity);
        const float score = closeness(*bond) + kAdoptAltruism * (traits != nullptr ? traits->altruism : 0.5f);
        if (score > bestScore || (score == bestScore && best != kInvalidId && id.value < best)) {
            best = id.value;
            bestScore = score;
        }
    }
    return best;
}

void updateFamilies(World& world) {
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> children;
    for (const auto [entity, id, person, kin] : registry.view<const Id, const Person, const Kin>().each()) {
        if (isMinor(registry, entity)) {
            children.emplace_back(id.value, entity);
        }
    }
    std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, entity] : children) {
        Kin& kin = registry.get<Kin>(entity);
        Person& person = registry.get<Person>(entity);
        if (const entt::entity guardian = livingPerson(world, kin.guardian); guardian != entt::null) {
            // Живёт у опекуна; в группе дом решает группа.
            const Person& keeper = registry.get<Person>(guardian);
            if (keeper.group == kInvalidId && person.group == kInvalidId && keeper.home != kInvalidId &&
                person.home != keeper.home) {
                person.home = keeper.home;
            }
            continue;
        }
        const EntityId previous = kin.guardian;
        bool parentAlive = false;
        for (std::size_t i = 0; i < kin.count; ++i) {
            parentAlive = parentAlive || (kin.relatives[i].kind == KinKind::Parent &&
                                          livingPerson(world, kin.relatives[i].id) != entt::null);
        }
        if (previous != kInvalidId && !parentAlive && kinOf(registry, entity, previous) == KinKind::Parent) {
            world.record(ChronicleKind::Orphaned, id, previous);
        }
        kin.guardian = chooseGuardian(world, entity);
        const entt::entity keeper = livingPerson(world, kin.guardian);
        if (keeper == entt::null) {
            kin.guardian = kInvalidId;
            continue;
        }
        const Person& theirs = registry.get<Person>(keeper);
        // Родитель, у которого ребёнок и так живёт, — не событие.
        const bool parent = kinOf(registry, entity, kin.guardian) == KinKind::Parent;
        if (!(parent && theirs.home == person.home)) {
            world.record(ChronicleKind::TookIn, kin.guardian, id, kinCode(world, keeper, id),
                         theirs.group != kInvalidId ? 1 : 0);
        }
        if (theirs.group == kInvalidId && person.group == kInvalidId && theirs.home != kInvalidId) {
            person.home = theirs.home;
        }
        if (!parent) {
            if (auto* relations = registry.try_get<Relations>(entity)) {
                static const Traits kAverage{};
                const auto* mine = registry.try_get<Traits>(entity);
                const auto* other = registry.try_get<Traits>(keeper);
                Bond* bond = acquireBond(*relations, kin.guardian,
                                         chemistry(id, mine != nullptr ? *mine : kAverage, kin.guardian,
                                                   other != nullptr ? *other : kAverage),
                                         world.tick());
                if (bond != nullptr) {
                    addDebt(*bond, kTakenInDebt);
                    refreshBondFlags(world, id, *bond);
                }
            }
        }
    }
}

void tickFamilies(World& world) {
    if (world.tick() % kTicksPerGameHour == 0) {
        updateFamilies(world);
    }
}

} // namespace lw
