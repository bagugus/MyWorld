#include "sim/systems/GroupSystem.h"

#include "sim/Memory.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Собрать группу может человек с лидерством не ниже этого — со взаимным
// другом, если оба доверяют друг другу не меньше kFoundTrust.
constexpr float kFoundLeadership = 0.45f;
constexpr float kFoundTrust = 0.3f;
// Вступает друг вожака, если доверяет ему не меньше этого, или друг половины
// участников, если вожаку хотя бы не враг.
constexpr float kJoinTrust = 0.25f;
// Дом вступающего — не дальше этого от базы, метры: туда переезжают.
constexpr float kGroupRange = 1500.0f;
// Авторитет — лидерство + kAuthorityTrust × доверие участников; сместить
// вожака может тот, чей авторитет выше на kCoupMargin.
constexpr float kAuthorityTrust = 0.5f;
constexpr float kCoupMargin = 0.2f;
// Уходит, если симпатия к вожаку ниже этого; изгоняют, если вожак его
// невзлюбил или средняя симпатия остальных к нему не выше kExpelAffinity.
constexpr float kLeaveAffinity = -0.1f;
constexpr float kExpelAffinity = -0.2f;

const Bond* bondOf(const entt::registry& registry, entt::entity self, EntityId other) {
    const auto* relations = self != entt::null ? registry.try_get<Relations>(self) : nullptr;
    return relations != nullptr ? relations->find(other) : nullptr;
}

// Живой человек с таким id; entt::null — нет (погиб, встал зомби, исчез).
entt::entity personOf(const World& world, EntityId id) {
    const entt::entity entity = world.find(id);
    return entity != entt::null && world.registry().all_of<Person>(entity) ? entity
                                                                           : entt::entity{entt::null};
}

// Где человек живёт: дверь дома; бездомный — где стоит.
std::pair<float, float> homeSpot(const World& world, entt::entity entity) {
    const entt::registry& registry = world.registry();
    if (const entt::entity home = world.find(registry.get<Person>(entity).home);
        home != entt::null) {
        const Cell door = registry.get<Building>(home).door;
        return {Map::cellCenter(door.x), Map::cellCenter(door.y)};
    }
    const Position& position = registry.get<Position>(entity);
    return {position.x, position.y};
}

float distance(std::pair<float, float> a, std::pair<float, float> b) {
    return std::hypot(a.first - b.first, a.second - b.second);
}

float leadershipOf(const entt::registry& registry, entt::entity entity) {
    const auto* traits = registry.try_get<Traits>(entity);
    return traits != nullptr ? traits->leadership : 0.5f;
}

// Самый авторитетный участник; при равных — меньший id.
EntityId mostAuthoritative(const World& world, const Group& group) {
    EntityId best = kInvalidId;
    float bestAuthority = 0.0f;
    for (const EntityId member : group.members) {
        const float authority = authorityOf(world, group, member);
        if (best == kInvalidId || authority > bestAuthority) {
            best = member;
            bestAuthority = authority;
        }
    }
    return best;
}

// Вступить: переехать на базу, если она есть и это не свой дом.
void join(World& world, Group& group, EntityId groupId, entt::entity entity, EntityId id) {
    Person& person = world.registry().get<Person>(entity);
    person.group = groupId;
    const bool moves = group.base != kInvalidId && person.home != group.base;
    if (moves) {
        person.formerHome = person.home;
        person.home = group.base;
    }
    group.members.insert(std::upper_bound(group.members.begin(), group.members.end(), id), id);
    if (id != group.leader) {
        world.record(ChronicleKind::JoinedGroup, id, groupId, moves ? 1 : 0, 0, group.leader);
    }
}

// Выйти: вернуться в прежний дом, если переезжал.
void leave(World& world, Group& group, entt::entity entity, EntityId id) {
    Person& person = world.registry().get<Person>(entity);
    person.group = kInvalidId;
    if (person.formerHome != kInvalidId) {
        person.home = person.formerHome;
        person.formerHome = kInvalidId;
    }
    group.members.erase(std::remove(group.members.begin(), group.members.end(), id),
                        group.members.end());
}

// Средняя симпатия участников (кроме вожака и его самого) к member; others —
// сколько их.
float groupAffinity(const World& world, const Group& group, EntityId member, int& others) {
    const entt::registry& registry = world.registry();
    float sum = 0.0f;
    others = 0;
    for (const EntityId other : group.members) {
        if (other == member || other == group.leader) {
            continue;
        }
        ++others;
        if (const Bond* bond = bondOf(registry, world.find(other), member)) {
            sum += bond->affinity;
        }
    }
    return others > 0 ? sum / static_cast<float>(others) : 0.0f;
}

// Выбывшие, преемник, ушедшие и изгнанные, смена вожака. false — группа
// распалась.
bool reviewGroup(World& world, Group& group, EntityId groupId) {
    entt::registry& registry = world.registry();
    // Погибшие и обратившиеся (Person больше нет) выбывают.
    std::erase_if(group.members, [&](EntityId member) {
        const entt::entity entity = personOf(world, member);
        return entity == entt::null || registry.get<Person>(entity).group != groupId;
    });
    if (!group.has(group.leader)) {
        const EntityId fallen = group.leader;
        group.leader = kInvalidId;
        if (group.members.size() >= 2) {
            group.leader = mostAuthoritative(world, group);
            world.record(ChronicleKind::LedGroup, group.leader, groupId, 0, 0, fallen);
        }
    }
    if (group.leader == kInvalidId && !group.members.empty()) {
        group.leader = group.members.front(); // один остался — распадётся ниже
    }
    // Уходят и изгоняются по возрастанию id; вожак не уходит.
    const std::vector<EntityId> members = group.members;
    for (const EntityId member : members) {
        if (member == group.leader || group.members.size() < 2) {
            continue;
        }
        const entt::entity entity = world.find(member);
        const Bond* toLeader = bondOf(registry, entity, group.leader);
        if (toLeader != nullptr && (toLeader->hostile() || toLeader->affinity < kLeaveAffinity)) {
            leave(world, group, entity, member);
            world.record(ChronicleKind::LeftGroup, member, groupId, 0, 0, group.leader);
            continue;
        }
        const Bond* fromLeader = bondOf(registry, world.find(group.leader), member);
        int others = 0;
        const float liked = groupAffinity(world, group, member, others);
        if ((fromLeader != nullptr && fromLeader->hostile()) ||
            (others >= 2 && liked <= kExpelAffinity)) {
            leave(world, group, entity, member);
            world.record(ChronicleKind::Expelled, group.leader, member, 0, 0, groupId);
            experience(world, entity, MemoryKind::Expelled, group.leader, groupId);
        }
    }
    if (group.members.size() >= 2) {
        // Вожак — тот, за кем идут.
        const EntityId best = mostAuthoritative(world, group);
        if (best != group.leader && authorityOf(world, group, best) >
                                        authorityOf(world, group, group.leader) + kCoupMargin) {
            world.record(ChronicleKind::LedGroup, best, groupId, 1, 0, group.leader);
            group.leader = best;
        }
        return true;
    }
    // Остался один — живёт, где жил.
    const EntityId last = group.members.empty() ? kInvalidId : group.members.front();
    if (last != kInvalidId) {
        Person& person = registry.get<Person>(world.find(last));
        person.group = kInvalidId;
        person.formerHome = kInvalidId;
    }
    world.record(ChronicleKind::GroupDisbanded, groupId, last);
    return false;
}

// Взаимный друг для новой группы: оба считают друг друга друзьями и доверяют
// не меньше kFoundTrust, живут недалеко; самый доверенный, при равных —
// меньший id. kInvalidId — нет.
EntityId pickPartner(const World& world, entt::entity founder, EntityId founderId) {
    const entt::registry& registry = world.registry();
    const Relations& relations = registry.get<Relations>(founder);
    const auto home = homeSpot(world, founder);
    EntityId partner = kInvalidId;
    float best = 0.0f;
    for (std::size_t i = 0; i < relations.count; ++i) {
        const Bond& bond = relations.bonds[i];
        if (!bond.friendly() || bond.trust < kFoundTrust) {
            continue;
        }
        const entt::entity other = personOf(world, bond.other);
        if (other == entt::null || registry.get<Person>(other).group != kInvalidId) {
            continue;
        }
        const Bond* back = bondOf(registry, other, founderId);
        if (back == nullptr || !back->friendly() || back->trust < kFoundTrust ||
            distance(home, homeSpot(world, other)) > kGroupRange) {
            continue;
        }
        const float mutual = std::min(bond.trust, back->trust);
        if (partner == kInvalidId || mutual > best || (mutual == best && bond.other < partner)) {
            partner = bond.other;
            best = mutual;
        }
    }
    return partner;
}

// Новые группы: основатель с лидерством и его взаимный друг. Вожак — у кого
// авторитет выше, база — его дом (бездомный вожак — дом другого).
void foundGroups(World& world) {
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> loners;
    for (const auto [entity, id, person] : registry.view<const Id, const Person>().each()) {
        if (person.group == kInvalidId && registry.all_of<Relations>(entity)) {
            loners.emplace_back(id.value, entity);
        }
    }
    std::sort(loners.begin(), loners.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [founderId, founder] : loners) {
        if (registry.get<Person>(founder).group != kInvalidId ||
            leadershipOf(registry, founder) < kFoundLeadership) {
            continue;
        }
        const EntityId partnerId = pickPartner(world, founder, founderId);
        if (partnerId == kInvalidId) {
            continue;
        }
        const entt::entity partner = world.find(partnerId);
        const float founderAuthority =
            leadershipOf(registry, founder) +
            kAuthorityTrust * bondOf(registry, partner, founderId)->trust;
        const float partnerAuthority =
            leadershipOf(registry, partner) +
            kAuthorityTrust * bondOf(registry, founder, partnerId)->trust;
        const bool partnerLeads = partnerAuthority > founderAuthority;
        const EntityId leaderId = partnerLeads ? partnerId : founderId;
        const entt::entity leader = partnerLeads ? partner : founder;
        const entt::entity follower = partnerLeads ? founder : partner;
        const EntityId followerId = partnerLeads ? founderId : partnerId;

        const entt::entity entity = world.create();
        const EntityId groupId = registry.get<Id>(entity).value;
        Group& group = registry.emplace<Group>(entity);
        group.leader = leaderId;
        group.formed = world.tick();
        group.base = registry.get<Person>(leader).home != kInvalidId
                         ? registry.get<Person>(leader).home
                         : registry.get<Person>(follower).home;
        world.record(ChronicleKind::GroupFormed, leaderId, groupId);
        join(world, group, groupId, leader, leaderId);
        join(world, group, groupId, follower, followerId);
    }
}

// Хочет ли candidate в группу и примут ли его: друг вожака, который ему
// доверяет, или друг половины участников, если вожаку не враг; вожак и
// участники его не невзлюбили; живёт недалеко от базы.
bool admits(const World& world, const Group& group, entt::entity candidate, EntityId candidateId,
            std::pair<float, float> base) {
    const entt::registry& registry = world.registry();
    const Bond* toLeader = bondOf(registry, candidate, group.leader);
    if (toLeader != nullptr && (toLeader->hostile() || toLeader->trust < 0.0f)) {
        return false;
    }
    std::size_t friends = 0;
    for (const EntityId member : group.members) {
        const Bond* bond = bondOf(registry, candidate, member);
        friends += bond != nullptr && bond->friendly() ? 1 : 0;
        if (const Bond* back = bondOf(registry, world.find(member), candidateId);
            back != nullptr && back->hostile()) {
            return false;
        }
    }
    const bool wants = (toLeader != nullptr && toLeader->friendly() && toLeader->trust >= kJoinTrust) ||
                       (friends >= 1 && 2 * friends >= group.members.size());
    if (!wants) {
        return false;
    }
    const Bond* fromLeader = bondOf(registry, world.find(group.leader), candidateId);
    if (fromLeader != nullptr && fromLeader->affinity < 0.0f) {
        return false;
    }
    return distance(homeSpot(world, candidate), base) <= kGroupRange;
}

// В группы вступают друзья участников — по возрастанию id, пока есть места.
void admitFriends(World& world) {
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> groups;
    for (const auto [entity, id, group] : registry.view<const Id, const Group>().each()) {
        groups.emplace_back(id.value, entity);
    }
    std::sort(groups.begin(), groups.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<EntityId> candidates;
    for (const auto& [groupId, entity] : groups) {
        Group& group = registry.get<Group>(entity);
        if (group.members.size() >= kMaxGroupSize) {
            continue;
        }
        candidates.clear();
        for (const EntityId member : group.members) {
            const auto* relations = registry.try_get<Relations>(world.find(member));
            for (std::size_t i = 0; relations != nullptr && i < relations->count; ++i) {
                if (relations->bonds[i].friendly()) {
                    candidates.push_back(relations->bonds[i].other);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        std::pair<float, float> base;
        if (const entt::entity home = world.find(group.base); home != entt::null) {
            const Cell door = registry.get<Building>(home).door;
            base = {Map::cellCenter(door.x), Map::cellCenter(door.y)};
        } else {
            base = homeSpot(world, world.find(group.leader));
        }
        for (const EntityId candidateId : candidates) {
            if (group.members.size() >= kMaxGroupSize) {
                break;
            }
            const entt::entity candidate = personOf(world, candidateId);
            if (candidate == entt::null || registry.get<Person>(candidate).group != kInvalidId ||
                !registry.all_of<Relations>(candidate)) {
                continue;
            }
            if (admits(world, group, candidate, candidateId, base)) {
                join(world, group, groupId, candidate, candidateId);
            }
        }
    }
}

} // namespace

bool sameGroup(const entt::registry& registry, entt::entity a, entt::entity b) {
    const auto* first = registry.try_get<Person>(a);
    const auto* second = registry.try_get<Person>(b);
    return first != nullptr && second != nullptr && first->group != kInvalidId &&
           first->group == second->group;
}

float authorityOf(const World& world, const Group& group, EntityId member) {
    const entt::registry& registry = world.registry();
    const entt::entity entity = world.find(member);
    if (entity == entt::null) {
        return 0.0f;
    }
    float trust = 0.0f;
    int others = 0;
    for (const EntityId other : group.members) {
        if (other == member) {
            continue;
        }
        ++others;
        if (const Bond* bond = bondOf(registry, world.find(other), member)) {
            trust += bond->trust;
        }
    }
    return leadershipOf(registry, entity) +
           kAuthorityTrust * (others > 0 ? trust / static_cast<float>(others) : 0.0f);
}

void desertGroup(World& world, entt::entity deserter, int items) {
    entt::registry& registry = world.registry();
    Person& person = registry.get<Person>(deserter);
    const EntityId groupId = person.group;
    const entt::entity entity = world.find(groupId);
    if (entity == entt::null) {
        return;
    }
    Group& group = registry.get<Group>(entity);
    const EntityId id = registry.get<Id>(deserter).value;
    world.record(ChronicleKind::Deserted, id, groupId, items, 0, group.leader);
    person.group = kInvalidId;
    person.home = person.formerHome; // не было прежнего дома — без крова
    person.formerHome = kInvalidId;
    group.members.erase(std::remove(group.members.begin(), group.members.end(), id),
                        group.members.end());
    if (group.leader == id && group.members.size() >= 2) {
        group.leader = mostAuthoritative(world, group);
        world.record(ChronicleKind::LedGroup, group.leader, groupId, 2, 0, id);
    }
    // Остальные узнают: пропали запасы, а с ними и он.
    const std::vector<EntityId> members = group.members;
    for (const EntityId member : members) {
        if (const entt::entity mate = personOf(world, member); mate != entt::null) {
            experience(world, mate, MemoryKind::Betrayed, id, groupId, items);
        }
    }
}

void updateGroups(World& world) {
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> groups;
    for (const auto [entity, id, group] : registry.view<const Id, const Group>().each()) {
        groups.emplace_back(id.value, entity);
    }
    std::sort(groups.begin(), groups.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [groupId, entity] : groups) {
        if (!reviewGroup(world, registry.get<Group>(entity), groupId)) {
            world.destroy(entity);
        }
    }
    foundGroups(world);
    admitFriends(world);
}

void tickGroups(World& world) {
    if (world.tick() % kTicksPerGameHour == 0) {
        updateGroups(world);
    }
}

} // namespace lw
