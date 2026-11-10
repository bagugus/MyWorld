#include "sim/systems/SocietySystem.h"

#include "sim/Calendar.h"
#include "sim/Family.h"
#include "sim/Infection.h"
#include "sim/Names.h"
#include "sim/Perception.h"
#include "sim/Scenario.h"
#include "sim/Society.h"
#include "sim/Traits.h"
#include "sim/Work.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Больного в жару и укушенного видно вблизи; тело — как в NeedsSystem.
constexpr float kIllnessSight = 12.0f;
constexpr float kBodySight = 15.0f;
// Передачу слышат дома, на работе, в магазине и больнице: за минуту — с таким
// шансом (за час — почти все).
constexpr float kBroadcastChance = 0.03f;
// Ждущие колонну — у двери пункта не дальше этого, метры; дети родителя — тоже.
constexpr float kWaitRadius = 30.0f;
// Старики садятся раньше остальных.
constexpr int kElderAge = 65;
// Армейский паёк в пункте эвакуации: солдатам и ждущим.
constexpr int kPointFood = 60;
constexpr int kPointWater = 60;
constexpr int kPointBandages = 10;
// Солдат: возраст, патроны.
constexpr int kSoldierAgeMin = 19;
constexpr int kSoldierAgeMax = 40;
constexpr float kSoldierFemale = 0.15f;
constexpr int kSoldierAmmoMin = 20;
constexpr int kSoldierAmmoMax = 40;
constexpr int kSoldierShiftStart = 6 * 60;
constexpr int kSoldierShiftEnd = 22 * 60;
constexpr int kNameAttempts = 50;
// С уходом армии остаётся солдат с дисциплиной ниже этого — или тот, у кого
// здесь появился друг.
constexpr float kStayDiscipline = 0.65f;

float metersBetween(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

std::vector<std::pair<EntityId, entt::entity>> peopleById(const entt::registry& registry) {
    std::vector<std::pair<EntityId, entt::entity>> people;
    for (const auto [entity, id, person, alarm] : registry.view<const Id, const Person, const Alarm>().each()) {
        people.emplace_back(id.value, entity);
    }
    std::sort(people.begin(), people.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return people;
}

// Дома, на работе, в магазине, в больнице — там радио и телевизор.
bool nearRadio(const entt::registry& registry, entt::entity entity) {
    const Person& person = registry.get<Person>(entity);
    if (person.indoors) {
        return true;
    }
    const auto* mind = registry.try_get<Mind>(entity);
    return mind != nullptr && mind->phase == ActionPhase::Perform &&
           (mind->action == Action::Work || mind->action == Action::Buy || mind->action == Action::SeeDoctor ||
            mind->action == Action::Evacuate);
}

void hearBroadcast(World& world, Rng& rng) {
    const Scenario* scenario = scenarioOf(world);
    if (scenario == nullptr || scenario->broadcast == 0) {
        return;
    }
    const float level = broadcastLevel(static_cast<BroadcastKind>(scenario->broadcast));
    const EntityId source = scenarioIdOf(world);
    const entt::registry& registry = world.registry();
    for (const auto& [id, entity] : peopleById(registry)) {
        if (registry.get<Person>(entity).asleep || alarmOf(registry, entity) >= level - 1e-4f ||
            !nearRadio(registry, entity)) {
            continue;
        }
        if (rng.chance(kBroadcastChance)) {
            raiseAlarm(world, entity, level, AlarmClue::Broadcast, source);
        }
    }
}

// Что человек видит своими глазами — самое страшное из увиденного.
void noticeEvidence(World& world) {
    const entt::registry& registry = world.registry();
    const std::uint64_t tick = world.tick();
    for (const auto& [id, entity] : peopleById(registry)) {
        float level = 0.0f;
        AlarmClue clue = AlarmClue::None;
        EntityId source = kInvalidId;
        const auto consider = [&](AlarmClue seen, EntityId who) {
            if (clueLevel(seen) > level) {
                level = clueLevel(seen);
                clue = seen;
                source = who;
            }
        };
        if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && isInfected(*wounds)) {
            if (wounds->bites > 0) {
                consider(AlarmClue::Bitten, wounds->hurtBy);
            } else if (isFeverish(*wounds)) {
                consider(AlarmClue::Fever, kInvalidId);
            }
        }
        const Person& person = registry.get<Person>(entity);
        if (!person.asleep && !person.indoors) {
            const Position& self = registry.get<Position>(entity);
            if (const auto* awareness = registry.try_get<Awareness>(entity)) {
                for (std::size_t i = 0; i < awareness->count; ++i) {
                    const Contact& contact = awareness->contacts[i];
                    if (!contact.seen || !isFresh(contact, *awareness, tick)) {
                        continue;
                    }
                    const entt::entity other = world.find(contact.id);
                    if (other == entt::null) {
                        continue;
                    }
                    if (registry.all_of<Zombie>(other)) {
                        consider(AlarmClue::Zombie, contact.id);
                        continue;
                    }
                    const auto* wounds = registry.try_get<Wounds>(other);
                    if (wounds == nullptr || !isVisiblyIll(*wounds) ||
                        metersBetween(self.x, self.y, contact.x, contact.y) > kIllnessSight) {
                        continue;
                    }
                    consider(wounds->bites > 0 ? AlarmClue::Bitten : AlarmClue::Sick, contact.id);
                }
            }
            if (level < clueLevel(AlarmClue::Body)) {
                // Тело умершего от заразы (или того, кто ещё встанет); из
                // нескольких — с меньшим id.
                EntityId body = kInvalidId;
                world.spatial().forEachInRadius(self.x, self.y, kBodySight, [&](const SpatialHit& hit) {
                    const auto* corpse = registry.try_get<Corpse>(hit.entity);
                    if (corpse != nullptr && corpse->kind == CorpseKind::Human &&
                        (corpse->turnsAt != 0 || corpse->cause == DeathCause::Infection) &&
                        (body == kInvalidId || hit.id < body)) {
                        body = hit.id;
                    }
                });
                if (body != kInvalidId) {
                    consider(AlarmClue::Body, body);
                }
            }
        }
        if (clue != AlarmClue::None) {
            raiseAlarm(world, entity, level, clue, source);
        }
    }
}

void liftQuarantine(World& world, entt::entity settlement, bool authorityGone) {
    world.record(ChronicleKind::QuarantineLifted, world.registry().get<Id>(settlement).value, kInvalidId,
                 authorityGone ? 1 : 0);
    world.registry().remove<Quarantine>(settlement);
}

void reviewQuarantines(World& world) {
    entt::registry& registry = world.registry();
    const bool authority = authorityPresent(world);
    const std::uint64_t tick = world.tick();
    const EntityId scenario = scenarioIdOf(world);
    for (const Places::SettlementPlace& place : world.places().settlements()) {
        const entt::entity entity = world.find(place.id);
        if (entity == entt::null) {
            continue;
        }
        auto* quarantine = registry.try_get<Quarantine>(entity);
        if (quarantine == nullptr) {
            if (!authority) {
                continue;
            }
            const int cases = quarantineCases(world, place.id);
            if (cases >= kQuarantineCases) {
                registry.emplace<Quarantine>(entity, Quarantine{tick, cases, Quarantine::kNever});
                world.record(ChronicleKind::QuarantineDeclared, place.id, scenario, cases);
                broadcast(world, BroadcastKind::Epidemic, place.id);
            }
            continue;
        }
        if (!authority) {
            liftQuarantine(world, entity, true);
            continue;
        }
        quarantine->cases = quarantineCases(world, place.id);
        if (quarantine->cases > 0) {
            quarantine->clearSince = Quarantine::kNever;
        } else if (quarantine->clearSince == Quarantine::kNever) {
            quarantine->clearSince = tick;
        } else if (tick >= quarantine->clearSince + kQuarantineClearTicks) {
            liftQuarantine(world, entity, false);
        }
    }
}

// Здание под пункт эвакуации в городе: школа, больница, участок, многоэтажка —
// что есть; при равных — меньший id. kInvalidId — зданий нет.
EntityId pickEvacuationSite(const Places::SettlementPlace& place) {
    constexpr BuildingKind kOrder[] = {BuildingKind::School, BuildingKind::Hospital, BuildingKind::PoliceStation,
                                       BuildingKind::Apartment, BuildingKind::Shop, BuildingKind::House};
    for (const BuildingKind kind : kOrder) {
        for (const Places::BuildingPlace& building : place.buildings) {
            if (building.kind == kind) {
                return building.id;
            }
        }
    }
    return kInvalidId;
}


void openEvacuation(World& world) {
    if (evacuationOpen(world)) {
        return;
    }
    entt::registry& registry = world.registry();
    Rng& rng = world.rng(RngStream::Society);
    std::unordered_set<std::string> taken = takenNames(world);
    bool opened = false;
    for (const Places::SettlementPlace& place : world.places().settlements()) {
        const entt::entity settlement = world.find(place.id);
        if (settlement == entt::null || registry.get<Settlement>(settlement).kind != SettlementKind::Town) {
            continue;
        }
        const EntityId site = pickEvacuationSite(place);
        const entt::entity building = world.find(site);
        if (building == entt::null) {
            continue;
        }
        registry.emplace_or_replace<EvacuationPoint>(building, EvacuationPoint{place.id, world.tick(), 0, 0});
        if (!registry.all_of<Workplace>(building)) {
            registry.emplace<Workplace>(building);
        }
        Inventory& stock = registry.get_or_emplace<Inventory>(building);
        stock.add(ItemKind::Food, kPointFood);
        stock.add(ItemKind::Water, kPointWater);
        stock.add(ItemKind::Bandage, kPointBandages);
        const Cell door = registry.get<Building>(building).door;
        for (int i = 0; i < kSoldiersPerPoint; ++i) {
            spawnSoldier(world, site, door, rng, taken);
        }
        world.record(ChronicleKind::EvacuationOpened, site, place.id, kSoldiersPerPoint);
        opened = true;
    }
    if (opened) {
        broadcast(world, BroadcastKind::Evacuation);
        world.syncSpatial();
    }
}

// Есть ли у солдата друг среди местных (не солдат).
bool hasLocalFriend(const World& world, entt::entity soldier) {
    const entt::registry& registry = world.registry();
    const auto* relations = registry.try_get<Relations>(soldier);
    for (std::size_t i = 0; relations != nullptr && i < relations->count; ++i) {
        const Bond& bond = relations->bonds[i];
        const entt::entity other = world.find(bond.other);
        if (bond.friendly() && other != entt::null && registry.all_of<Person>(other) &&
            !isSoldier(registry, other)) {
            return true;
        }
    }
    return false;
}

void closeEvacuation(World& world) {
    entt::registry& registry = world.registry();
    std::vector<std::pair<EntityId, entt::entity>> points;
    for (const auto [entity, id, point] : registry.view<const Id, const EvacuationPoint>().each()) {
        points.emplace_back(id.value, entity);
    }
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [pointId, entity] : points) {
        departConvoy(world, pointId, true);
        std::vector<std::pair<EntityId, entt::entity>> soldiers;
        for (const auto [soldier, id, person, job] : registry.view<const Id, const Person, const Job>().each()) {
            if (job.profession == Profession::Soldier && job.workplace == pointId) {
                soldiers.emplace_back(id.value, soldier);
            }
        }
        std::sort(soldiers.begin(), soldiers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        static const Traits kAverage{};
        for (const auto& [id, soldier] : soldiers) {
            const auto* traits = registry.try_get<Traits>(soldier);
            const bool stays = soldierDiscipline(traits != nullptr ? *traits : kAverage) < kStayDiscipline ||
                               hasLocalFriend(world, soldier);
            if (stays) {
                world.record(ChronicleKind::StayedBehind, id, pointId);
            } else {
                evacuate(world, soldier, pointId, true);
            }
        }
        world.record(ChronicleKind::EvacuationEnded, pointId, kInvalidId,
                     registry.get<EvacuationPoint>(entity).evacuated);
        registry.remove<EvacuationPoint>(entity);
    }
}

} // namespace

std::unordered_set<std::string> takenNames(const World& world) {
    std::unordered_set<std::string> taken;
    for (const auto [entity, name] : world.registry().view<const Name>().each()) {
        taken.insert(name.first + " " + name.last);
    }
    return taken;
}

entt::entity spawnSoldier(World& world, EntityId point, Cell door, Rng& rng,
                          std::unordered_set<std::string>& taken) {
    entt::registry& registry = world.registry();
    const bool female = rng.chance(kSoldierFemale);
    Name name;
    for (int attempt = 0; attempt < kNameAttempts; ++attempt) {
        name.first = randomFirstName(rng, female);
        name.last = randomLastName(rng);
        if (!taken.contains(name.first + " " + name.last)) {
            break;
        }
    }
    taken.insert(name.first + " " + name.last);
    // Солдат смелее и решительнее среднего жителя.
    Traits traits = generateTraits(rng);
    traits.bravery = 0.5f + 0.45f * traits.bravery;
    traits.aggression = 0.3f + 0.5f * traits.aggression;
    traits.prudence = 0.4f + 0.4f * traits.prudence;
    traits.chronotype = 0.3f + 0.4f * traits.chronotype;
    Walker walker;
    walker.speed = rng.range(1.3f, 1.7f);
    Needs needs;
    needs.hunger = rng.range(0.0f, 0.3f);
    needs.thirst = rng.range(0.0f, 0.3f);
    needs.fatigue = rng.range(0.1f, 0.4f);
    Mind mind;
    mind.spot = door;
    mind.nextThink = world.tick() + rng.below(static_cast<std::uint32_t>(10 * kTicksPerGameMinute));
    mind.until = mind.nextThink;
    Person person;
    person.home = point;
    Job job;
    job.profession = Profession::Soldier;
    job.workplace = point;
    job.start = static_cast<std::uint16_t>(kSoldierShiftStart);
    job.end = static_cast<std::uint16_t>(kSoldierShiftEnd);
    job.days = 0b1111111;
    const int ammo = rng.rangeInt(kSoldierAmmoMin, kSoldierAmmoMax);
    const int age = rng.rangeInt(kSoldierAgeMin, kSoldierAgeMax);

    const entt::entity entity = world.create();
    const EntityId id = registry.get<Id>(entity).value;
    registry.emplace<Position>(entity, Map::cellCenter(door.x), Map::cellCenter(door.y));
    registry.emplace<Velocity>(entity);
    registry.emplace<Person>(entity, person);
    registry.emplace<Name>(entity, std::move(name));
    registry.emplace<Age>(entity, Age{static_cast<std::uint8_t>(age)});
    registry.emplace<Kin>(entity);
    registry.emplace<Walker>(entity, std::move(walker));
    registry.emplace<Needs>(entity, needs);
    registry.emplace<Traits>(entity, traits);
    registry.emplace<Mind>(entity, mind);
    Inventory inventory;
    inventory.set(ItemKind::Food, 3);
    inventory.set(ItemKind::Water, 3);
    inventory.set(ItemKind::Bandage, 2);
    inventory.set(ItemKind::Medicine, 1);
    inventory.set(ItemKind::RifleAmmo, ammo);
    registry.emplace<Inventory>(entity, inventory);
    registry.emplace<Senses>(entity);
    registry.emplace<Awareness>(entity);
    registry.emplace<Weapon>(entity, Weapon{WeaponKind::Rifle, static_cast<std::uint16_t>(ammo)});
    registry.emplace<Combat>(entity);
    registry.emplace<Wounds>(entity);
    registry.emplace<Relations>(entity);
    registry.emplace<Memories>(entity);
    registry.emplace<Leads>(entity);
    registry.emplace<Job>(entity, job);
    // Солдат знает, зачем его прислали.
    registry.emplace<Alarm>(entity, Alarm{broadcastLevel(BroadcastKind::Evacuation), AlarmClue::Broadcast,
                                          scenarioIdOf(world), world.tick(), 0});
    world.record(ChronicleKind::Settled, id, point);
    return entity;
}

int departConvoy(World& world, EntityId pointId, bool /*last*/) {
    entt::registry& registry = world.registry();
    const entt::entity building = world.find(pointId);
    if (building == entt::null || !registry.all_of<EvacuationPoint>(building)) {
        return 0;
    }
    const Cell door = registry.get<Building>(building).door;
    const float dx = Map::cellCenter(door.x);
    const float dy = Map::cellCenter(door.y);
    const auto nearDoor = [&](entt::entity entity) {
        const Position& at = registry.get<Position>(entity);
        return metersBetween(at.x, at.y, dx, dy) <= kWaitRadius;
    };

    // Кто охраняет посадку: солдат этого пункта на посту (меньший id).
    EntityId guard = kInvalidId;
    std::vector<std::pair<EntityId, entt::entity>> waiting;
    for (const auto [entity, id, person, mind] : registry.view<const Id, const Person, const Mind>().each()) {
        const auto* job = registry.try_get<Job>(entity);
        if (job != nullptr && job->profession == Profession::Soldier && job->workplace == pointId &&
            isOnDuty(world, entity) && (guard == kInvalidId || id.value < guard)) {
            guard = id.value;
        }
        if (mind.action == Action::Evacuate && mind.target == pointId && mind.phase == ActionPhase::Perform &&
            nearDoor(entity)) {
            waiting.emplace_back(id.value, entity);
        }
    }
    if (waiting.empty()) {
        return 0;
    }
    std::sort(waiting.begin(), waiting.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto waits = [&](EntityId id) {
        return std::any_of(waiting.begin(), waiting.end(), [id](const auto& w) { return w.first == id; });
    };

    // Семьи: взрослый и его дети (подопечные). Ребёнок едет только с опекуном.
    struct Party {
        int priority = 2;
        EntityId lead = kInvalidId;
        std::vector<entt::entity> members;
    };
    std::vector<Party> parties;
    int refused = 0;
    for (const auto& [id, entity] : waiting) {
        const Kin* kin = registry.try_get<Kin>(entity);
        const entt::entity guardian = kin != nullptr ? world.find(kin->guardian) : entt::entity{entt::null};
        if (isMinor(registry, entity) && guardian != entt::null && registry.all_of<Person>(guardian)) {
            continue; // садится с опекуном
        }
        Party party;
        party.lead = id;
        party.members.push_back(entity);
        bool complete = true;
        bool ill = false;
        // Подопечные — все живые дети, чей опекун он.
        for (const auto [child, childId, person, childKin] : registry.view<const Id, const Person, const Kin>().each()) {
            if (childKin.guardian != id || !isMinor(registry, child)) {
                continue;
            }
            if (!waits(childId.value)) {
                complete = false; // без своих детей не уезжает
                break;
            }
            party.members.push_back(child);
        }
        if (!complete) {
            continue;
        }
        for (const entt::entity member : party.members) {
            const auto* wounds = registry.try_get<Wounds>(member);
            if (guard != kInvalidId && wounds != nullptr && isVisiblyIll(*wounds)) {
                ill = true;
                world.record(ChronicleKind::EvacuationRefused, registry.get<Id>(member).value, pointId, 0, 0, guard);
                if (auto* alarm = registry.try_get<Alarm>(member)) {
                    alarm->refusedAt = world.tick();
                }
                ++refused;
            }
        }
        if (ill) {
            // Больного не пустили — семья остаётся с ним.
            for (const entt::entity member : party.members) {
                if (auto* alarm = registry.try_get<Alarm>(member)) {
                    alarm->refusedAt = world.tick();
                }
            }
            continue;
        }
        bool children = party.members.size() > 1 || isMinor(registry, entity);
        party.priority = children ? 0 : ageOf(registry, entity) >= kElderAge ? 1 : 2;
        parties.push_back(std::move(party));
    }
    std::stable_sort(parties.begin(), parties.end(), [](const Party& a, const Party& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.lead < b.lead;
    });
    int seats = kConvoySeats;
    int boarded = 0;
    for (const Party& party : parties) {
        const int size = static_cast<int>(party.members.size());
        if (size > seats) {
            continue;
        }
        seats -= size;
        for (const entt::entity member : party.members) {
            evacuate(world, member, pointId, false);
            ++boarded;
        }
    }
    auto& point = registry.get<EvacuationPoint>(building);
    point.convoys += boarded > 0 ? 1 : 0;
    const int left = static_cast<int>(waiting.size()) - boarded - refused;
    world.record(ChronicleKind::ConvoyLeft, pointId, kInvalidId, boarded, std::max(0, left));
    return boarded;
}

void onPhaseBegan(World& world, WorldPhase phase) {
    if (phase == WorldPhase::Panic) {
        // Власть признала беду: если об эпидемии ещё не сообщали — сейчас.
        broadcast(world, BroadcastKind::Epidemic);
        openEvacuation(world);
        return;
    }
    if (phase == WorldPhase::Collapse) {
        closeEvacuation(world);
        entt::registry& registry = world.registry();
        std::vector<std::pair<EntityId, entt::entity>> quarantined;
        for (const auto [entity, id, quarantine] : registry.view<const Id, const Quarantine>().each()) {
            quarantined.emplace_back(id.value, entity);
        }
        std::sort(quarantined.begin(), quarantined.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [id, entity] : quarantined) {
            liftQuarantine(world, entity, true);
        }
        broadcast(world, BroadcastKind::Farewell);
    }
}

void updateSociety(World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return;
    }
    const Scenario* scenario = scenarioOf(world);
    if (scenario == nullptr || scenario->phase == WorldPhase::Peace) {
        return;
    }
    hearBroadcast(world, world.rng(RngStream::Society));
    noticeEvidence(world);
    const CalendarTime time = calendarAt(tick);
    if (time.minute != 0) {
        return;
    }
    if (evacuationOpen(world) &&
        std::find(std::begin(kConvoyHours), std::end(kConvoyHours), time.hour) != std::end(kConvoyHours)) {
        std::vector<EntityId> points;
        for (const auto [entity, id, point] : world.registry().view<const Id, const EvacuationPoint>().each()) {
            points.push_back(id.value);
        }
        std::sort(points.begin(), points.end());
        for (const EntityId point : points) {
            departConvoy(world, point);
        }
    }
    reviewQuarantines(world);
}

} // namespace lw
