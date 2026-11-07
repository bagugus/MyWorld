#include "sim/Infection.h"

#include "sim/Death.h"
#include "sim/World.h"

#include <algorithm>
#include <iterator>

namespace lw {

bool isInfected(const entt::registry& registry, entt::entity entity) {
    const auto* wounds = registry.try_get<Wounds>(entity);
    return wounds != nullptr && isInfected(*wounds);
}

float infectionHours(const Wounds& wounds) {
    return wounds.bites > 0 ? kInfectionHours : kContactInfectionHours;
}

float feverOf(const Wounds& wounds) {
    if (wounds.infection < kFeverStart) {
        return 0.0f;
    }
    return std::clamp((wounds.infection - kFeverStart) / (1.0f - kFeverStart), 0.0f, 1.0f);
}

float contagiousness(const Wounds& wounds) {
    if (wounds.infection < kContagiousFrom) {
        return 0.0f;
    }
    return isFeverish(wounds) ? 1.0f : kLatentShedding;
}

float hoursToFever(const Wounds& wounds) {
    return std::max(0.0f, kFeverStart - wounds.infection) * infectionHours(wounds);
}

float hoursToEnd(const Wounds& wounds) {
    return std::max(0.0f, 1.0f - wounds.infection) * infectionHours(wounds);
}

bool infect(World& world, entt::entity person, InfectionRoute route, EntityId source) {
    entt::registry& registry = world.registry();
    if (!registry.all_of<Person>(person)) {
        return false;
    }
    Wounds& wounds = registry.get_or_emplace<Wounds>(person);
    if (isInfected(wounds)) {
        return false;
    }
    wounds.infection = kInfectionSeed;
    wounds.route = route;
    wounds.infectedBy = source;
    wounds.infectedAt = world.tick();
    // Нулевого пациента и завозной случай пишет infectPatientZero (PatientZero).
    if (route != InfectionRoute::Origin && route != InfectionRoute::Bite) {
        world.record(ChronicleKind::Infected, registry.get<Id>(person).value, source,
                     static_cast<std::int32_t>(route));
    }
    return true;
}

InfectionRoute infectionRouteOf(const World& world, EntityId id) {
    const entt::entity entity = world.find(id);
    if (entity != entt::null) {
        if (const auto* wounds = world.registry().try_get<Wounds>(entity); wounds != nullptr) {
            return isInfected(*wounds) ? wounds->route : InfectionRoute::None;
        }
    }
    const Chronicle& chronicle = world.chronicle();
    const auto& events = chronicle.events();
    for (const std::uint32_t index : chronicle.eventsOf(id)) {
        const ChronicleEvent& event = events[index];
        if (event.subject != id) {
            continue;
        }
        switch (event.kind) {
        case ChronicleKind::Bitten:
            return InfectionRoute::Bite;
        case ChronicleKind::PatientZero:
            return InfectionRoute::Origin;
        case ChronicleKind::Infected:
            return static_cast<InfectionRoute>(event.a);
        default:
            break;
        }
    }
    return InfectionRoute::None;
}

const char* infectionRouteName(InfectionRoute route) {
    constexpr const char* kNames[] = {"none",   "origin", "bite",   "household",
                                      "care",   "contact", "nearby", "body"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(InfectionRoute::Count));
    const auto index = static_cast<std::size_t>(route);
    return index < std::size(kNames) ? kNames[index] : "?";
}

} // namespace lw
