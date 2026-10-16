#include "sim/Relations.h"

#include "sim/Hash.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace lw {

namespace {

// Общая часть совместимости — равномерно в [kPairMin, kPairMin + kPairSpread):
// с этими числами примерно каждый четвёртый становится другом, если они
// проводят время вместе, и каждый двенадцатый — недругом.
constexpr float kPairMin = -0.4f;
constexpr float kPairSpread = 1.2f;
// Что self видит в характере other.
constexpr float kKindness = 0.4f;   // × (альтруизм other − 0,5)
constexpr float kHarshness = 0.5f;  // × (агрессия other − 0,5) × (1 − агрессия self)
constexpr float kWarmth = 0.3f;     // × (общительность self − 0,5)

// Долг и обида: насколько сдвигают симпатию (и цель, к которой она тянется) и
// доверие. Обида весит больше: плохое помнится сильнее хорошего.
constexpr float kDebtAffinity = 0.5f;
constexpr float kDebtTrust = 0.5f;
constexpr float kGrudgeAffinity = 0.8f;
constexpr float kGrudgeTrust = 1.0f;

// Знакомые с начала мира: соседи по дому — хорошо, по поселению — немного.
constexpr float kHousemateFamiliarity = 0.7f;
constexpr float kHousemateAffinity = 0.8f;  // доля совместимости
constexpr float kNeighbourFamiliarity = 0.25f;
constexpr float kNeighbourAffinity = 0.3f;
// Сколько ближайших соседей по поселению знает каждый и как далеко они живут.
constexpr std::size_t kNeighboursKnown = 3;
constexpr float kNeighbourRange = 200.0f; // метры между дверями

float clampUnit(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

// Начальная связь: знакомство и симпатия как у давних знакомых.
void befriendAtStart(Bond& bond, float familiarity, float affinityShare) {
    bond.familiarity = familiarity;
    bond.affinity = clampUnit(affinityShare * bond.chemistry);
    bond.trust = std::max(0.0f, bond.affinity) * bond.familiarity;
    // Начальные отметки — без летописи: они знакомы давно.
    updateBondFlags(bond);
}

} // namespace

float chemistry(EntityId self, const Traits& mine, EntityId other, const Traits& theirs) {
    Hasher hasher;
    hasher.add(std::min(self, other));
    hasher.add(std::max(self, other));
    // Старшие 24 бита — точное число в float.
    const float u = static_cast<float>(hasher.value() >> 40) / static_cast<float>(1u << 24);
    const float pair = kPairMin + kPairSpread * u;
    const float kind = kKindness * (theirs.altruism - 0.5f);
    const float harsh = -kHarshness * (theirs.aggression - 0.5f) * (1.0f - mine.aggression);
    const float warm = kWarmth * (mine.sociability - 0.5f);
    return clampUnit(pair + kind + harsh + warm);
}

float bondStrength(const Bond& bond) {
    return bond.familiarity + 0.5f * std::abs(bond.affinity) + bond.debt + bond.grudge;
}

float closeness(const Bond& bond) {
    return std::clamp(bond.familiarity * (0.5f + 0.5f * bond.affinity), 0.0f, 1.0f);
}

float affinityGoal(const Bond& bond) {
    return clampUnit(bond.chemistry + kDebtAffinity * bond.debt - kGrudgeAffinity * bond.grudge);
}

float trustCeiling(const Bond& bond) {
    return std::max(0.0f, bond.affinity) * bond.familiarity * (1.0f - bond.grudge);
}

void addDebt(Bond& bond, float amount) {
    // Сначала добро гасит обиду, остаток — долг.
    const float forgiven = std::min(amount, bond.grudge);
    bond.grudge -= forgiven;
    const float added = std::min(amount - forgiven, 1.0f - bond.debt);
    bond.debt += added;
    bond.affinity = clampUnit(bond.affinity + kDebtAffinity * (forgiven + added));
    bond.trust = clampUnit(bond.trust + kDebtTrust * (forgiven + added));
}

void addGrudge(Bond& bond, float amount) {
    const float added = std::min(amount, 1.0f - bond.grudge);
    bond.grudge += added;
    bond.affinity = clampUnit(bond.affinity - kGrudgeAffinity * added);
    bond.trust = clampUnit(bond.trust - kGrudgeTrust * added);
}

Bond* acquireBond(Relations& relations, EntityId other, float chemistry, std::uint64_t tick,
                  bool* created) {
    if (created != nullptr) {
        *created = false;
    }
    if (Bond* bond = relations.find(other)) {
        return bond;
    }
    std::size_t slot = relations.count;
    if (relations.count == Relations::kMaxBonds) {
        // Самая слабая, при равных — первая.
        slot = 0;
        for (std::size_t i = 1; i < relations.count; ++i) {
            if (bondStrength(relations.bonds[i]) < bondStrength(relations.bonds[slot])) {
                slot = i;
            }
        }
        if (bondStrength(relations.bonds[slot]) >= kForgettable) {
            return nullptr;
        }
    } else {
        ++relations.count;
    }
    Bond& bond = relations.bonds[slot];
    bond = {};
    bond.other = other;
    bond.chemistry = chemistry;
    bond.met = tick;
    bond.seen = tick;
    if (created != nullptr) {
        *created = true;
    }
    return &bond;
}

std::uint8_t updateBondFlags(Bond& bond) {
    std::uint8_t raised = 0;
    if (!bond.friendly() && bond.affinity >= kFriendAffinity &&
        bond.familiarity >= kFriendFamiliarity) {
        bond.flags |= Bond::kFriend;
        raised = Bond::kFriend;
    } else if (bond.friendly() && bond.affinity < kFriendLost) {
        bond.flags &= static_cast<std::uint8_t>(~Bond::kFriend);
    }
    if (!bond.hostile() && bond.affinity <= kFoeAffinity && bond.familiarity >= kFoeFamiliarity) {
        bond.flags |= Bond::kFoe;
        raised = Bond::kFoe;
    } else if (bond.hostile() && bond.affinity > kFoeLost) {
        bond.flags &= static_cast<std::uint8_t>(~Bond::kFoe);
    }
    return raised;
}

void refreshBondFlags(World& world, EntityId self, Bond& bond) {
    const bool wasFoe = bond.hostile();
    switch (updateBondFlags(bond)) {
    case Bond::kFriend:
        world.record(ChronicleKind::Befriended, self, bond.other);
        break;
    case Bond::kFoe:
        world.record(ChronicleKind::Disliked, self, bond.other);
        break;
    default:
        break;
    }
    if (wasFoe && !bond.hostile()) {
        world.record(ChronicleKind::Reconciled, self, bond.other);
    }
}

Reputation reputationOf(const World& world, EntityId id) {
    Reputation reputation;
    float sum = 0.0f;
    for (const auto [entity, person, relations] :
         world.registry().view<const Person, const Relations>().each()) {
        const Bond* bond = relations.find(id);
        if (bond != nullptr && bond->familiarity >= kReputationFamiliarity) {
            sum += bond->affinity;
            ++reputation.known;
        }
    }
    reputation.affinity = reputation.known > 0 ? sum / static_cast<float>(reputation.known) : 0.0f;
    return reputation;
}

void introduceNeighbours(World& world) {
    entt::registry& registry = world.registry();
    struct Resident {
        EntityId settlement;
        EntityId home;
        EntityId id;
        entt::entity entity;
        float x;
        float y;
    };
    std::vector<Resident> residents;
    for (const auto [entity, id, person, relations] :
         registry.view<const Id, const Person, const Relations>().each()) {
        const entt::entity home = world.find(person.home);
        if (home == entt::null) {
            continue; // бездомный никого не знает с начала
        }
        const Building& building = registry.get<Building>(home);
        residents.push_back({building.settlement, person.home, id.value, entity,
                             Map::cellCenter(building.door.x), Map::cellCenter(building.door.y)});
    }
    // По поселению, затем по id: порядок не зависит от хранения в реестре.
    std::sort(residents.begin(), residents.end(), [](const Resident& a, const Resident& b) {
        return std::tie(a.settlement, a.id) < std::tie(b.settlement, b.id);
    });
    static const Traits kAverage{};
    const auto traitsOf = [&](entt::entity entity) -> const Traits& {
        const Traits* traits = registry.try_get<Traits>(entity);
        return traits != nullptr ? *traits : kAverage;
    };
    const std::uint64_t tick = world.tick();
    std::vector<std::pair<float, std::size_t>> nearby;
    for (std::size_t begin = 0; begin < residents.size();) {
        std::size_t end = begin;
        while (end < residents.size() && residents[end].settlement == residents[begin].settlement) {
            ++end;
        }
        for (std::size_t i = begin; i < end; ++i) {
            const Resident& self = residents[i];
            Relations& relations = registry.get<Relations>(self.entity);
            const Traits& mine = traitsOf(self.entity);
            const auto meet = [&](const Resident& other, float familiarity, float affinityShare,
                                  float bonus) {
                const float chem =
                    clampUnit(chemistry(self.id, mine, other.id, traitsOf(other.entity)) + bonus);
                bool created = false;
                if (Bond* bond = acquireBond(relations, other.id, chem, tick, &created); created) {
                    befriendAtStart(*bond, familiarity, affinityShare);
                }
            };
            // Соседи по дому — первыми: им места хватит всегда.
            nearby.clear();
            for (std::size_t j = begin; j < end; ++j) {
                const Resident& other = residents[j];
                if (j == i) {
                    continue;
                }
                if (other.home == self.home) {
                    meet(other, kHousemateFamiliarity, kHousemateAffinity, kHousemateChemistry);
                    continue;
                }
                const float d = std::hypot(other.x - self.x, other.y - self.y);
                if (d <= kNeighbourRange) {
                    nearby.emplace_back(d, j);
                }
            }
            // Ближайшие соседи из других домов; при равных расстояниях — меньший id.
            const std::size_t known = std::min(kNeighboursKnown, nearby.size());
            std::partial_sort(nearby.begin(), nearby.begin() + static_cast<std::ptrdiff_t>(known),
                              nearby.end());
            for (std::size_t k = 0; k < known; ++k) {
                meet(residents[nearby[k].second], kNeighbourFamiliarity, kNeighbourAffinity, 0.0f);
            }
        }
        begin = end;
    }
}

} // namespace lw
