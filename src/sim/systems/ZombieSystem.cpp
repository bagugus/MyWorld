#include "sim/systems/ZombieSystem.h"

#include "sim/Hash.h"
#include "sim/Calendar.h"
#include "sim/Perception.h"
#include "sim/Population.h"
#include "sim/Places.h"
#include "sim/Relations.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"
#include "sim/systems/MovementSystem.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace lw {

namespace {

// --- Зомби ---------------------------------------------------------------

constexpr float kZombieMinSpeed = 1.3f; // скорость погони, м/с
constexpr float kZombieMaxSpeed = 1.7f;
// Видят хуже человека, слышат лучше.
constexpr float kZombieSight = 90.0f;
constexpr float kZombieHearing = 1.3f;

// Одиночка бредёт короткими переходами до kRoamStep метров, а ушёл от родных
// мест дальше kRoamRadius — возвращается к ним.
constexpr float kRoamStep = 30.0f;
constexpr float kRoamRadius = 80.0f;
constexpr int kRoamAttempts = 4;
// Чаще стоит, чем бредёт: так и выглядят зомби, и движение дешевле.
constexpr float kRoamChance = 0.35f; // иначе стоит
constexpr int kIdleMinSeconds = 20;
constexpr int kIdleMaxSeconds = 120;
// Сколько стоит там, где слышал шум или потерял добычу.
constexpr int kLingerMinSeconds = 10;
constexpr int kLingerMaxSeconds = 40;
// Ближе этого к добыче — догнал: стоит рядом и бьёт (CombatSystem).
constexpr float kReachDistance = 1.5f;
// Цель сдвинулась дальше этого — проложить путь заново, метры.
constexpr float kChaseRegoal = 4.0f;
constexpr float kInvestigateRegoal = 8.0f;
// Стон, которым преследующий созывает других: слышен в стольких метрах.
constexpr float kCallLoudness = 60.0f;
constexpr std::uint64_t kCallCooldownTicks = 15 * kTicksPerSecond;

// --- Орды ----------------------------------------------------------------

constexpr float kFollowSpeed = 0.55f;  // догоняет своё место в орде
constexpr float kCatchUpSpeed = 0.9f;  // отстал или орда бросилась за добычей
constexpr float kCatchUpDistance = 25.0f;
// Место в орде: круг вокруг вожака радиусом kSpreadBase + kSpreadPerRoot·√n метров.
constexpr float kSpreadBase = 2.0f;
constexpr float kSpreadPerRoot = 1.0f;
constexpr float kSlotTolerance = 3.0f;  // ближе к своему месту — стоит
constexpr float kSlotRegoal = 6.0f;
// Дальше от вожака — отбился от орды и стал одиночкой.
constexpr float kHordeLeash = 300.0f;
constexpr float kMergeDistance = 40.0f; // центры двух орд ближе — сливаются
constexpr std::size_t kHordeMax = 60;
// Одиночку у центра орды она подбирает с такой вероятностью за обновление.
constexpr float kRecruitRadius = 20.0f;
constexpr float kRecruitChance = 0.3f;
// Возбуждённые (видели или слышали добычу за последние полминуты симуляции)
// одиночки, сбежавшиеся в одно место, сбиваются в орду.
constexpr float kGatherRadius = 15.0f;
constexpr std::size_t kGatherMin = 6;
constexpr std::uint64_t kGatherAlertTicks = 30 * kTicksPerSecond;
// Куда ведёт вожак: поселение не дальше kHordeRange, иначе (или с
// вероятностью 1 − kSettlementChance) — точка не дальше kHordeWanderRange.
constexpr float kHordeRange = 3000.0f;
constexpr float kHordeWanderRange = 1500.0f;
constexpr float kArriveDistance = 20.0f; // ближе к цели — пришли
constexpr float kSettlementChance = 0.4f;
constexpr int kGoalAttempts = 8;
// У цели орда стоит столько игровых часов, вожак переминается рядом.
constexpr int kRestMinMinutes = 60;
constexpr int kRestMaxMinutes = 240;
constexpr float kRestRoamRadius = 40.0f;
constexpr int kRestIdleMinSeconds = 30;
constexpr int kRestIdleMaxSeconds = 120;
// Путь не нашёлся — попробовать снова через столько игровых минут.
constexpr int kRetryMinutes = 10;
// Новая орда с начала мира трогается не сразу: в пределах стольких минут.
constexpr int kFirstMoveMaxMinutes = 120;

// --- Угроза для человека -------------------------------------------------

// Видимый зомби пугает ближе этого, слышимый — ближе kThreatHearRange, метры.
constexpr float kThreatSightRange = 60.0f;
constexpr float kThreatHearRange = 40.0f;
// Каждый следующий замеченный зомби добавляет столько угрозы.
constexpr float kCrowdThreat = 0.1f;
// Тот, кого преследуют, в опасности, даже если зомби пока далеко.
constexpr float kHuntedThreat = 0.9f;
constexpr float kHuntedRange = 80.0f;
// Человек бьёт того, кто дорог (товарища по группе или близкого, этап 6.5), —
// угроза слабее, чем если бьют самого.
constexpr float kDefendThreat = 0.8f;
constexpr float kDearCloseness = 0.2f;

std::uint64_t seconds(int count) {
    return static_cast<std::uint64_t>(count) * kTicksPerSecond;
}

std::uint64_t gameMinutes(int count) {
    return static_cast<std::uint64_t>(count) * kTicksPerGameMinute;
}

float distance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

float cellX(Cell cell) {
    return Map::cellCenter(cell.x);
}

float cellY(Cell cell) {
    return Map::cellCenter(cell.y);
}

bool calm(ZombieState state) {
    return state == ZombieState::Idle || state == ZombieState::Roam ||
           state == ZombieState::Follow;
}

// Место зомби в орде — постоянное смещение от вожака в круге радиуса 1, из id
// (без рандома: место не меняется, сколько бы раз его ни спрашивали).
void slotOffset(EntityId id, float& ox, float& oy) {
    Hasher hasher;
    hasher.add(id);
    const std::uint64_t bits = hasher.value();
    ox = static_cast<float>(bits & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
    oy = static_cast<float>((bits >> 16) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
    const float length = std::sqrt(ox * ox + oy * oy);
    if (length > 1.0f) {
        ox /= length;
        oy /= length;
    }
}

float spreadOf(std::size_t members) {
    return kSpreadBase + kSpreadPerRoot * std::sqrt(static_cast<float>(members));
}

// Поселение, в круге которого лежит точка; kInvalidId — ни в каком.
EntityId settlementAt(const World& world, float x, float y) {
    const EntityId id = world.places().nearestSettlement(x, y);
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return kInvalidId;
    }
    const Settlement& settlement = world.registry().get<Settlement>(entity);
    const float radius = static_cast<float>(settlement.radius) * Map::kCellSize;
    return distance(x, y, cellX(settlement.center), cellY(settlement.center)) <= radius
               ? id
               : kInvalidId;
}

void setSpeed(Walker& walker, float metersPerSecond) {
    walker.pace = std::max(0.05f, metersPerSecond / walker.speed);
}

// --- Решения одного зомби ------------------------------------------------

class ZombieBrain {
public:
    ZombieBrain(World& world, Rng& rng)
        : world_(world), registry_(world.registry()), rng_(rng), tick_(world.tick()) {}

    void think(entt::entity entity, EntityId id, Zombie& zombie, Walker& walker,
               const Position& position, const Awareness& awareness, Combat& combat) {
        entity_ = entity;
        id_ = id;
        zombie_ = &zombie;
        walker_ = &walker;
        position_ = position;
        combat_ = &combat;
        combat.target = kInvalidId; // бьёт, только если догнал (chase)

        // Добыча на виду — погоня.
        const Contact* prey = nullptr;
        // Куда идти: слышал добычу, слышал зов другого зомби, помнит, где её потерял.
        const Contact* trail = nullptr;
        int trailRank = 0;
        for (std::size_t i = 0; i < awareness.count; ++i) {
            const Contact& contact = awareness.contacts[i];
            if (zombie.checked != 0 && contact.tick <= zombie.checked) {
                continue; // уже проверено
            }
            const bool fresh = isFresh(contact, tick_);
            const bool fellow = isZombie(contact.id);
            if (fresh && contact.seen && !fellow) {
                prey = &contact; // контакты — ближние вперёд
                break;
            }
            int rank = 0;
            if (fresh && !fellow && contact.heard != SoundKind::None) {
                rank = 3;
            } else if (fresh && fellow && contact.heard == SoundKind::Groan) {
                rank = 2;
            } else if (!fresh && !fellow) {
                rank = 1;
            }
            if (rank > trailRank) {
                trail = &contact;
                trailRank = rank;
            }
        }

        if (prey != nullptr) {
            chase(*prey);
            return;
        }
        if (trail != nullptr) {
            investigate(*trail);
        }
        if (zombie.state == ZombieState::Investigate) {
            if (!walker.moving()) {
                if (zombie.until == 0) {
                    // Дошёл: всё замеченное до этого проверено.
                    zombie.checked = tick_;
                    zombie.until =
                        tick_ + seconds(rng_.rangeInt(kLingerMinSeconds, kLingerMaxSeconds));
                } else if (tick_ >= zombie.until) {
                    becomeIdle(0);
                }
            }
            if (zombie.state == ZombieState::Investigate) {
                return;
            }
        }
        if (zombie.state == ZombieState::Chase) {
            // Добыча пропала из виду, а где она была, уже проверено.
            becomeIdle(0);
        }

        const entt::entity hordeEntity = world_.find(zombie.horde);
        if (hordeEntity != entt::null) {
            Horde& horde = registry_.get<Horde>(hordeEntity);
            if (horde.leader == id_) {
                leadHorde(horde);
            } else {
                follow(horde);
            }
            return;
        }
        roam();
    }

private:
    bool isZombie(EntityId id) const {
        const entt::entity entity = world_.find(id);
        return entity != entt::null && registry_.all_of<Zombie>(entity);
    }

    bool walkTo(Cell goal) {
        zombie_->goal = goal;
        if (!startWalkDirect(world_, entity_, goal)) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        walker_->waypoints.clear();
        walker_->next = 0;
    }

    void becomeIdle(int idleSeconds) {
        zombie_->state = ZombieState::Idle;
        zombie_->target = kInvalidId;
        zombie_->until = tick_ + seconds(idleSeconds);
        stop();
    }

    void chase(const Contact& prey) {
        Zombie& zombie = *zombie_;
        const bool started = zombie.state != ZombieState::Chase || zombie.target != prey.id;
        zombie.state = ZombieState::Chase;
        zombie.target = prey.id;
        zombie.alerted = tick_;
        zombie.until = 0;
        if (started && (zombie.lastGroan == 0 || tick_ >= zombie.lastGroan + kCallCooldownTicks)) {
            world_.emitSound(position_.x, position_.y, kCallLoudness, SoundKind::Groan, id_);
            zombie.lastGroan = tick_;
        }
        if (distance(position_.x, position_.y, prey.x, prey.y) <= kReachDistance) {
            stop(); // догнал — бьёт
            combat_->target = prey.id;
            return;
        }
        const Cell goal = Map::cellAt(prey.x, prey.y);
        setSpeed(*walker_, walker_->speed);
        if (started || !walker_->moving() ||
            distance(cellX(goal), cellY(goal), cellX(zombie.goal), cellY(zombie.goal)) >
                kChaseRegoal) {
            walkTo(goal);
        }
    }

    void investigate(const Contact& trail) {
        Zombie& zombie = *zombie_;
        const Cell goal = Map::cellAt(trail.x, trail.y);
        zombie.alerted = tick_;
        if (zombie.state == ZombieState::Investigate &&
            distance(cellX(goal), cellY(goal), cellX(zombie.goal), cellY(zombie.goal)) <=
                kInvestigateRegoal) {
            return;
        }
        zombie.state = ZombieState::Investigate;
        zombie.target = kInvalidId;
        zombie.until = 0;
        setSpeed(*walker_, kInvestigateSpeed);
        if (!walkTo(goal)) {
            // Не дойти — считать проверенным, иначе он будет пытаться снова.
            zombie.checked = tick_;
            becomeIdle(rng_.rangeInt(kIdleMinSeconds, kIdleMaxSeconds));
        }
    }

    // Клетка не дальше radius от центра (cx, cy), куда можно дойти.
    std::optional<Cell> pickNear(float cx, float cy, float radius) {
        const NavGrid& nav = world_.nav();
        const Cell from = Map::cellAt(position_.x, position_.y);
        for (int attempt = 0; attempt < kRoamAttempts; ++attempt) {
            const Cell cell = Map::cellAt(cx + rng_.range(-radius, radius),
                                          cy + rng_.range(-radius, radius));
            if (nav.passable(cell) && nav.connected(from, cell)) {
                return cell;
            }
        }
        return std::nullopt;
    }

    // Одиночка: стоит или бредёт по родным местам.
    void roam() {
        Zombie& zombie = *zombie_;
        if (zombie.state == ZombieState::Follow) {
            becomeIdle(0);
        }
        if (walker_->moving() || tick_ < zombie.until) {
            return;
        }
        if (rng_.chance(kRoamChance)) {
            const float ax = cellX(zombie.anchor);
            const float ay = cellY(zombie.anchor);
            const bool away = distance(position_.x, position_.y, ax, ay) > kRoamRadius;
            if (const auto cell = away ? pickNear(ax, ay, kRoamStep)
                                       : pickNear(position_.x, position_.y, kRoamStep)) {
                zombie.state = ZombieState::Roam;
                setSpeed(*walker_, kZombieRoamSpeed);
                if (walkTo(*cell)) {
                    return;
                }
            }
        }
        becomeIdle(rng_.rangeInt(kIdleMinSeconds, kIdleMaxSeconds));
    }

    // Вожак: ведёт орду к цели, у цели стоит и переминается.
    void leadHorde(Horde& horde) {
        Zombie& zombie = *zombie_;
        if (zombie.state == ZombieState::Follow) {
            becomeIdle(0);
        }
        if (walker_->moving()) {
            return;
        }
        if (horde.travelling) {
            // Отвлёкся на добычу по дороге — вести дальше.
            if (distance(position_.x, position_.y, cellX(horde.goal), cellY(horde.goal)) >
                kArriveDistance) {
                zombie.state = ZombieState::Roam;
                zombie.goal = horde.goal;
                setSpeed(*walker_, kHordeSpeed);
                if (startWalk(world_, entity_, horde.goal)) {
                    return;
                }
            }
            horde.travelling = false;
            horde.until = tick_ + gameMinutes(rng_.rangeInt(kRestMinMinutes, kRestMaxMinutes));
        }
        if (tick_ < horde.until) {
            if (tick_ < zombie.until) {
                return;
            }
            if (rng_.chance(kRoamChance)) {
                if (const auto cell =
                        pickNear(cellX(horde.goal), cellY(horde.goal), kRestRoamRadius)) {
                    zombie.state = ZombieState::Roam;
                    setSpeed(*walker_, kZombieRoamSpeed);
                    if (walkTo(*cell)) {
                        return;
                    }
                }
            }
            becomeIdle(rng_.rangeInt(kRestIdleMinSeconds, kRestIdleMaxSeconds));
            return;
        }
        // Дальше: в соседнее поселение или просто куда-то.
        if (const auto goal = pickHordeGoal(horde)) {
            zombie.state = ZombieState::Roam;
            zombie.goal = *goal;
            setSpeed(*walker_, kHordeSpeed);
            if (startWalk(world_, entity_, *goal)) {
                horde.goal = *goal;
                horde.travelling = true;
                return;
            }
        }
        horde.until = tick_ + gameMinutes(kRetryMinutes);
        becomeIdle(0);
    }

    std::optional<Cell> pickHordeGoal(const Horde& horde) {
        const Places& places = world_.places();
        const NavGrid& nav = world_.nav();
        const Cell from = Map::cellAt(position_.x, position_.y);
        if (rng_.chance(kSettlementChance)) {
            std::vector<const Places::SettlementPlace*> near;
            for (const Places::SettlementPlace& settlement : places.settlements()) {
                if (settlement.id != horde.settlement &&
                    distance(position_.x, position_.y, settlement.x, settlement.y) <= kHordeRange &&
                    nav.connected(from, settlement.center)) {
                    near.push_back(&settlement);
                }
            }
            if (!near.empty()) {
                return near[rng_.below(static_cast<std::uint32_t>(near.size()))]->center;
            }
        }
        for (int attempt = 0; attempt < kGoalAttempts; ++attempt) {
            const Cell cell =
                Map::cellAt(position_.x + rng_.range(-kHordeWanderRange, kHordeWanderRange),
                            position_.y + rng_.range(-kHordeWanderRange, kHordeWanderRange));
            if (nav.passable(cell) && nav.connected(from, cell)) {
                return cell;
            }
        }
        return std::nullopt;
    }

    // Участник орды: держится своего места вокруг вожака.
    void follow(const Horde& horde) {
        Zombie& zombie = *zombie_;
        const entt::entity leader = world_.find(horde.leader);
        if (leader == entt::null || !registry_.all_of<Zombie>(leader)) {
            roam(); // вожака нет или он упокоен — нового назначит обновление орд
            return;
        }
        if (zombie.state != ZombieState::Follow) {
            zombie.state = ZombieState::Follow;
            zombie.target = kInvalidId;
            stop();
        }
        const Position& at = registry_.get<Position>(leader);
        float ox = 0.0f;
        float oy = 0.0f;
        slotOffset(id_, ox, oy);
        const float spread = spreadOf(horde.members.size());
        const float sx = at.x + ox * spread;
        const float sy = at.y + oy * spread;
        const float d = distance(position_.x, position_.y, sx, sy);
        if (!walker_->moving() && d <= kSlotTolerance) {
            return;
        }
        if (walker_->moving() &&
            distance(cellX(zombie.goal), cellY(zombie.goal), sx, sy) <= kSlotRegoal) {
            return;
        }
        Cell slot = Map::cellAt(sx, sy);
        if (!world_.nav().passable(slot)) {
            slot = Map::cellAt(at.x, at.y);
        }
        // Вожак бросился за добычей или отстал — бегом.
        const ZombieState leaderState = registry_.get<Zombie>(leader).state;
        const bool surge = leaderState == ZombieState::Chase ||
                           leaderState == ZombieState::Investigate || d > kCatchUpDistance;
        setSpeed(*walker_, surge ? kCatchUpSpeed : kFollowSpeed);
        walkTo(slot);
    }

    World& world_;
    entt::registry& registry_;
    Rng& rng_;
    std::uint64_t tick_;
    entt::entity entity_ = entt::null;
    EntityId id_ = kInvalidId;
    Zombie* zombie_ = nullptr;
    Walker* walker_ = nullptr;
    Combat* combat_ = nullptr;
    Position position_;
};

// --- Орды ----------------------------------------------------------------

void leaveHorde(World& world, entt::entity entity) {
    entt::registry& registry = world.registry();
    Zombie& zombie = registry.get<Zombie>(entity);
    // Родные места прежние: отбившийся бредёт туда, где обратился, а не оседает
    // в деревне, через которую прошла орда.
    zombie.horde = kInvalidId;
    if (zombie.state == ZombieState::Follow) {
        zombie.state = ZombieState::Idle;
        zombie.until = world.tick();
    }
}

void join(World& world, Horde& horde, EntityId hordeId, EntityId member) {
    const entt::entity entity = world.find(member);
    world.registry().get<Zombie>(entity).horde = hordeId;
    horde.members.insert(std::upper_bound(horde.members.begin(), horde.members.end(), member),
                         member);
}

// Участники на месте, вожак есть, центр посчитан. false — орды больше нет.
bool maintain(World& world, entt::entity hordeEntity, EntityId hordeId) {
    entt::registry& registry = world.registry();
    Horde& horde = registry.get<Horde>(hordeEntity);
    std::vector<EntityId> members;
    members.reserve(horde.members.size());
    for (const EntityId member : horde.members) {
        const entt::entity entity = world.find(member);
        if (entity != entt::null && registry.all_of<Zombie>(entity) &&
            registry.get<Zombie>(entity).horde == hordeId) {
            members.push_back(member);
        }
    }
    if (std::find(members.begin(), members.end(), horde.leader) == members.end() &&
        !members.empty()) {
        horde.leader = members.front();
    }
    // Отбившиеся далеко от вожака — сами по себе.
    if (!members.empty()) {
        const Position& at = registry.get<Position>(world.find(horde.leader));
        std::erase_if(members, [&](EntityId member) {
            const entt::entity entity = world.find(member);
            const Position& position = registry.get<Position>(entity);
            if (distance(position.x, position.y, at.x, at.y) <= kHordeLeash) {
                return false;
            }
            leaveHorde(world, entity);
            return true;
        });
    }
    horde.members = std::move(members);
    if (horde.members.size() < 2) {
        world.record(ChronicleKind::HordeScattered, hordeId);
        for (const EntityId member : horde.members) {
            leaveHorde(world, world.find(member));
        }
        world.destroy(hordeEntity);
        return false;
    }

    float x = 0.0f;
    float y = 0.0f;
    for (const EntityId member : horde.members) {
        const Position& position = registry.get<Position>(world.find(member));
        x += position.x;
        y += position.y;
    }
    const float n = static_cast<float>(horde.members.size());
    horde.x = x / n;
    horde.y = y / n;
    const EntityId settlement = settlementAt(world, horde.x, horde.y);
    if (settlement != horde.settlement) {
        if (settlement != kInvalidId) {
            world.record(ChronicleKind::HordeArrived, hordeId, settlement,
                         static_cast<std::int32_t>(horde.members.size()));
        }
        horde.settlement = settlement;
    }
    return true;
}

std::vector<std::pair<EntityId, entt::entity>> hordesById(World& world) {
    std::vector<std::pair<EntityId, entt::entity>> hordes;
    for (const auto [entity, id, horde] : world.registry().view<const Id, const Horde>().each()) {
        hordes.emplace_back(id.value, entity);
    }
    std::sort(hordes.begin(), hordes.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return hordes;
}

void updateHordes(World& world) {
    entt::registry& registry = world.registry();
    Rng& rng = world.rng(RngStream::Zombies);
    const std::uint64_t tick = world.tick();

    std::vector<std::pair<EntityId, entt::entity>> hordes;
    for (const auto& [id, entity] : hordesById(world)) {
        if (maintain(world, entity, id)) {
            hordes.emplace_back(id, entity);
        }
    }

    // Слияние: большая поглощает меньшую (при равных — с меньшим id).
    for (std::size_t i = 0; i < hordes.size(); ++i) {
        for (std::size_t j = i + 1; j < hordes.size(); ++j) {
            if (hordes[i].second == entt::null || hordes[j].second == entt::null) {
                continue;
            }
            Horde& a = registry.get<Horde>(hordes[i].second);
            Horde& b = registry.get<Horde>(hordes[j].second);
            if (distance(a.x, a.y, b.x, b.y) > kMergeDistance ||
                a.members.size() + b.members.size() > kHordeMax) {
                continue;
            }
            const bool aWins = a.members.size() >= b.members.size();
            auto& [winnerId, winnerEntity] = aWins ? hordes[i] : hordes[j];
            auto& [loserId, loserEntity] = aWins ? hordes[j] : hordes[i];
            Horde& winner = registry.get<Horde>(winnerEntity);
            const std::vector<EntityId> absorbed = registry.get<Horde>(loserEntity).members;
            for (const EntityId member : absorbed) {
                join(world, winner, winnerId, member);
            }
            world.record(ChronicleKind::HordeMerged, winnerId, loserId,
                         static_cast<std::int32_t>(winner.members.size()));
            world.destroy(loserEntity);
            loserEntity = entt::null;
            if (!aWins) {
                break; // орды i больше нет
            }
        }
    }
    std::erase_if(hordes, [](const auto& horde) { return horde.second == entt::null; });

    // Одиночки у орды пристают к ней.
    std::vector<EntityId> recruits;
    for (const auto& [hordeId, hordeEntity] : hordes) {
        Horde& horde = registry.get<Horde>(hordeEntity);
        recruits.clear();
        const auto lone = [&](const SpatialHit& hit) {
            const Zombie* zombie = registry.try_get<Zombie>(hit.entity);
            if (zombie != nullptr && zombie->horde == kInvalidId && calm(zombie->state)) {
                recruits.push_back(hit.id);
            }
        };
        world.spatial().forEachInRadius(horde.x, horde.y, kRecruitRadius, lone);
        std::sort(recruits.begin(), recruits.end());
        for (const EntityId recruit : recruits) {
            if (horde.members.size() < kHordeMax && rng.chance(kRecruitChance)) {
                join(world, horde, hordeId, recruit);
            }
        }
    }

    // Одиночки, сбежавшиеся на шум, сбиваются в орду.
    std::vector<std::pair<EntityId, entt::entity>> loners;
    for (const auto [entity, id, zombie] : registry.view<const Id, const Zombie>().each()) {
        if (zombie.horde == kInvalidId && zombie.alerted + kGatherAlertTicks >= tick &&
            zombie.alerted > 0) {
            loners.emplace_back(id.value, entity);
        }
    }
    std::sort(loners.begin(), loners.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<EntityId> gathered;
    for (const auto& [id, entity] : loners) {
        if (registry.get<Zombie>(entity).horde != kInvalidId) {
            continue; // уже собрался в орду в этом обновлении
        }
        const Position& position = registry.get<Position>(entity);
        gathered.clear();
        const auto excited = [&](const SpatialHit& hit) {
            const Zombie* other = registry.try_get<Zombie>(hit.entity);
            if (other != nullptr && other->horde == kInvalidId && other->alerted > 0 &&
                other->alerted + kGatherAlertTicks >= tick) {
                gathered.push_back(hit.id);
            }
        };
        world.spatial().forEachInRadius(position.x, position.y, kGatherRadius, excited);
        if (gathered.size() >= kGatherMin) {
            if (gathered.size() > kHordeMax) {
                std::sort(gathered.begin(), gathered.end());
                gathered.resize(kHordeMax);
            }
            formHorde(world, gathered, HordeOrigin::Gathered);
        }
    }
}

} // namespace

Threat threatOf(const World& world, entt::entity person) {
    Threat threat;
    const entt::registry& registry = world.registry();
    const auto* awareness = registry.try_get<Awareness>(person);
    const auto* position = registry.try_get<Position>(person);
    if (awareness == nullptr || position == nullptr) {
        return threat;
    }
    const EntityId self = registry.get<Id>(person).value;
    const std::uint64_t tick = world.tick();
    const auto* own = registry.try_get<Person>(person);
    const auto* relations = registry.try_get<Relations>(person);
    // Дорог ли человек: товарищ по группе или близкий (этап 6.5).
    const auto dear = [&](EntityId other) {
        const entt::entity entity = world.find(other);
        const auto* theirs = entity != entt::null ? registry.try_get<Person>(entity) : nullptr;
        if (theirs == nullptr) {
            return false;
        }
        if (own != nullptr && own->group != kInvalidId && theirs->group == own->group) {
            return true;
        }
        const Bond* bond = relations != nullptr ? relations->find(other) : nullptr;
        return bond != nullptr && !bond->hostile() && closeness(*bond) >= kDearCloseness;
    };
    float nearest = 0.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    for (std::size_t i = 0; i < awareness->count; ++i) {
        const Contact& contact = awareness->contacts[i];
        if (!isFresh(contact, tick)) {
            continue;
        }
        const entt::entity entity = world.find(contact.id);
        if (entity == entt::null) {
            continue;
        }
        const Zombie* zombie = registry.try_get<Zombie>(entity);
        const float d = distance(position->x, position->y, contact.x, contact.y);
        float level = 0.0f;
        if (zombie != nullptr) {
            level = contact.seen ? 1.0f - d / kThreatSightRange : 0.0f;
            if (contact.heard != SoundKind::None) {
                level = std::max(level, 1.0f - d / kThreatHearRange);
            }
            if (zombie->state == ZombieState::Chase && zombie->target == self && d <= kHuntedRange) {
                level = std::max(level, kHuntedThreat);
            }
        } else if (const auto* combat = registry.try_get<Combat>(entity);
                   combat != nullptr && combat->target != kInvalidId &&
                   registry.all_of<Person>(entity)) {
            // Человек, который бьёт его самого или того, кто ему дорог (этап 6.5).
            if (combat->target == self) {
                level = std::max(contact.seen ? 1.0f - d / kThreatSightRange : 0.0f,
                                 d <= kHuntedRange ? kHuntedThreat : 0.0f);
                threat.assailant = contact.id;
            } else if (contact.seen && dear(combat->target)) {
                level = kDefendThreat * (1.0f - d / kThreatSightRange);
            }
        }
        if (level <= 0.0f) {
            continue;
        }
        if (threat.nearest == kInvalidId || d < nearest) {
            threat.nearest = contact.id;
            threat.nearestX = contact.x;
            threat.nearestY = contact.y;
            nearest = d;
        }
        threat.level = std::max(threat.level, level);
        ++threat.count;
        // Прочь от каждого, сильнее — от опасных.
        const float inv = level / std::max(d, 0.5f);
        ax += (position->x - contact.x) * inv;
        ay += (position->y - contact.y) * inv;
    }
    if (threat.count == 0) {
        return threat;
    }
    threat.level =
        std::min(1.0f, threat.level + kCrowdThreat * static_cast<float>(threat.count - 1));
    const float length = std::sqrt(ax * ax + ay * ay);
    if (length > 1e-4f) {
        threat.awayX = ax / length;
        threat.awayY = ay / length;
    } else {
        threat.awayY = -1.0f; // зомби ровно там же: куда угодно, лишь бы прочь
    }
    return threat;
}

entt::entity createZombie(World& world, Cell cell, Rng& rng) {
    entt::registry& registry = world.registry();
    const entt::entity entity = world.create();
    // Не ровно в центре клетки: несколько зомби в одной клетке не сливаются.
    const float jitter = Map::kCellSize * 0.35f;
    registry.emplace<Position>(entity, Map::cellCenter(cell.x) + rng.range(-jitter, jitter),
                               Map::cellCenter(cell.y) + rng.range(-jitter, jitter));
    raiseZombie(world, entity, cell, rng);
    return entity;
}

void raiseZombie(World& world, entt::entity entity, Cell cell, Rng& rng) {
    entt::registry& registry = world.registry();
    registry.emplace_or_replace<Velocity>(entity);
    Walker walker;
    walker.speed = rng.range(kZombieMinSpeed, kZombieMaxSpeed);
    setSpeed(walker, kZombieRoamSpeed);
    registry.emplace<Walker>(entity, std::move(walker));
    registry.emplace<Senses>(entity, Senses{kZombieSight, kZombieHearing});
    registry.emplace<Awareness>(entity);
    registry.emplace<Combat>(entity);
    Zombie zombie;
    zombie.anchor = cell;
    zombie.goal = cell;
    zombie.until = world.tick() + seconds(rng.rangeInt(0, kIdleMaxSeconds));
    registry.emplace<Zombie>(entity, zombie);
}

EntityId formHorde(World& world, std::vector<EntityId> members, HordeOrigin origin) {
    entt::registry& registry = world.registry();
    std::sort(members.begin(), members.end());
    const entt::entity entity = world.create();
    const EntityId id = registry.get<Id>(entity).value;
    Horde horde;
    horde.leader = members.front();
    const entt::entity leader = world.find(horde.leader);
    const Position& at = registry.get<Position>(leader);
    horde.goal = Map::cellAt(at.x, at.y);
    horde.x = at.x;
    horde.y = at.y;
    // Сбежавшиеся на шум стоят там, где собрались; орды с начала мира
    // трогаются вразнобой; пришедшие издалека идут дальше сразу.
    int wait = 0;
    if (origin == HordeOrigin::Gathered) {
        wait = kRestMinMinutes;
    } else if (origin == HordeOrigin::Start) {
        wait = static_cast<int>(world.rng(RngStream::Zombies).below(kFirstMoveMaxMinutes));
    }
    horde.until = world.tick() + gameMinutes(wait);
    horde.settlement = settlementAt(world, at.x, at.y);
    for (const EntityId member : members) {
        Zombie& zombie = registry.get<Zombie>(world.find(member));
        zombie.horde = id;
    }
    horde.members = std::move(members);
    const auto size = static_cast<std::int32_t>(horde.members.size());
    registry.emplace<Horde>(entity, std::move(horde));
    world.record(ChronicleKind::HordeFormed, id, kInvalidId, size, static_cast<std::int32_t>(origin));
    return id;
}

void updateZombies(World& world) {
    const std::uint64_t tick = world.tick();
    Rng& rng = world.rng(RngStream::Zombies);
    ZombieBrain brain(world, rng);
    // Порядок — порядок хранения в реестре: он детерминирован. Компоненты по
    // ходу не добавляются и не удаляются.
    auto view = world.registry()
                    .view<const Id, Zombie, Walker, const Position, const Awareness, Combat>();
    for (const auto [entity, id, zombie, walker, position, awareness, combat] : view.each()) {
        if ((tick + id.value) % kZombieThinkTicks == 0) {
            brain.think(entity, id.value, zombie, walker, position, awareness, combat);
        }
    }
    if (tick % kHordeUpdateTicks == 0) {
        updateHordes(world);
    }
    // Приток: упокоенных заменяют пришедшие издалека, не больше орды в сутки.
    if ((tick + kStartGameTick) % kTicksPerGameDay == 0) {
        const int target = static_cast<int>(kArrivalBelow * static_cast<float>(world.config().zombies));
        const int missing = target - static_cast<int>(world.registry().view<const Zombie>().size());
        if (missing > 0) {
            spawnZombieGroup(world, std::min(missing, rng.rangeInt(kArrivalMin, kArrivalMax)), rng);
        }
    }
}

} // namespace lw
