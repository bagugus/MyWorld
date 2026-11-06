#include "sim/Foresight.h"

#include "sim/Components.h"
#include "sim/Death.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>
#include <utility>

namespace lw {

namespace {

// Игровой час — в секундах симуляции.
constexpr float kGameHourSeconds =
    static_cast<float>(kTicksPerGameHour) / static_cast<float>(kTicksPerSecond);
// Дальше этого (секунды симуляции; 6 игровых часов) — уже не «вот-вот».
constexpr float kHorizon = 1800.0f;
// Обращение предвидится с лихорадки (половина заражения) — до него не больше
// 15 игровых часов.
constexpr float kTurningFrom = 0.5f;
// Люди ближе этого к укушенному — под угрозой, когда он встанет.
constexpr float kTurningCrowd = 25.0f;
// Медленнее этого никто не идёт: стоящий тоже когда-нибудь дойдёт.
constexpr float kMinSpeed = 0.3f;
// Путь длиннее прямой: орда идёт по дорогам и в обход (по замерам
// lw_headless --foresight — примерно на столько).
constexpr float kDetour = 1.15f;

// По видам: интересность вплотную к кульминации и за сколько секунд до неё
// она падает вдвое (не ниже kFarShare).
struct KindSpec {
    float weight;
    float halfTime;
};
constexpr KindSpec kSpecs[] = {
    {2.0f, 60.0f},  // Revenge
    {1.6f, 240.0f}, // Raid
    {1.3f, 900.0f}, // Turning
    {1.4f, 120.0f}, // CacheRace
    {1.0f, 60.0f},  // Theft
};
static_assert(std::size(kSpecs) == static_cast<std::size_t>(ForesightKind::Count));
// Доля интересности, которая остаётся при далёкой кульминации.
constexpr float kFarShare = 0.35f;

float hypot2(float ax, float ay, float bx, float by) {
    return std::hypot(ax - bx, ay - by);
}

float centerX(Cell cell) {
    return Map::cellCenter(cell.x);
}

float centerY(Cell cell) {
    return Map::cellCenter(cell.y);
}

// Сколько осталось пройти: по пути, если идёт, иначе по прямой до spot.
float remaining(const Position& at, const Walker& walker, Cell spot) {
    if (!walker.moving()) {
        return hypot2(at.x, at.y, centerX(spot), centerY(spot));
    }
    float length = 0.0f;
    float x = at.x;
    float y = at.y;
    for (std::size_t i = walker.next; i < walker.waypoints.size(); ++i) {
        const float nx = centerX(walker.waypoints[i]);
        const float ny = centerY(walker.waypoints[i]);
        length += hypot2(x, y, nx, ny);
        x = nx;
        y = ny;
    }
    return length;
}

float speedOf(const Walker& walker) {
    return std::max(kMinSpeed, walker.speed * walker.pace);
}

void rate(Foresight& foresight, float stakes) {
    const KindSpec& spec = kSpecs[static_cast<std::size_t>(foresight.kind)];
    const float urgency = spec.halfTime / (spec.halfTime + std::max(0.0f, foresight.eta));
    foresight.interest = spec.weight * (kFarShare + (1.0f - kFarShare) * urgency) * stakes;
}

// Живых людей в круге, кроме except.
int peopleNear(const World& world, float x, float y, float radius, EntityId except) {
    int count = 0;
    const entt::registry& registry = world.registry();
    world.spatial().forEachInRadius(x, y, radius, [&](const SpatialHit& hit) {
        if (hit.id != except && registry.valid(hit.entity) && registry.all_of<Person>(hit.entity)) {
            ++count;
        }
    });
    return count;
}

// Сколько ещё идти и через сколько секунд дело сделано: в пути — путь и
// скорость, на месте (обыск, кража) — остаток времени действия: кульминация —
// когда оно кончится.
std::pair<float, float> approach(const Mind& mind, const Position& at, const Walker& walker,
                                 std::uint64_t now) {
    if (mind.phase == ActionPhase::Perform) {
        return {0.0f, static_cast<float>(mind.until - std::min(mind.until, now)) /
                          static_cast<float>(kTicksPerSecond)};
    }
    const float distance = remaining(at, walker, mind.spot);
    return {distance, distance / speedOf(walker)};
}

// Мститель и вор: человек идёт к цели своего действия.
void foreseeDeeds(const World& world, std::vector<Foresight>& out) {
    const entt::registry& registry = world.registry();
    const std::uint64_t now = world.tick();
    for (const auto [entity, id, mind, position, walker] :
         registry.view<const Id, const Mind, const Position, const Walker>().each()) {
        if (mind.action == Action::Avenge) {
            const entt::entity foe = world.find(mind.target);
            if (foe == entt::null || !registry.all_of<Person, Position>(foe)) {
                continue;
            }
            const Position& at = registry.get<Position>(foe);
            Foresight& f = out.emplace_back();
            f.kind = ForesightKind::Revenge;
            f.hero = id.value;
            f.other = mind.target;
            f.focus = mind.target;
            f.x = at.x;
            f.y = at.y;
            f.distance = hypot2(position.x, position.y, at.x, at.y);
            f.eta = std::max(0.0f, f.distance - 2.0f) / speedOf(walker);
            f.crowd = 1;
            rate(f, 1.0f);
        } else if (mind.action == Action::Steal) {
            const entt::entity home = world.find(mind.target);
            if (home == entt::null || !registry.all_of<Building>(home)) {
                continue;
            }
            const Person* self = registry.try_get<Person>(entity);
            // Своя база — побег из группы: столкнётся с вожаком.
            const bool desertion = self != nullptr && self->home == mind.target;
            EntityId owner = kInvalidId;
            if (desertion) {
                if (const entt::entity band = world.find(self->group); band != entt::null) {
                    owner = registry.get<Group>(band).leader;
                }
            } else {
                for (const auto [other, otherId, person] : registry.view<const Id, const Person>().each()) {
                    if (person.home == mind.target && (owner == kInvalidId || otherId.value < owner)) {
                        owner = otherId.value;
                    }
                }
            }
            Foresight& f = out.emplace_back();
            f.kind = ForesightKind::Theft;
            f.hero = id.value;
            f.other = owner;
            f.focus = mind.target;
            f.x = centerX(mind.spot);
            f.y = centerY(mind.spot);
            std::tie(f.distance, f.eta) = approach(mind, position, walker, now);
            f.crowd = 1;
            rate(f, desertion ? 1.3f : 1.0f);
        }
    }
}

// Гонка за грузом: два и больше идут к одному ивенту (или уже обыскивают груз).
void foreseeRaces(const World& world, std::vector<Foresight>& out) {
    struct Claimant {
        EntityId event;
        float eta;
        EntityId id;
        float distance;
        float x;
        float y;
    };
    std::vector<Claimant> claimants;
    const entt::registry& registry = world.registry();
    const std::uint64_t now = world.tick();
    for (const auto [entity, id, mind, position, walker] :
         registry.view<const Id, const Mind, const Position, const Walker>().each()) {
        if (mind.action != Action::Salvage || mind.target == kInvalidId) {
            continue;
        }
        const auto [distance, eta] = approach(mind, position, walker, now);
        claimants.push_back({mind.target, eta, id.value, distance,
                             centerX(mind.spot), centerY(mind.spot)});
    }
    std::sort(claimants.begin(), claimants.end(), [](const Claimant& a, const Claimant& b) {
        return std::tie(a.event, a.eta, a.id) < std::tie(b.event, b.eta, b.id);
    });
    for (std::size_t i = 0; i < claimants.size();) {
        std::size_t end = i;
        while (end < claimants.size() && claimants[end].event == claimants[i].event) {
            ++end;
        }
        if (end - i >= 2) {
            const Claimant& first = claimants[i];
            const Claimant& second = claimants[i + 1];
            Foresight& f = out.emplace_back();
            f.kind = ForesightKind::CacheRace;
            f.hero = first.id;
            f.other = second.id;
            f.focus = first.event;
            f.x = first.x;
            f.y = first.y;
            f.distance = first.distance;
            f.eta = first.eta;
            f.crowd = static_cast<int>(end - i);
            // Ноздря в ноздрю — интереснее, чем когда второй далеко позади.
            const float close = 60.0f / (60.0f + (second.eta - first.eta));
            rate(f, std::min(1.5f, 0.7f + 0.15f * static_cast<float>(f.crowd)) *
                        (0.6f + 0.4f * close));
        }
        i = end;
    }
}

// Орда идёт к поселению, где есть люди.
void foreseeRaids(const World& world, std::vector<Foresight>& out) {
    const entt::registry& registry = world.registry();
    for (const auto [entity, id, horde] : registry.view<const Id, const Horde>().each()) {
        if (!horde.travelling || horde.members.empty()) {
            continue;
        }
        const float goalX = centerX(horde.goal);
        const float goalY = centerY(horde.goal);
        const EntityId settlementId = world.places().nearestSettlement(goalX, goalY);
        const entt::entity settlement = world.find(settlementId);
        if (settlement == entt::null || settlementId == horde.settlement) {
            continue;
        }
        const Settlement& place = registry.get<Settlement>(settlement);
        const float radius = static_cast<float>(place.radius) * Map::kCellSize;
        const float sx = centerX(place.center);
        const float sy = centerY(place.center);
        if (hypot2(goalX, goalY, sx, sy) > radius) {
            continue; // идёт не в поселение, а куда-то рядом
        }
        const entt::entity leader = world.find(horde.leader);
        if (leader == entt::null || !registry.all_of<Walker>(leader)) {
            continue;
        }
        const float distance = std::max(0.0f, hypot2(horde.x, horde.y, sx, sy) - radius);
        const float eta = kDetour * distance / speedOf(registry.get<Walker>(leader));
        if (eta > kHorizon) {
            continue;
        }
        const int people = peopleNear(world, sx, sy, radius, kInvalidId);
        if (people == 0) {
            continue;
        }
        Foresight& f = out.emplace_back();
        f.kind = ForesightKind::Raid;
        f.hero = horde.leader;
        f.other = settlementId;
        f.focus = id.value;
        f.x = sx;
        f.y = sy;
        f.distance = distance;
        f.eta = eta;
        f.crowd = people;
        rate(f, std::min(1.6f, 0.5f + 0.08f * static_cast<float>(people)) *
                    std::min(1.5f, 0.7f + 0.04f * static_cast<float>(horde.members.size())));
    }
}

// Укушенный в лихорадке среди людей; тело укушенного, которое вот-вот встанет.
void foreseeTurning(const World& world, std::vector<Foresight>& out) {
    const entt::registry& registry = world.registry();
    const float mid = 0.5f * static_cast<float>(kTurnMinMinutes + kTurnMaxMinutes) *
                      kGameHourSeconds / 60.0f;
    const auto add = [&](EntityId id, const Position& at, float eta) {
        const int crowd = peopleNear(world, at.x, at.y, kTurningCrowd, id);
        if (crowd == 0) {
            return; // встанет в одиночестве — не страшно никому
        }
        Foresight& f = out.emplace_back();
        f.kind = ForesightKind::Turning;
        f.hero = id;
        f.focus = id;
        f.x = at.x;
        f.y = at.y;
        f.eta = eta;
        f.crowd = crowd;
        rate(f, std::min(1.4f, 0.6f + 0.2f * static_cast<float>(crowd)));
    };
    for (const auto [entity, id, wounds, position] :
         registry.view<const Id, const Wounds, const Position>().each()) {
        if (wounds.infection >= kTurningFrom && registry.all_of<Person>(entity)) {
            add(id.value, position,
                (1.0f - wounds.infection) * kInfectionHours * kGameHourSeconds + mid);
        }
    }
    const std::uint64_t now = world.tick();
    for (const auto [entity, id, corpse, position] :
         registry.view<const Id, const Corpse, const Position>().each()) {
        if (corpse.turnsAt > now) {
            add(id.value, position,
                static_cast<float>(corpse.turnsAt - now) / static_cast<float>(kTicksPerSecond));
        }
    }
}

} // namespace

std::vector<Foresight> foresee(const World& world) {
    std::vector<Foresight> all;
    foreseeDeeds(world, all);
    foreseeRaces(world, all);
    foreseeRaids(world, all);
    foreseeTurning(world, all);
    std::erase_if(all, [](const Foresight& f) {
        return f.kind != ForesightKind::Turning && f.eta > kHorizon;
    });
    std::sort(all.begin(), all.end(), [](const Foresight& a, const Foresight& b) {
        if (a.interest != b.interest) {
            return a.interest > b.interest;
        }
        return std::tie(a.kind, a.hero, a.focus) < std::tie(b.kind, b.hero, b.focus);
    });
    return all;
}

bool fulfilledBy(const World& world, const Foresight& foresight, const ChronicleEvent& event) {
    using K = ChronicleKind;
    switch (foresight.kind) {
    case ForesightKind::Revenge:
        return (event.kind == K::Avenged && event.subject == foresight.hero &&
                event.object == foresight.other) ||
               (event.kind == K::AssaultedBy && event.subject == foresight.other &&
                event.object == foresight.hero) ||
               (event.kind == K::Died && event.subject == foresight.other &&
                event.object == foresight.hero);
    case ForesightKind::Raid:
        return event.kind == K::HordeArrived && event.subject == foresight.focus &&
               event.object == foresight.other;
    case ForesightKind::Turning:
        return event.kind == K::Turned && event.subject == foresight.hero;
    case ForesightKind::CacheRace: {
        if (event.kind != K::Salvaged) {
            return false;
        }
        // Забрали из груза этого ивента (object — груз или сам ивент); груза
        // уже нет — хотя бы кто-то из тех, кто шёл первым.
        if (event.object == foresight.focus) {
            return true;
        }
        const entt::entity cache = world.find(event.object);
        if (cache != entt::null) {
            const auto* box = world.registry().try_get<Cache>(cache);
            return box != nullptr && box->event == foresight.focus;
        }
        return event.subject == foresight.hero || event.subject == foresight.other;
    }
    case ForesightKind::Theft:
        return (event.kind == K::Stole || event.kind == K::Deserted) &&
               event.subject == foresight.hero;
    default:
        return false;
    }
}

const char* foresightKindName(ForesightKind kind) {
    constexpr const char* kNames[] = {"revenge", "raid", "turning", "cache-race", "theft"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ForesightKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "?";
}

} // namespace lw
