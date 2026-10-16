#include "sim/Memory.h"

#include "sim/Perception.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>
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
// Кого запомнил по поступку, того знает хотя бы так; кого только по слухам —
// так (по имени).
constexpr float kMetInEvent = 0.2f;
constexpr float kKnownByName = 0.05f;
// Горе — близость погибшего: столько к одиночеству и к страху. Близкий встал
// зомби после того, как нашли его тело, — горе вполовину.
constexpr float kGriefLoneliness = 0.5f;
constexpr float kGriefFear = 0.3f;

// Чужой поступок (этап 6.3). Сочувствие к незнакомцу — kBystanderConcern ×
// (0,5 + альтруизм); к знакомому — ещё его близость. Помнится чужой поступок
// с силой от kWitnessMemory (пострадал чужой) до полной (пострадал близкий).
constexpr float kBystanderConcern = 0.1f;
constexpr float kWitnessMemory = 0.5f;
// Ради меньших долга и обиды незнакомца не запоминают в лицо.
constexpr float kMinBondEffect = 0.05f;
// Сколько вещей взял вор, по слухам неизвестно: считается столько.
constexpr int kRumoredItems = 3;

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

bool isDeed(MemoryKind kind) {
    return kind != MemoryKind::Bereaved && kind != MemoryKind::Turned;
}

bool remember(Memories& memories, const Memory& memory) {
    if (Memory* known = memories.find(memory.kind, memory.who, memory.victim)) {
        const std::uint64_t gap =
            memory.tick > known->tick ? memory.tick - known->tick : known->tick - memory.tick;
        const bool repeat = gap < kEpisodeTicks;
        // Повтор освежает память; новый случай с тем же человеком — крепче.
        known->strength =
            repeat ? std::max(known->strength, memory.strength)
                   : std::min(1.0f, std::max(known->strength, memory.strength) + 0.5f * memory.strength);
        known->tick = std::max(known->tick, memory.tick);
        known->about = memory.about;
        // Увидел сам то, о чём слышал, — рассказчик больше ни при чём.
        if (memory.source == kInvalidId) {
            known->source = kInvalidId;
        }
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
        if (memories.items[slot].strength >= memory.strength) {
            return false; // всё, что помнит, важнее
        }
    } else {
        ++memories.count;
    }
    memories.items[slot] = memory;
    return true;
}

bool remember(Memories& memories, MemoryKind kind, EntityId who, EntityId about, float strength,
              std::uint64_t tick) {
    return remember(memories, Memory{kind, who, kInvalidId, about, kInvalidId, tick, strength});
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

float concernFor(const World& world, entt::entity self, EntityId victim) {
    const entt::registry& registry = world.registry();
    if (victim == registry.get<Id>(self).value) {
        return 1.0f;
    }
    float concern = kBystanderConcern * (0.5f + traitsOf(registry, self).altruism);
    if (const auto* relations = registry.try_get<Relations>(self)) {
        if (const Bond* bond = relations->find(victim)) {
            if (bond->hostile()) {
                return 0.0f; // недругу не сочувствуют
            }
            concern += closeness(*bond);
        }
    }
    return clamp01(concern);
}

bool experience(World& world, entt::entity self, MemoryKind kind, EntityId who, EntityId about,
                std::int32_t a, const Account& account) {
    entt::registry& registry = world.registry();
    auto* memories = registry.try_get<Memories>(self);
    if (memories == nullptr || who == kInvalidId) {
        return false;
    }
    const EntityId id = registry.get<Id>(self).value;
    if (who == id) {
        return false; // о своих поступках не вспоминают
    }
    auto* relations = registry.try_get<Relations>(self);
    Bond* bond = relations != nullptr ? relations->find(who) : nullptr;
    const bool deed = isDeed(kind);
    // Сделали с ним самим (или это весть о близком) — или с кем-то другим.
    const bool own = !deed || account.victim == kInvalidId;
    const bool heard = account.source != kInvalidId;
    // Близость к сделавшему важна, когда сделали с ним самим: бросил близкий.
    const float close = bond != nullptr && own ? closeness(*bond) : 0.0f;
    const bool mourned = memories->find(MemoryKind::Bereaved, who) != nullptr;
    const Traits& traits = traitsOf(registry, self);
    const Impression impression = impressionOf(kind, traits, close, mourned, a);
    // Чужой поступок весит столько, насколько небезразличен пострадавший;
    // слух — меньше и по доверию к рассказчику.
    const float concern = own ? 1.0f : concernFor(world, self, account.victim);
    const float belief = heard ? kHearsay * clamp01(account.credibility) : 1.0f;
    const float effect = concern * belief;
    const float strength =
        impression.strength * (kWitnessMemory + (1.0f - kWitnessMemory) * concern) * belief;
    const std::uint64_t tick = world.tick();
    const Memory memory{kind,          who,  deed ? account.victim : kInvalidId, about,
                        account.source, account.tick != 0 ? account.tick : tick, strength};
    if (strength < kForgotten || !remember(*memories, memory)) {
        return false;
    }

    if (!heard && own) {
        world.record(impression.chronicle, id, who, a);
    } else if (!heard) {
        // Видел, как обидели (или выручили) близкого.
        if (concern >= kCloseEnough) {
            world.record(ChronicleKind::Witnessed, id, who, static_cast<std::int32_t>(kind), 0,
                         account.victim);
        }
    } else if (!deed) {
        world.record(ChronicleKind::Mourned, id, who, 0, 0, account.source);
    } else if (own) {
        world.record(ChronicleKind::HeardOf, id, who, static_cast<std::int32_t>(kind), 0,
                     account.source);
    }

    const float debt = impression.debt * effect;
    const float grudge = impression.grudge * effect;
    if ((debt > 0.0f || grudge > 0.0f) && relations != nullptr) {
        if (bond == nullptr && debt + grudge >= kMinBondEffect) {
            const entt::entity other = world.find(who);
            bond = acquireBond(*relations, who,
                               chemistry(id, traits, who, traitsOf(registry, other)), tick);
        }
        if (bond != nullptr) {
            bond->familiarity = std::max(bond->familiarity, heard ? kKnownByName : kMetInEvent);
            if (!heard) {
                bond->seen = tick;
            }
            addDebt(*bond, debt);
            addGrudge(*bond, grudge);
            refreshBondFlags(world, id, *bond);
        }
    }
    if (impression.grief > 0.0f) {
        if (auto* needs = registry.try_get<Needs>(self)) {
            const float grief = impression.grief * belief;
            needs->loneliness = clamp01(needs->loneliness + kGriefLoneliness * grief);
            needs->fear = clamp01(needs->fear + kGriefFear * grief);
        }
    }
    return true;
}

void witnessDeed(World& world, MemoryKind kind, entt::entity doer, EntityId victim,
                 EntityId about, std::int32_t a) {
    entt::registry& registry = world.registry();
    const auto* at = registry.try_get<Position>(doer);
    if (at == nullptr || victim == kInvalidId) {
        return;
    }
    const EntityId doerId = registry.get<Id>(doer).value;
    const std::uint64_t tick = world.tick();
    std::vector<std::pair<EntityId, entt::entity>> witnesses;
    world.spatial().forEachInRadius(at->x, at->y, kWitnessSight, [&](const SpatialHit& hit) {
        if (hit.id == doerId || hit.id == victim) {
            return;
        }
        const auto* person = registry.try_get<Person>(hit.entity);
        const auto* awareness = registry.try_get<Awareness>(hit.entity);
        const auto* memories = registry.try_get<Memories>(hit.entity);
        if (person == nullptr || person->asleep || awareness == nullptr || memories == nullptr) {
            return;
        }
        const Contact* contact = awareness->find(doerId);
        if (contact == nullptr || !contact->seen || !isFresh(*contact, tick)) {
            return;
        }
        // Уже запомнил как сделанное с ним самим: хозяин дома.
        if (const Memory* own = memories->find(kind, doerId); own != nullptr && own->tick == tick) {
            return;
        }
        witnesses.emplace_back(hit.id, hit.entity);
    });
    // Порядок обхода индекса — не порядок мира: по id.
    std::sort(witnesses.begin(), witnesses.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    Account account;
    account.victim = victim;
    for (const auto& [id, entity] : witnesses) {
        experience(world, entity, kind, doerId, about, a, account);
    }
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
    const entt::entity hero = world.find(rescuer);
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
            // Кто видел со стороны — что он спас другого (этап 6.3).
            if (hero != entt::null) {
                witnessDeed(world, MemoryKind::Saved, hero, victim, zombieId);
            }
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
    // Хозяин для тех, кто видел со стороны, — живой жилец с меньшим id.
    EntityId victim = kInvalidId;
    for (const auto [entity, id, person] : registry.view<const Id, const Person>().each()) {
        if (person.home != building || entity == thief) {
            continue;
        }
        victim = victim == kInvalidId ? id.value : std::min(victim, id.value);
        if (person.asleep || !registry.all_of<Memories>(entity)) {
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
    if (victim != kInvalidId) {
        witnessDeed(world, MemoryKind::Robbed, thief, victim, building, items);
    }
}

const Memory* pickRumor(const World& world, entt::entity teller, entt::entity listener) {
    const entt::registry& registry = world.registry();
    const auto* told = registry.try_get<Memories>(teller);
    const auto* known = registry.try_get<Memories>(listener);
    if (told == nullptr || known == nullptr) {
        return nullptr;
    }
    const EntityId tellerId = registry.get<Id>(teller).value;
    const EntityId listenerId = registry.get<Id>(listener).value;
    const auto* relations = registry.try_get<Relations>(listener);
    const auto knows = [relations](EntityId other) {
        return relations != nullptr && relations->find(other) != nullptr;
    };
    const auto close = [relations](EntityId other) {
        const Bond* bond = relations != nullptr ? relations->find(other) : nullptr;
        return bond != nullptr && closeness(*bond) >= kCloseEnough;
    };
    const Memory* best = nullptr;
    float bestWeight = 0.0f;
    for (std::size_t i = 0; i < told->count; ++i) {
        const Memory& memory = told->items[i];
        if (memory.strength < kRetellStrength || memory.who == listenerId ||
            memory.source == listenerId) {
            continue;
        }
        float weight = memory.strength;
        if (isDeed(memory.kind)) {
            const EntityId victim = memory.victim == kInvalidId ? tellerId : memory.victim;
            // С самим слушателем — для него это «со мной».
            if (known->find(memory.kind, memory.who, victim == listenerId ? kInvalidId : victim)) {
                continue;
            }
            if (victim == listenerId) {
                weight *= 2.0f; // о том, что сделали с ним самим, расскажут первым
            } else if (!knows(memory.who) && !close(victim)) {
                continue; // о чужих чужому не рассказывают
            }
        } else if (!close(memory.who) || known->find(MemoryKind::Bereaved, memory.who) ||
                   known->find(MemoryKind::Turned, memory.who)) {
            continue;
        }
        if (best == nullptr || weight > bestWeight ||
            (weight == bestWeight &&
             std::tie(memory.kind, memory.who) < std::tie(best->kind, best->who))) {
            best = &memory;
            bestWeight = weight;
        }
    }
    return best;
}

void hearRumor(World& world, entt::entity listener, EntityId teller, const Memory& memory) {
    const entt::registry& registry = world.registry();
    const EntityId listenerId = registry.get<Id>(listener).value;
    const auto* relations = registry.try_get<Relations>(listener);
    const Bond* bond = relations != nullptr ? relations->find(teller) : nullptr;
    const float trust = bond != nullptr ? bond->trust : 0.0f;
    if ((bond != nullptr && bond->hostile()) || trust < 0.0f) {
        return; // недругу и тому, кто подвёл, не верят
    }
    Account account;
    account.source = teller;
    account.credibility = 0.5f + 0.5f * trust;
    account.tick = memory.tick;
    if (isDeed(memory.kind)) {
        const EntityId victim = memory.victim == kInvalidId ? teller : memory.victim;
        account.victim = victim == listenerId ? kInvalidId : victim;
    }
    experience(world, listener, memory.kind, memory.who, memory.about,
               memory.kind == MemoryKind::Robbed ? kRumoredItems : 0, account);
}

} // namespace lw
