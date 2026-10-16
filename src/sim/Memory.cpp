#include "sim/Memory.h"

#include "sim/Perception.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lw {

namespace {

// Воспоминание угасает: за столько часов сила уменьшается примерно в e раз.
// Самое сильное (1) забывается примерно через 70 суток — больше года мира,
// среднее (0,5) — через 50.
constexpr float kMemoryHours = 30.0f * 24.0f;

// Долг и обида от поступков, до черт характера. Благодарность — множитель
// 0,7 + 0,6 × альтруизм, злопамятность — 0,7 + 0,6 × агрессия.
constexpr float kSavedDebt = 0.6f;
constexpr float kDefendedDebt = 0.2f;
constexpr float kAbandonedGrudge = 0.2f;      // + kAbandonedCloseGrudge × близость
constexpr float kAbandonedCloseGrudge = 0.4f;
constexpr float kRobbedGrudge = 0.15f;        // + kRobbedItemGrudge за вещь
constexpr float kRobbedItemGrudge = 0.05f;
constexpr float kRobbedMaxGrudge = 0.5f;
// Кого запомнил по поступку, того знает хотя бы так.
constexpr float kMetInEvent = 0.2f;
// Горе — близость погибшего: столько к одиночеству и к страху. Близкий встал
// зомби после того, как нашли его тело, — горе вполовину.
constexpr float kGriefLoneliness = 0.5f;
constexpr float kGriefFear = 0.3f;

// Свидетель нападения — не дальше этого от жертвы, метры.
constexpr float kWitnessRange = 30.0f;
// Спас — если зомби был не дальше этого от жертвы, метры.
constexpr float kRescueRange = 30.0f;

float distance(float ax, float ay, float bx, float by) {
    return std::hypot(ax - bx, ay - by);
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

// Последствия пережитого: сила воспоминания, долг, обида, горе, запись в летописи.
struct Impression {
    float strength = 0.0f;
    float debt = 0.0f;
    float grudge = 0.0f;
    float grief = 0.0f;
    ChronicleKind chronicle = ChronicleKind::SavedBy;
};

// close — насколько близок тот, о ком воспоминание; mourned — его тело уже
// находили; items — сколько вещей взял вор.
Impression impressionOf(MemoryKind kind, const Traits& traits, float close, bool mourned,
                        int items) {
    const float gratitude = 0.7f + 0.6f * traits.altruism;
    const float vindictiveness = 0.7f + 0.6f * traits.aggression;
    Impression impression;
    switch (kind) {
    case MemoryKind::Saved:
        impression = {1.0f, kSavedDebt * gratitude, 0.0f, 0.0f, ChronicleKind::SavedBy};
        break;
    case MemoryKind::Defended:
        impression = {0.5f, kDefendedDebt * gratitude, 0.0f, 0.0f, ChronicleKind::DefendedBy};
        break;
    case MemoryKind::Abandoned:
        impression = {0.5f + 0.5f * close, 0.0f,
                      (kAbandonedGrudge + kAbandonedCloseGrudge * close) * vindictiveness, 0.0f,
                      ChronicleKind::AbandonedBy};
        break;
    case MemoryKind::Robbed:
        impression = {0.6f, 0.0f,
                      std::min(kRobbedMaxGrudge,
                               kRobbedGrudge + kRobbedItemGrudge * static_cast<float>(items)) *
                          vindictiveness,
                      0.0f, ChronicleKind::CaughtThief};
        break;
    case MemoryKind::Bereaved:
        impression = {0.5f + 0.5f * close, 0.0f, 0.0f, close, ChronicleKind::Mourned};
        break;
    case MemoryKind::Turned:
        impression = {0.5f + 0.5f * close, 0.0f, 0.0f, mourned ? 0.5f * close : close,
                      ChronicleKind::SawTurned};
        break;
    case MemoryKind::Count:
        break;
    }
    return impression;
}

const Traits& traitsOf(const entt::registry& registry, entt::entity entity) {
    static const Traits kAverage{};
    const Traits* traits = entity != entt::null ? registry.try_get<Traits>(entity) : nullptr;
    return traits != nullptr ? *traits : kAverage;
}

} // namespace

bool remember(Memories& memories, MemoryKind kind, EntityId who, EntityId about, float strength,
              std::uint64_t tick) {
    if (Memory* known = memories.find(kind, who)) {
        const bool repeat = tick < known->tick + kEpisodeTicks;
        // Повтор освежает память; новый случай с тем же человеком — крепче.
        known->strength = repeat ? std::max(known->strength, strength)
                                 : std::min(1.0f, std::max(known->strength, strength) + 0.5f * strength);
        known->tick = tick;
        known->about = about;
        return !repeat;
    }
    std::size_t slot = memories.count;
    if (memories.count == Memories::kMaxMemories) {
        // Самое слабое, при равных — первое.
        slot = 0;
        for (std::size_t i = 1; i < memories.count; ++i) {
            if (memories.items[i].strength < memories.items[slot].strength) {
                slot = i;
            }
        }
        if (memories.items[slot].strength >= strength) {
            return false; // всё, что помнит, важнее
        }
    } else {
        ++memories.count;
    }
    memories.items[slot] = {kind, who, about, tick, strength};
    return true;
}

void fadeMemories(Memories& memories, float minutes) {
    const float share = std::min(1.0f, minutes / 60.0f / kMemoryHours);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < memories.count; ++i) {
        Memory memory = memories.items[i];
        memory.strength -= memory.strength * share;
        if (memory.strength >= kForgotten) {
            memories.items[kept++] = memory;
        }
    }
    for (std::size_t i = kept; i < memories.count; ++i) {
        memories.items[i] = {};
    }
    memories.count = static_cast<std::uint8_t>(kept);
}

bool experience(World& world, entt::entity self, MemoryKind kind, EntityId who, EntityId about,
                std::int32_t a) {
    entt::registry& registry = world.registry();
    auto* memories = registry.try_get<Memories>(self);
    if (memories == nullptr) {
        return false;
    }
    auto* relations = registry.try_get<Relations>(self);
    Bond* bond = relations != nullptr ? relations->find(who) : nullptr;
    const float close = bond != nullptr ? closeness(*bond) : 0.0f;
    const bool mourned = memories->find(MemoryKind::Bereaved, who) != nullptr;
    const Traits& traits = traitsOf(registry, self);
    const Impression impression = impressionOf(kind, traits, close, mourned, a);
    const std::uint64_t tick = world.tick();
    if (!remember(*memories, kind, who, about, impression.strength, tick)) {
        return false;
    }
    const EntityId id = registry.get<Id>(self).value;
    world.record(impression.chronicle, id, who, a);

    if ((impression.debt > 0.0f || impression.grudge > 0.0f) && relations != nullptr) {
        if (bond == nullptr) {
            const entt::entity other = world.find(who);
            bond = acquireBond(*relations, who,
                               chemistry(id, traits, who, traitsOf(registry, other)), tick);
        }
        if (bond != nullptr) {
            bond->familiarity = std::max(bond->familiarity, kMetInEvent);
            bond->seen = tick;
            addDebt(*bond, impression.debt);
            addGrudge(*bond, impression.grudge);
            switch (updateBondFlags(*bond)) {
            case Bond::kFriend:
                world.record(ChronicleKind::Befriended, id, who);
                break;
            case Bond::kFoe:
                world.record(ChronicleKind::Disliked, id, who);
                break;
            default:
                break;
            }
        }
    }
    if (impression.grief > 0.0f) {
        if (auto* needs = registry.try_get<Needs>(self)) {
            needs->loneliness = clamp01(needs->loneliness + kGriefLoneliness * impression.grief);
            needs->fear = clamp01(needs->fear + kGriefFear * impression.grief);
        }
    }
    return true;
}

void witnessRescue(World& world, EntityId rescuer, entt::entity zombie) {
    entt::registry& registry = world.registry();
    const auto* attacker = registry.try_get<Zombie>(zombie);
    const auto* at = registry.try_get<Position>(zombie);
    if (attacker == nullptr || at == nullptr) {
        return;
    }
    const EntityId zombieId = registry.get<Id>(zombie).value;
    EntityId victims[2] = {attacker->state == ZombieState::Chase ? attacker->target : kInvalidId,
                           kInvalidId};
    if (const auto* combat = registry.try_get<Combat>(zombie)) {
        victims[1] = combat->target != victims[0] ? combat->target : kInvalidId;
    }
    for (const EntityId victim : victims) {
        if (victim == kInvalidId || victim == rescuer) {
            continue;
        }
        const entt::entity entity = world.find(victim);
        if (entity == entt::null || !registry.all_of<Person>(entity)) {
            continue;
        }
        const auto* needs = registry.try_get<Needs>(entity);
        const Position& position = registry.get<Position>(entity);
        if (needs != nullptr && needs->health > 0.0f &&
            distance(position.x, position.y, at->x, at->y) <= kRescueRange) {
            experience(world, entity, MemoryKind::Saved, rescuer, zombieId);
        }
    }
}

void witnessAttack(World& world, entt::entity victim, EntityId zombie) {
    entt::registry& registry = world.registry();
    const auto* awareness = registry.try_get<Awareness>(victim);
    const auto* relations = registry.try_get<Relations>(victim);
    if (awareness == nullptr) {
        return;
    }
    const EntityId victimId = registry.get<Id>(victim).value;
    const Position& position = registry.get<Position>(victim);
    const std::uint64_t tick = world.tick();
    // Сначала — кого и за что; потом переживать: experience меняет память жертвы.
    std::vector<std::pair<MemoryKind, EntityId>> seen;
    for (std::size_t i = 0; i < awareness->count; ++i) {
        const Contact& contact = awareness->contacts[i];
        if (!contact.seen || !isFresh(contact, tick) ||
            distance(position.x, position.y, contact.x, contact.y) > kWitnessRange) {
            continue;
        }
        const entt::entity other = world.find(contact.id);
        const auto* mind = other != entt::null ? registry.try_get<Mind>(other) : nullptr;
        if (mind == nullptr || !registry.all_of<Person>(other)) {
            continue;
        }
        if (mind->action == Action::Flee) {
            const Bond* bond = relations != nullptr ? relations->find(contact.id) : nullptr;
            if (bond != nullptr && closeness(*bond) >= kCloseEnough) {
                seen.emplace_back(MemoryKind::Abandoned, contact.id);
            }
        } else if (mind->action == Action::Fight) {
            // Дерётся с одним из тех, кто нападает на жертву.
            const entt::entity foe = world.find(registry.get<Combat>(other).target);
            const auto* foeZombie = foe != entt::null ? registry.try_get<Zombie>(foe) : nullptr;
            const auto* foeCombat = foe != entt::null ? registry.try_get<Combat>(foe) : nullptr;
            if (foeZombie != nullptr &&
                (foeZombie->target == victimId ||
                 (foeCombat != nullptr && foeCombat->target == victimId))) {
                seen.emplace_back(MemoryKind::Defended, contact.id);
            }
        }
    }
    for (const auto& [kind, who] : seen) {
        experience(world, victim, kind, who, zombie);
    }
}

void witnessTheft(World& world, entt::entity thief, EntityId building, int items) {
    if (items <= 0) {
        return;
    }
    entt::registry& registry = world.registry();
    const EntityId thiefId = registry.get<Id>(thief).value;
    const std::uint64_t tick = world.tick();
    // Порядок — порядок хранения в реестре: он детерминирован.
    std::vector<entt::entity> owners;
    for (const auto [entity, person, memories] :
         registry.view<const Person, const Memories>().each()) {
        if (person.home != building || entity == thief || person.asleep) {
            continue;
        }
        bool noticed = person.indoors; // дома: вор пришёл к нему
        if (const auto* awareness = registry.try_get<Awareness>(entity); !noticed && awareness) {
            const Contact* contact = awareness->find(thiefId);
            noticed = contact != nullptr && contact->seen && isFresh(*contact, tick);
        }
        if (noticed) {
            owners.push_back(entity);
        }
    }
    for (const entt::entity owner : owners) {
        experience(world, owner, MemoryKind::Robbed, thiefId, building, items);
    }
}

} // namespace lw
