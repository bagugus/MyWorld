#include "sim/systems/SocialSystem.h"

#include "sim/Components.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <vector>

namespace lw {

namespace {

// Рядом — не дальше этого, метры (как компания в NeedsSystem).
constexpr float kCompanyRadius = 20.0f;
// Узнаёт за минуту не больше стольких ближайших: в толпе на площади всех не
// запомнить.
constexpr std::size_t kMaxCompany = 3;
// Постоянные времени, часы разговора: за столько остаток до цели (знакомства —
// до 1, симпатии — до совместимости, доверия — до симпатии × знакомство)
// уменьшается примерно в e раз. Просто рядом — в kNearShare раз медленнее.
constexpr float kFamiliarityHours = 3.0f;
constexpr float kAffinityHours = 3.0f;
constexpr float kTrustHours = 24.0f;
constexpr float kNearShare = 0.2f;
// Не виделись дольше этого — знакомство забывается: от полного до нуля за
// kForgetHours.
constexpr std::uint64_t kForgetAfterTicks = 2 * kTicksPerGameDay;
constexpr float kForgetHours = 60.0f * 24.0f;

bool talking(const Mind& mind) {
    return mind.action == Action::Socialize && mind.phase == ActionPhase::Perform;
}

float approach(float value, float target, float share) {
    return value + (target - value) * std::min(1.0f, share);
}

} // namespace

void updateRelations(World& world, float minutes) {
    const float hours = minutes / 60.0f;
    const std::uint64_t tick = world.tick();
    entt::registry& registry = world.registry();
    static const Traits kAverage{};

    struct Companion {
        float distanceSq;
        EntityId id;
        entt::entity entity;
    };
    std::vector<Companion> company;
    auto view = registry.view<const Id, const Person, Relations, const Position, const Walker>();
    for (const auto [entity, id, person, relations, position, walker] : view.each()) {
        for (std::size_t i = 0; i < relations.count; ++i) {
            Bond& bond = relations.bonds[i];
            if (tick > bond.seen + kForgetAfterTicks) {
                bond.familiarity = std::max(0.0f, bond.familiarity - hours / kForgetHours);
            }
        }
        if (person.asleep || walker.moving()) {
            continue;
        }
        company.clear();
        world.spatial().forEachInRadius(
            position.x, position.y, kCompanyRadius, [&](const SpatialHit& hit) {
                if (hit.id == id.value) {
                    return;
                }
                const Person* other = registry.try_get<Person>(hit.entity);
                const Walker* steps = registry.try_get<Walker>(hit.entity);
                if (other != nullptr && !other->asleep && steps != nullptr && !steps->moving()) {
                    company.push_back({hit.distanceSq, hit.id, hit.entity});
                }
            });
        if (company.empty()) {
            continue;
        }
        // Ближайшие; при равных расстояниях — меньший id.
        const std::size_t count = std::min(kMaxCompany, company.size());
        std::partial_sort(company.begin(), company.begin() + static_cast<std::ptrdiff_t>(count),
                          company.end(), [](const Companion& a, const Companion& b) {
                              return a.distanceSq != b.distanceSq ? a.distanceSq < b.distanceSq
                                                                  : a.id < b.id;
                          });
        const Mind* mind = registry.try_get<Mind>(entity);
        const bool speaking = mind != nullptr && talking(*mind);
        const Traits* own = registry.try_get<Traits>(entity);
        const Traits& mine = own != nullptr ? *own : kAverage;
        for (std::size_t k = 0; k < count; ++k) {
            const Companion& companion = company[k];
            const Mind* theirMind = registry.try_get<Mind>(companion.entity);
            const float intensity =
                speaking || (theirMind != nullptr && talking(*theirMind)) ? 1.0f : kNearShare;
            const Traits* theirs = registry.try_get<Traits>(companion.entity);
            Bond* bond = acquireBond(
                relations, companion.id,
                chemistry(id.value, mine, companion.id, theirs != nullptr ? *theirs : kAverage),
                tick);
            if (bond == nullptr) {
                continue; // все связи крепкие: незнакомца не запомнить
            }
            const float pace = hours * intensity;
            bond->familiarity = approach(bond->familiarity, 1.0f, pace / kFamiliarityHours);
            bond->affinity = approach(bond->affinity, bond->chemistry, pace / kAffinityHours);
            // Доверие от одного общения только растёт; подорвать его могут поступки.
            const float trusted = std::max(0.0f, bond->affinity) * bond->familiarity;
            if (trusted > bond->trust) {
                bond->trust = approach(bond->trust, trusted, pace / kTrustHours);
            }
            bond->seen = tick;
            switch (updateBondFlags(*bond)) {
            case Bond::kFriend:
                world.record(ChronicleKind::Befriended, id.value, companion.id);
                break;
            case Bond::kFoe:
                world.record(ChronicleKind::Disliked, id.value, companion.id);
                break;
            default:
                break;
            }
        }
    }
}

void tickRelations(World& world) {
    if (world.tick() % kTicksPerGameMinute == 0) {
        updateRelations(world, 1.0f);
    }
}

} // namespace lw
