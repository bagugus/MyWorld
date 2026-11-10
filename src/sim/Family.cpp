#include "sim/Family.h"

#include "sim/Hash.h"
#include "sim/Names.h"
#include "sim/Relations.h"
#include "sim/Rng.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <tuple>
#include <utility>

namespace lw {

namespace {

// --- Состав домов ---------------------------------------------------------

// Доли видов домохозяйств, проценты (по порядку HouseholdKind).
constexpr int kHouseholdPercent[] = {22, 18, 30, 8, 7, 9, 6};
static_assert(std::size(kHouseholdPercent) == static_cast<std::size_t>(HouseholdKind::Count));
// Дети: самый младший возраст (младенцев симуляция не носит на руках) и
// разница в возрасте с матерью.
constexpr int kChildMinAge = 4;
constexpr int kChildAtHomeMaxAge = 24;
constexpr int kMotherGapMin = 19;
constexpr int kMotherGapMax = 40;
constexpr int kChildAttempts = 6;
// Пожилые: выросшие дети живут отдельно с такой вероятностью (второй ребёнок —
// с kSecondChildChance), разница в возрасте с ними — от и до.
constexpr int kElderAge = 60;
constexpr float kGrownChildChance = 0.65f;
constexpr float kSecondChildChance = 0.35f;
constexpr int kGrownGapMin = 20;
constexpr int kGrownGapMax = 42;
constexpr int kGrownMinAge = 24;

// --- Знакомства родни -----------------------------------------------------

// Родня знакома всю жизнь: живущие вместе — полностью, порознь — чуть меньше.
constexpr float kKinFamiliarity = 1.0f;
constexpr float kKinApartFamiliarity = 0.85f;
// Надбавка к совместимости по родству (по порядку KinKind).
constexpr float kKinBonus[] = {0.6f, 0.6f, 0.6f, 0.4f, 0.5f, 0.5f};
static_assert(std::size(kKinBonus) == static_cast<std::size_t>(KinKind::Count));
// В части семей разлад: несчастливый брак, обида ребёнка на отца. Тогда
// вместо надбавки — kDiscordBonus. Решает хеш пары (одинаково с обеих сторон).
constexpr float kDiscordShare = 0.1f;
constexpr float kDiscordBonus = -0.1f;

// --- Коллеги и одноклассники ----------------------------------------------

constexpr float kColleagueFamiliarity = 0.5f;
constexpr float kColleagueAffinity = 0.6f;
// Школьник знает столько ровесников из своей школы (разница не больше
// kClassmateAgeGap лет) и своих учителей; учитель знает учеников меньше.
constexpr std::size_t kClassmatesKnown = 4;
constexpr int kClassmateAgeGap = 1;
constexpr float kClassmateFamiliarity = 0.5f;
constexpr float kClassmateAffinity = 0.6f;
constexpr float kTeacherFamiliarity = 0.4f;
constexpr float kPupilFamiliarity = 0.25f;
constexpr float kTeacherAffinity = 0.4f;

// --- Прошлое --------------------------------------------------------------

// Вероятность у взрослого: давний друг, давняя вражда, бывший супруг.
constexpr int kPastMinAge = 20;
constexpr float kOldFriendChance = 0.3f;
constexpr float kOldFeudChance = 0.08f;
constexpr float kExPartnerChance = 0.06f;
constexpr int kOldFriendAgeGap = 6;
constexpr int kExPartnerAgeGap = 8;
constexpr int kExPartnerMinAge = 25;
// Давние друзья: знакомство и надбавка к совместимости.
constexpr float kOldFriendFamiliarity = 0.6f;
constexpr float kOldFriendBonus = 0.5f;
constexpr float kOldFriendMinChemistry = 0.55f;
// Вражда — с соседом не дальше kFeudRange метров между дверями: совместимость
// не выше kFeudChemistry, симпатия и доверие — такие.
constexpr float kFeudRange = 300.0f;
constexpr float kFeudFamiliarity = 0.5f;
constexpr float kFeudChemistry = -0.5f;
constexpr float kFeudAffinity = -0.45f;
constexpr float kFeudTrust = -0.2f;
// Бывшие супруги: знают друг друга хорошо, чувства смешанные.
constexpr float kExFamiliarity = 0.7f;
constexpr float kExBonus = -0.3f;

std::uint8_t clampAge(int years) {
    return static_cast<std::uint8_t>(std::clamp(years, 0, 100));
}

// Строитель плана: люди, родство, опекуны.
struct Planner {
    Rng& rng;
    HouseholdPlan plan;
    std::vector<int> kinCount;
    int household = 0;
    int remaining = 0;

    int person(int age, bool female, int lineage) {
        if (remaining <= 0) {
            return -1;
        }
        --remaining;
        plan.people.push_back({clampAge(age), female, household, lineage});
        plan.guardian.push_back(-1);
        kinCount.push_back(0);
        return static_cast<int>(plan.people.size()) - 1;
    }
    bool related(int a, int b) const {
        return std::any_of(plan.kin.begin(), plan.kin.end(),
                           [&](const PlannedKin& k) { return k.a == a && k.b == b; });
    }
    // b приходится a тем, что kind; в обе стороны, пока у обоих есть место.
    void link(int a, int b, KinKind kind) {
        if (a < 0 || b < 0 || a == b || related(a, b) ||
            kinCount[static_cast<std::size_t>(a)] >= static_cast<int>(Kin::kMaxRelatives) ||
            kinCount[static_cast<std::size_t>(b)] >= static_cast<int>(Kin::kMaxRelatives)) {
            return;
        }
        plan.kin.push_back({a, b, kind});
        plan.kin.push_back({b, a, inverseKin(kind)});
        ++kinCount[static_cast<std::size_t>(a)];
        ++kinCount[static_cast<std::size_t>(b)];
    }
    int newLineage() { return plan.lineages++; }

    // Дети матери (или отца) mother: возраст по разнице с ней, родство со
    // всеми родителями и между собой; опекун — мать.
    void children(const std::vector<int>& parents, int mother, int count, int lineage) {
        std::vector<int> kids;
        const int motherAge = plan.people[static_cast<std::size_t>(mother)].age;
        for (int i = 0; i < count; ++i) {
            int age = -1;
            for (int attempt = 0; attempt < kChildAttempts && age < 0; ++attempt) {
                const int candidate = motherAge - rng.rangeInt(kMotherGapMin, kMotherGapMax);
                if (candidate >= kChildMinAge && candidate <= kChildAtHomeMaxAge) {
                    age = candidate;
                }
            }
            if (age < 0) {
                continue;
            }
            const int kid = person(age, rng.chance(0.5f), lineage);
            if (kid < 0) {
                break;
            }
            for (const int parent : parents) {
                link(kid, parent, KinKind::Parent);
            }
            for (const int sibling : kids) {
                link(kid, sibling, KinKind::Sibling);
            }
            if (age < kAdultAge) {
                plan.guardian[static_cast<std::size_t>(kid)] = mother;
            }
            kids.push_back(kid);
        }
    }

    int childCount() {
        const int roll = rng.rangeInt(0, 99);
        return roll < 40 ? 1 : roll < 80 ? 2 : 3;
    }

    void build(HouseholdKind kind) {
        const int lineage = newLineage();
        switch (kind) {
        case HouseholdKind::Single:
            person(rng.rangeInt(20, 79), rng.chance(0.5f), lineage);
            break;
        case HouseholdKind::Couple: {
            const int age = rng.rangeInt(22, 74);
            const int man = person(age, false, lineage);
            const int woman = person(std::max(20, age + rng.rangeInt(-6, 3)), true, lineage);
            link(man, woman, KinKind::Spouse);
            break;
        }
        case HouseholdKind::Family:
        case HouseholdKind::ThreeGenerations: {
            const int motherAge = rng.rangeInt(26, 48);
            const int mother = person(motherAge, true, lineage);
            const int father = person(std::max(20, motherAge + rng.rangeInt(-2, 6)), false, lineage);
            link(mother, father, KinKind::Spouse);
            if (kind == HouseholdKind::ThreeGenerations && mother >= 0) {
                // Дед или бабушка — родитель отца (тот же род) или матери.
                const bool paternal = father >= 0 && rng.chance(0.5f);
                const int child = paternal ? father : mother;
                const int age = std::max(kElderAge, plan.people[static_cast<std::size_t>(child)].age +
                                                        rng.rangeInt(22, 34));
                const int elder = person(age, rng.chance(0.65f), paternal ? lineage : newLineage());
                link(child, elder, KinKind::Parent);
                const std::size_t before = plan.people.size();
                if (mother >= 0) {
                    std::vector<int> parents{mother};
                    if (father >= 0) {
                        parents.push_back(father);
                    }
                    children(parents, mother, childCount(), lineage);
                }
                for (std::size_t kid = before; kid < plan.people.size(); ++kid) {
                    link(static_cast<int>(kid), elder, KinKind::Grandparent);
                }
                break;
            }
            if (mother >= 0) {
                std::vector<int> parents{mother};
                if (father >= 0) {
                    parents.push_back(father);
                }
                children(parents, mother, childCount(), lineage);
            }
            break;
        }
        case HouseholdKind::SingleParent: {
            const bool female = rng.chance(0.75f);
            const int parent = person(rng.rangeInt(24, 52), female, lineage);
            if (parent >= 0) {
                children({parent}, parent, rng.chance(0.6f) ? 1 : 2, lineage);
            }
            break;
        }
        case HouseholdKind::Elderly: {
            const int age = rng.rangeInt(63, 86);
            const int man = person(age, false, lineage);
            const int woman = person(std::max(kElderAge, age + rng.rangeInt(-6, 2)), true, lineage);
            link(man, woman, KinKind::Spouse);
            break;
        }
        case HouseholdKind::Sharing:
            if (rng.chance(0.5f)) {
                // Взрослые брат и сестра (или братья, сёстры).
                const int age = rng.rangeInt(19, 40);
                const int first = person(age, rng.chance(0.5f), lineage);
                const int second = person(std::max(18, age + rng.rangeInt(-6, 6)), rng.chance(0.5f), lineage);
                link(first, second, KinKind::Sibling);
            } else {
                person(rng.rangeInt(19, 35), rng.chance(0.5f), lineage);
                person(rng.rangeInt(19, 35), rng.chance(0.5f), newLineage());
            }
            break;
        case HouseholdKind::Count:
            break;
        }
    }

    // Род людей рода from — теперь род to (сын носит фамилию родителей).
    void relabel(int from, int to) {
        if (from == to) {
            return;
        }
        for (PlannedPerson& p : plan.people) {
            if (p.lineage == from) {
                p.lineage = to;
            }
        }
    }

    // Выросшие дети пожилых — в своих домах.
    void grownChildren() {
        const std::size_t total = plan.people.size();
        std::vector<bool> hasParents(total, false);
        for (const PlannedKin& k : plan.kin) {
            if (k.kind == KinKind::Parent) {
                hasParents[static_cast<std::size_t>(k.a)] = true;
            }
        }
        for (int h = 0; h < static_cast<int>(plan.households.size()); ++h) {
            std::vector<int> elders;
            for (std::size_t i = 0; i < total; ++i) {
                if (plan.people[i].household == h && plan.people[i].age >= kElderAge) {
                    elders.push_back(static_cast<int>(i));
                }
            }
            const HouseholdKind kind = plan.households[static_cast<std::size_t>(h)];
            if (elders.empty() || (kind != HouseholdKind::Elderly && kind != HouseholdKind::Single) ||
                !rng.chance(kGrownChildChance)) {
                continue;
            }
            // Возраст матери (если есть), иначе старшего.
            int reference = elders.front();
            for (const int elder : elders) {
                if (plan.people[static_cast<std::size_t>(elder)].female) {
                    reference = elder;
                }
            }
            const int referenceAge = plan.people[static_cast<std::size_t>(reference)].age;
            const int wanted = rng.chance(kSecondChildChance) ? 2 : 1;
            std::vector<int> grown;
            for (const PlannedKin& k : plan.kin) {
                if (k.a == elders.front() && k.kind == KinKind::Child) {
                    grown.push_back(k.b);
                }
            }
            for (int n = 0; n < wanted; ++n) {
                std::vector<int> candidates;
                for (std::size_t i = 0; i < total; ++i) {
                    const PlannedPerson& p = plan.people[i];
                    const int gap = referenceAge - p.age;
                    if (p.household != h && !hasParents[i] && p.age >= kGrownMinAge &&
                        gap >= kGrownGapMin && gap <= kGrownGapMax) {
                        candidates.push_back(static_cast<int>(i));
                    }
                }
                if (candidates.empty()) {
                    break;
                }
                const int child = candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))];
                hasParents[static_cast<std::size_t>(child)] = true;
                for (const int elder : elders) {
                    link(child, elder, KinKind::Parent);
                }
                for (const int sibling : grown) {
                    link(child, sibling, KinKind::Sibling);
                }
                grown.push_back(child);
                // Внуки — дети выросшего ребёнка.
                std::vector<int> grandchildren;
                for (const PlannedKin& k : plan.kin) {
                    if (k.a == child && k.kind == KinKind::Child) {
                        grandchildren.push_back(k.b);
                    }
                }
                for (const int grandchild : grandchildren) {
                    for (const int elder : elders) {
                        link(grandchild, elder, KinKind::Grandparent);
                    }
                }
                // Фамилия: сын и незамужняя дочь носят фамилию родителей.
                const PlannedPerson& c = plan.people[static_cast<std::size_t>(child)];
                const bool married = std::any_of(plan.kin.begin(), plan.kin.end(), [&](const PlannedKin& k) {
                    return k.a == child && k.kind == KinKind::Spouse;
                });
                if (!c.female || !married) {
                    // Его дом (супруга, дети) — того же рода, что и родители.
                    relabel(c.lineage, plan.people[static_cast<std::size_t>(elders.front())].lineage);
                }
            }
        }
    }
};

HouseholdKind rollHousehold(Rng& rng) {
    int roll = rng.rangeInt(0, 99);
    std::size_t kind = 0;
    while (kind + 1 < std::size(kHouseholdPercent) && roll >= kHouseholdPercent[kind]) {
        roll -= kHouseholdPercent[kind];
        ++kind;
    }
    return static_cast<HouseholdKind>(kind);
}

// Разлад в семье: по хешу пары, одинаково с обеих сторон.
bool discord(EntityId a, EntityId b) {
    Hasher hasher;
    hasher.add(std::min(a, b));
    hasher.add(std::max(a, b));
    hasher.add(std::uint64_t{0x6B696E}); // «kin»: не совпадать с совместимостью
    const float u = static_cast<float>(hasher.value() >> 40) / static_cast<float>(1u << 24);
    return u < kDiscordShare;
}

// Житель для знакомств: дом, поселение, дверь, возраст, пол.
struct Resident {
    EntityId id;
    entt::entity entity;
    EntityId home;
    EntityId settlement;
    float x;
    float y;
    int age;
    bool female;
};

std::vector<Resident> residentsOf(const World& world) {
    const entt::registry& registry = world.registry();
    std::vector<Resident> result;
    for (const auto [entity, id, person, relations] :
         registry.view<const Id, const Person, const Relations>().each()) {
        Resident r{id.value, entity, person.home, kInvalidId, 0.0f, 0.0f, ageOf(registry, entity), false};
        if (const entt::entity home = world.find(person.home); home != entt::null) {
            const Building& building = registry.get<Building>(home);
            r.settlement = building.settlement;
            r.x = Map::cellCenter(building.door.x);
            r.y = Map::cellCenter(building.door.y);
        }
        if (const auto* name = registry.try_get<Name>(entity)) {
            r.female = isFemaleName(name->first);
        }
        result.push_back(r);
    }
    std::sort(result.begin(), result.end(), [](const Resident& a, const Resident& b) { return a.id < b.id; });
    return result;
}

bool isKin(const entt::registry& registry, entt::entity self, EntityId other) {
    return kinOf(registry, self, other) != KinKind::Count;
}

} // namespace

std::uint8_t ageOf(const entt::registry& registry, entt::entity entity) {
    const auto* age = entity != entt::null ? registry.try_get<Age>(entity) : nullptr;
    return age != nullptr ? age->years : std::uint8_t{30};
}

bool isMinor(const entt::registry& registry, entt::entity entity) {
    return ageOf(registry, entity) < kAdultAge;
}

KinKind kinOf(const entt::registry& registry, entt::entity self, EntityId other) {
    const auto* kin = self != entt::null ? registry.try_get<Kin>(self) : nullptr;
    const Relative* relative = kin != nullptr ? kin->find(other) : nullptr;
    return relative != nullptr ? relative->kind : KinKind::Count;
}

KinKind inverseKin(KinKind kind) {
    switch (kind) {
    case KinKind::Parent:
        return KinKind::Child;
    case KinKind::Child:
        return KinKind::Parent;
    case KinKind::Grandparent:
        return KinKind::Grandchild;
    case KinKind::Grandchild:
        return KinKind::Grandparent;
    default:
        return kind; // супруг и брат — взаимно
    }
}

std::int32_t kinCode(const World& world, entt::entity self, EntityId other) {
    const entt::registry& registry = world.registry();
    const KinKind kind = kinOf(registry, self, other);
    if (kind == KinKind::Count) {
        return 0;
    }
    const entt::entity entity = world.find(other);
    const auto* name = entity != entt::null ? registry.try_get<Name>(entity) : nullptr;
    const bool female = name != nullptr && isFemaleName(name->first);
    return 1 + 2 * static_cast<std::int32_t>(kind) + (female ? 1 : 0);
}

bool decodeKin(std::int32_t code, KinKind& kind, bool& female) {
    if (code <= 0 || code > 2 * static_cast<std::int32_t>(KinKind::Count)) {
        return false;
    }
    kind = static_cast<KinKind>((code - 1) / 2);
    female = ((code - 1) & 1) != 0;
    return true;
}

bool caresFor(const World& world, entt::entity self, EntityId child) {
    const entt::registry& registry = world.registry();
    const entt::entity entity = world.find(child);
    if (entity == entt::null || !registry.all_of<Person>(entity) || !isMinor(registry, entity)) {
        return false;
    }
    const auto* kin = registry.try_get<Kin>(entity);
    if (kin == nullptr) {
        return false;
    }
    const EntityId selfId = registry.get<Id>(self).value;
    const Relative* relative = kin->find(selfId);
    return kin->guardian == selfId || (relative != nullptr && relative->kind == KinKind::Parent);
}

HouseholdPlan planHouseholds(Rng& rng, int count) {
    Planner planner{rng, {}, {}, 0, std::max(0, count)};
    while (planner.remaining > 0) {
        const HouseholdKind kind = rollHousehold(rng);
        planner.household = static_cast<int>(planner.plan.households.size());
        planner.plan.households.push_back(kind);
        planner.build(kind);
    }
    planner.grownChildren();
    return std::move(planner.plan);
}

void introduceFamilies(World& world) {
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> people;
    for (const auto [entity, id, kin] : registry.view<const Id, const Kin>().each()) {
        if (registry.all_of<Person, Relations>(entity)) {
            people.emplace_back(id.value, entity);
        }
    }
    std::sort(people.begin(), people.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, entity] : people) {
        const Kin kin = registry.get<Kin>(entity);
        const EntityId home = registry.get<Person>(entity).home;
        for (std::size_t i = 0; i < kin.count; ++i) {
            const Relative& relative = kin.relatives[i];
            const entt::entity other = world.find(relative.id);
            if (other == entt::null || !registry.all_of<Person>(other)) {
                continue;
            }
            const bool together = home != kInvalidId && registry.get<Person>(other).home == home;
            const float bonus =
                discord(id, relative.id) ? kDiscordBonus : kKinBonus[static_cast<std::size_t>(relative.kind)];
            meetAtStart(world, entity, id, other, relative.id,
                        together ? kKinFamiliarity : kKinApartFamiliarity, 1.0f, bonus, BondOrigin::Kin);
        }
    }
}

void introduceColleagues(World& world) {
    entt::registry& registry = world.registry();
    struct Worker {
        EntityId workplace;
        Profession profession;
        EntityId id;
        entt::entity entity;
        int age;
    };
    std::vector<Worker> workers;
    for (const auto [entity, id, job] : registry.view<const Id, const Job>().each()) {
        if (job.profession != Profession::None && job.workplace != kInvalidId &&
            registry.all_of<Person, Relations>(entity)) {
            workers.push_back({job.workplace, job.profession, id.value, entity, ageOf(registry, entity)});
        }
    }
    std::sort(workers.begin(), workers.end(), [](const Worker& a, const Worker& b) {
        return std::tie(a.workplace, a.id) < std::tie(b.workplace, b.id);
    });
    std::vector<std::pair<int, std::size_t>> mates;
    for (std::size_t begin = 0; begin < workers.size();) {
        std::size_t end = begin;
        while (end < workers.size() && workers[end].workplace == workers[begin].workplace) {
            ++end;
        }
        for (std::size_t i = begin; i < end; ++i) {
            const Worker& self = workers[i];
            const auto meet = [&](const Worker& other, float familiarity, float share, BondOrigin origin) {
                meetAtStart(world, self.entity, self.id, other.entity, other.id, familiarity, share, 0.0f,
                            origin);
            };
            if (self.profession != Profession::Pupil) {
                // Коллеги; учитель — ещё и ученики своей школы, по возрасту.
                for (std::size_t j = begin; j < end; ++j) {
                    const Worker& other = workers[j];
                    if (j == i) {
                        continue;
                    }
                    if (other.profession == Profession::Pupil) {
                        meet(other, kPupilFamiliarity, kTeacherAffinity, BondOrigin::Classmate);
                    } else {
                        meet(other, kColleagueFamiliarity, kColleagueAffinity, BondOrigin::Colleague);
                    }
                }
                continue;
            }
            // Школьник: учителя и самые близкие по возрасту одноклассники.
            mates.clear();
            for (std::size_t j = begin; j < end; ++j) {
                const Worker& other = workers[j];
                if (j == i) {
                    continue;
                }
                if (other.profession != Profession::Pupil) {
                    meet(other, kTeacherFamiliarity, kTeacherAffinity, BondOrigin::Classmate);
                } else if (std::abs(other.age - self.age) <= kClassmateAgeGap) {
                    mates.emplace_back(std::abs(other.age - self.age), j);
                }
            }
            const std::size_t known = std::min(kClassmatesKnown, mates.size());
            std::partial_sort(mates.begin(), mates.begin() + static_cast<std::ptrdiff_t>(known), mates.end());
            for (std::size_t k = 0; k < known; ++k) {
                meet(workers[mates[k].second], kClassmateFamiliarity, kClassmateAffinity, BondOrigin::Classmate);
            }
        }
        begin = end;
    }
}

void weavePast(World& world) {
    entt::registry& registry = world.registry();
    Rng& rng = world.rng(RngStream::Families);
    const std::vector<Resident> residents = residentsOf(world);
    std::vector<std::size_t> adults;
    for (std::size_t i = 0; i < residents.size(); ++i) {
        if (residents[i].age >= kPastMinAge) {
            adults.push_back(i);
        }
    }
    const auto pick = [&](const std::vector<std::size_t>& candidates) -> const Resident* {
        if (candidates.empty()) {
            return nullptr;
        }
        return &residents[candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))]];
    };
    // Связь в обе стороны: заводится, если её не было (или это не родня — тогда
    // перекраивается), и получает происхождение origin.
    const auto tie = [&](const Resident& a, const Resident& b, BondOrigin origin) {
        for (int side = 0; side < 2; ++side) {
            const Resident& self = side == 0 ? a : b;
            const Resident& other = side == 0 ? b : a;
            auto& relations = registry.get<Relations>(self.entity);
            Bond* bond = relations.find(other.id);
            if (bond == nullptr) {
                bond = meetAtStart(world, self.entity, self.id, other.entity, other.id, 0.0f, 0.0f, 0.0f, origin);
            }
            if (bond == nullptr || bond->origin == BondOrigin::Kin) {
                continue;
            }
            bond->origin = origin;
            switch (origin) {
            case BondOrigin::OldFriend:
                bond->familiarity = std::max(bond->familiarity, kOldFriendFamiliarity);
                bond->chemistry = std::max(kOldFriendMinChemistry, std::min(1.0f, bond->chemistry + kOldFriendBonus));
                bond->affinity = bond->chemistry;
                bond->trust = bond->affinity * bond->familiarity;
                break;
            case BondOrigin::OldFeud:
                bond->familiarity = std::max(bond->familiarity, kFeudFamiliarity);
                bond->chemistry = std::min(bond->chemistry, kFeudChemistry);
                bond->affinity = kFeudAffinity;
                bond->trust = kFeudTrust;
                break;
            case BondOrigin::ExPartner:
                bond->familiarity = std::max(bond->familiarity, kExFamiliarity);
                bond->chemistry = std::clamp(bond->chemistry + kExBonus, -1.0f, 1.0f);
                bond->affinity = bond->chemistry;
                bond->trust = std::max(0.0f, bond->affinity) * bond->familiarity;
                break;
            default:
                break;
            }
            updateBondFlags(*bond);
        }
    };
    const auto available = [&](const Resident& self, const Resident& other) {
        if (other.id == self.id || isKin(registry, self.entity, other.id)) {
            return false;
        }
        const Bond* bond = registry.get<Relations>(self.entity).find(other.id);
        return bond == nullptr || bond->origin == BondOrigin::Met || bond->origin == BondOrigin::Neighbour ||
               bond->origin == BondOrigin::Housemate;
    };
    std::vector<std::size_t> nearby;
    std::vector<std::size_t> anywhere;
    for (const std::size_t i : adults) {
        const Resident& self = residents[i];
        // Давний друг: ровесник, лучше — из своего поселения.
        if (rng.chance(kOldFriendChance)) {
            nearby.clear();
            anywhere.clear();
            for (const std::size_t j : adults) {
                const Resident& other = residents[j];
                if (available(self, other) && std::abs(other.age - self.age) <= kOldFriendAgeGap) {
                    anywhere.push_back(j);
                    if (other.settlement == self.settlement && self.settlement != kInvalidId) {
                        nearby.push_back(j);
                    }
                }
            }
            if (const Resident* other = pick(nearby.empty() ? anywhere : nearby)) {
                tie(self, *other, BondOrigin::OldFriend);
            }
        }
        // Давняя вражда: сосед — межа, шум, старые счёты.
        if (rng.chance(kOldFeudChance)) {
            nearby.clear();
            for (const std::size_t j : adults) {
                const Resident& other = residents[j];
                if (available(self, other) && other.home != self.home && other.settlement == self.settlement &&
                    self.settlement != kInvalidId &&
                    std::hypot(other.x - self.x, other.y - self.y) <= kFeudRange) {
                    nearby.push_back(j);
                }
            }
            if (const Resident* other = pick(nearby)) {
                tie(self, *other, BondOrigin::OldFeud);
            }
        }
        // Бывшие супруги.
        if (self.age >= kExPartnerMinAge && rng.chance(kExPartnerChance)) {
            nearby.clear();
            anywhere.clear();
            for (const std::size_t j : adults) {
                const Resident& other = residents[j];
                if (available(self, other) && other.female != self.female && other.age >= kExPartnerMinAge &&
                    std::abs(other.age - self.age) <= kExPartnerAgeGap) {
                    anywhere.push_back(j);
                    if (other.settlement == self.settlement && self.settlement != kInvalidId) {
                        nearby.push_back(j);
                    }
                }
            }
            if (const Resident* other = pick(nearby.empty() ? anywhere : nearby)) {
                tie(self, *other, BondOrigin::ExPartner);
            }
        }
    }
}

} // namespace lw
