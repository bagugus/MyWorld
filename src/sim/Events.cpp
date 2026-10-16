#include "sim/Events.h"

#include "sim/Rng.h"
#include "sim/World.h"

#include <cstring>
#include <iterator>

namespace lw {

namespace {

// Имя, звук, громкость и где его слышат люди, знак (минуты, метры), грузов.
constexpr EventSpec kSpecs[] = {
    // Падение припасов: самолёт кружит низко над местом — зомби идут на гул за
    // 800 м, люди слышат его за 1,5 км; дымовая шашка горит 6 игровых часов,
    // днём её видно за 2,5 км.
    {"drop", SoundKind::Aircraft, 800.0f, 1500.0f, 6 * 60, 2500.0f, 1, 2},
};
static_assert(std::size(kSpecs) == static_cast<std::size_t>(EventKind::Count));

} // namespace

const EventSpec& eventSpec(EventKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return kSpecs[index < std::size(kSpecs) ? index : 0];
}

std::optional<EventKind> eventKindByName(const char* name) {
    for (std::size_t i = 0; i < std::size(kSpecs); ++i) {
        if (std::strcmp(kSpecs[i].name, name) == 0) {
            return static_cast<EventKind>(i);
        }
    }
    return std::nullopt;
}

void fillCache(Inventory& inventory, EventKind kind, Rng& rng) {
    switch (kind) {
    case EventKind::SupplyDrop: {
        // Армейский ящик: паёк и вода на неделю для одного, аптечка, патроны
        // одного калибра, иногда ружьё. Ящик весит 20–35 кг — одному не унести.
        inventory.add(ItemKind::Food, rng.rangeInt(6, 12));
        inventory.add(ItemKind::Water, rng.rangeInt(5, 10));
        inventory.add(ItemKind::Bandage, rng.rangeInt(2, 5));
        inventory.add(ItemKind::Medicine, rng.rangeInt(1, 3));
        constexpr ItemKind kAmmo[] = {ItemKind::PistolAmmo, ItemKind::ShotgunShells,
                                      ItemKind::RifleAmmo};
        inventory.add(kAmmo[rng.below(3)], rng.rangeInt(10, 24));
        if (rng.chance(0.25f)) {
            inventory.add(rng.chance(0.5f) ? ItemKind::Rifle : ItemKind::Shotgun, 1);
        }
        break;
    }
    case EventKind::Count:
        break;
    }
}

bool learnOf(World& world, entt::entity person, EntityId event, Cell site, EventClue clue,
             EntityId source, std::uint64_t since) {
    entt::registry& registry = world.registry();
    auto* leads = registry.try_get<Leads>(person);
    if (leads == nullptr || leads->find(event) != nullptr) {
        return false;
    }
    std::size_t slot = leads->count;
    if (slot == Leads::kMaxLeads) {
        // Самая старая (при равных — первая).
        slot = 0;
        for (std::size_t i = 1; i < Leads::kMaxLeads; ++i) {
            if (leads->items[i].since < leads->items[slot].since) {
                slot = i;
            }
        }
    } else {
        ++leads->count;
    }
    leads->items[slot] = {event, site, source != kInvalidId ? since : world.tick(), source};
    world.record(ChronicleKind::LearnedOfEvent, registry.get<Id>(person).value, event,
                 static_cast<std::int32_t>(clue), 0, source);
    return true;
}

void markVisited(Leads& leads, EntityId event, bool empty) {
    for (std::size_t i = 0; i < leads.count; ++i) {
        if (leads.items[i].event == event) {
            leads.items[i].visited = true;
            leads.items[i].empty = empty;
        }
    }
}

bool hearEmpty(Leads& leads, EntityId event) {
    for (std::size_t i = 0; i < leads.count; ++i) {
        if (leads.items[i].event == event && !leads.items[i].empty) {
            leads.items[i].visited = true;
            leads.items[i].empty = true;
            return true;
        }
    }
    return false;
}

const Lead* leadToShare(const World& world, entt::entity teller, entt::entity listener) {
    const entt::registry& registry = world.registry();
    const auto* mine = registry.try_get<Leads>(teller);
    const auto* theirs = registry.try_get<Leads>(listener);
    if (mine == nullptr || theirs == nullptr || mine->count == 0) {
        return nullptr;
    }
    const EntityId listenerId = registry.get<Id>(listener).value;
    const Person* self = registry.try_get<Person>(teller);
    const Person* other = registry.try_get<Person>(listener);
    const bool mate = self != nullptr && other != nullptr && self->group != kInvalidId &&
                      self->group == other->group;
    const auto* relations = registry.try_get<Relations>(teller);
    const Bond* bond = relations != nullptr ? relations->find(listenerId) : nullptr;
    const bool foe = bond != nullptr && bond->hostile();
    // Там пусто — тому, кто туда ещё собирается.
    if (!foe) {
        for (std::size_t i = 0; i < mine->count; ++i) {
            const Lead& lead = mine->items[i];
            const Lead* known = theirs->find(lead.event);
            if (lead.empty && leadFresh(lead, world.tick()) && known != nullptr && !known->visited) {
                return &lead;
            }
        }
    }
    const bool dear = !foe && bond != nullptr && bond->affinity >= kShareLeadAffinity;
    const Traits* traits = registry.try_get<Traits>(teller);
    const bool generous = traits == nullptr || traits->greed < kKeepLeadGreed;
    if (!mate && !dear && !(generous && !foe)) {
        return nullptr;
    }
    // Самую свежую из тех, что тот не знает.
    const Lead* best = nullptr;
    for (std::size_t i = 0; i < mine->count; ++i) {
        const Lead& lead = mine->items[i];
        if (!leadFresh(lead, world.tick()) || lead.visited || lead.source != kInvalidId ||
            theirs->find(lead.event) != nullptr) {
            continue;
        }
        if (best == nullptr || lead.since > best->since) {
            best = &lead;
        }
    }
    return best;
}

} // namespace lw
