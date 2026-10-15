#include "sim/systems/AnimalSystem.h"

#include "sim/Perception.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"
#include "sim/systems/MovementSystem.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace lw {

namespace {

// --- Животные ------------------------------------------------------------

// Пасётся на месте столько секунд симуляции, затем с вероятностью kRoamChance
// переходит к соседнему месту (до kRoamStep метров), а ушёл от родных мест
// дальше kHomeRange — возвращается к ним.
constexpr int kGrazeMinSeconds = 20;
constexpr int kGrazeMaxSeconds = 90;
constexpr float kRoamChance = 0.4f;
constexpr float kRoamStep = 30.0f;
constexpr float kHomeRange = 120.0f;
constexpr int kRoamAttempts = 6;
// Убежав, стоит настороже столько секунд.
constexpr int kAlertMinSeconds = 10;
constexpr int kAlertMaxSeconds = 30;
// Раненое животное помнит обидчика столько и бежит от него, даже не видя.
constexpr std::uint64_t kHurtMemoryTicks = 10 * kTicksPerSecond;
// Кабан бросается на человека, который подошёл ближе kBoarCornered метров или
// ранил его, с вероятностью kBoarChargeChance; бросок длится до kChargeSeconds.
constexpr float kBoarCornered = 6.0f;
constexpr float kBoarChargeChance = 0.5f;
constexpr int kChargeSeconds = 8;
constexpr float kChargeReach = 1.5f;
// Направления бегства: прямо прочь, затем на 40° и 80° в стороны (cos, sin).
constexpr float kFleeTurns[][2] = {
    {1.0f, 0.0f}, {0.766f, 0.643f}, {0.766f, -0.643f}, {0.174f, 0.985f}, {0.174f, -0.985f}};

constexpr AnimalSpec kSpecs[] = {
    // бег, шаг, зрение, слух, бежит от увиденного, услышанного, на сколько, лес
    {7.0f, 0.7f, 120.0f, 1.5f, 70.0f, 40.0f, 90.0f, true},  // олень
    {5.0f, 0.6f, 50.0f, 1.3f, 30.0f, 18.0f, 70.0f, true},   // кабан
    {6.5f, 0.5f, 60.0f, 1.8f, 20.0f, 12.0f, 50.0f, false},  // заяц
};
static_assert(std::size(kSpecs) == static_cast<std::size_t>(AnimalKind::Count));

// --- Птицы ---------------------------------------------------------------

// Взлетает, если кто-то идёт ближе kFlockStartle метров или стоит ближе kFlockTooClose.
constexpr float kFlockStartle = 20.0f;
constexpr float kFlockTooClose = 8.0f;
constexpr float kMovingSpeed = 0.05f; // м/с: медленнее — стоит
// Крик вспугнутой стаи слышен в стольких метрах; перелёт по своей воле — тише.
constexpr float kAlarmLoudness = 100.0f;
constexpr float kCalmLoudness = 30.0f;
constexpr float kFlightSpeed = 10.0f; // м/с
constexpr float kHopMin = 300.0f;      // перелёт, метры
constexpr float kHopMax = 800.0f;
constexpr int kHopAttempts = 8;
// Перелетая сама, стая садится у ближайшего тела не дальше kCarrionRange (этап 5.5):
// вороны над полем выдают, где кто-то погиб.
constexpr float kCarrionRange = 1500.0f;
constexpr float kCarrionOffset = 4.0f;
constexpr int kPerchMinMinutes = 60;   // сидит на месте, игровые минуты
constexpr int kPerchMaxMinutes = 240;

std::uint64_t seconds(int count) {
    return static_cast<std::uint64_t>(count) * kTicksPerSecond;
}

std::uint64_t gameMinutes(int count) {
    return static_cast<std::uint64_t>(count) * kTicksPerGameMinute;
}

float distance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

void setSpeed(Walker& walker, float metersPerSecond) {
    walker.pace = std::max(0.05f, metersPerSecond / walker.speed);
}

// Клетка, где животное может стоять: проходимая суша без воды.
bool dryLand(const World& world, Cell cell) {
    const Map& map = world.map();
    return world.nav().passable(cell) && map.water.at(cell.x, cell.y) == Water::None;
}

// Привычные места вида: лес для оленя и кабана, открытое место для зайца.
bool habitat(const World& world, const AnimalSpec& spec, Cell cell) {
    const bool forest = world.map().cover.at(cell.x, cell.y) == Cover::Forest;
    return dryLand(world, cell) && forest == spec.forest;
}

class AnimalBrain {
public:
    AnimalBrain(World& world, Rng& rng)
        : world_(world), registry_(world.registry()), rng_(rng), tick_(world.tick()) {}

    void think(entt::entity entity, Animal& animal, Walker& walker, const Position& position,
               const Awareness& awareness, Combat* combat) {
        entity_ = entity;
        animal_ = &animal;
        walker_ = &walker;
        position_ = position;
        combat_ = combat;
        if (combat != nullptr) {
            combat->target = kInvalidId; // бьёт, только пока бросается
        }
        const AnimalSpec& spec = animalSpec(animal.kind);

        if (animal.state == AnimalState::Charge && charge(spec)) {
            return;
        }

        // Кого боится и куда бежать: прочь от всех, сильнее — от ближних.
        EntityId threat = kInvalidId;
        float threatDistance = 0.0f;
        float awayX = 0.0f;
        float awayY = 0.0f;
        // Ранен недавно — обидчик угроза, даже если его не видно.
        if (animal.hurtAt > 0 && tick_ < animal.hurtAt + kHurtMemoryTicks) {
            const entt::entity other = world_.find(animal.hurtBy);
            if (other != entt::null && registry_.any_of<Person, Zombie>(other)) {
                const Position& at = registry_.get<Position>(other);
                threat = animal.hurtBy;
                threatDistance = distance(position.x, position.y, at.x, at.y);
                const float weight = 1.0f / std::max(threatDistance, 0.5f);
                awayX += (position.x - at.x) * weight;
                awayY += (position.y - at.y) * weight;
            }
        }
        for (std::size_t i = 0; i < awareness.count; ++i) {
            const Contact& contact = awareness.contacts[i];
            if (!isFresh(contact, tick_)) {
                continue;
            }
            const entt::entity other = world_.find(contact.id);
            if (other == entt::null || !registry_.any_of<Person, Zombie>(other)) {
                continue;
            }
            const float d = distance(position.x, position.y, contact.x, contact.y);
            const float limit = contact.seen ? spec.fleeSeen : spec.fleeHeard;
            if (d > limit) {
                continue;
            }
            if (threat == kInvalidId) {
                threat = contact.id; // контакты — угрозы и ближние вперёд
                threatDistance = d;
            }
            const float weight = 1.0f / std::max(d, 0.5f);
            awayX += (position.x - contact.x) * weight;
            awayY += (position.y - contact.y) * weight;
        }

        if (threat != kInvalidId) {
            // Бежит — пусть добежит; добежал, а угроза рядом, — дальше.
            if (animal.state != AnimalState::Flee || !walker.moving()) {
                // Кабан, которого загнали или ранили, может броситься на человека.
                const entt::entity other = world_.find(threat);
                const bool cornered = threatDistance <= kBoarCornered || animal.hurtBy == threat;
                if (combat != nullptr && animal.kind == AnimalKind::Boar && cornered &&
                    other != entt::null && registry_.all_of<Person>(other) &&
                    rng_.chance(kBoarChargeChance)) {
                    animal.state = AnimalState::Charge;
                    animal.threat = threat;
                    animal.until = tick_ + seconds(kChargeSeconds);
                    if (charge(spec)) {
                        return;
                    }
                }
                flee(spec, threat, awayX, awayY);
            }
            return;
        }
        if (walker.moving()) {
            return;
        }
        if (animal.state == AnimalState::Flee) {
            graze(spec, rng_.rangeInt(kAlertMinSeconds, kAlertMaxSeconds));
            return;
        }
        if (animal.state == AnimalState::Roam) {
            graze(spec, rng_.rangeInt(kGrazeMinSeconds, kGrazeMaxSeconds));
            return;
        }
        if (tick_ < animal.until) {
            return;
        }
        if (rng_.chance(kRoamChance)) {
            if (const auto cell = pickRoam(spec)) {
                animal.state = AnimalState::Roam;
                animal.goal = *cell;
                setSpeed(walker, spec.walkSpeed);
                if (startWalkDirect(world_, entity_, *cell)) {
                    return;
                }
            }
        }
        graze(spec, rng_.rangeInt(kGrazeMinSeconds, kGrazeMaxSeconds));
    }

private:
    void stop() {
        walker_->waypoints.clear();
        walker_->next = 0;
    }

    // Бросок на обидчика: бежит к нему и бьёт, пока не выйдет время. false —
    // бросок кончился (обидчика нет или время вышло).
    bool charge(const AnimalSpec& spec) {
        const entt::entity target = world_.find(animal_->threat);
        if (combat_ == nullptr || target == entt::null || !registry_.all_of<Person>(target) ||
            tick_ >= animal_->until) {
            animal_->state = AnimalState::Flee;
            stop();
            return false;
        }
        const Position& at = registry_.get<Position>(target);
        setSpeed(*walker_, spec.runSpeed);
        combat_->target = animal_->threat;
        if (distance(position_.x, position_.y, at.x, at.y) <= kChargeReach) {
            stop();
            return true;
        }
        const Cell goal = Map::cellAt(at.x, at.y);
        animal_->goal = goal;
        if (!startWalkDirect(world_, entity_, goal)) {
            animal_->state = AnimalState::Flee;
            combat_->target = kInvalidId;
            return false;
        }
        return true;
    }

    void graze(const AnimalSpec& spec, int grazeSeconds) {
        animal_->state = AnimalState::Graze;
        animal_->threat = kInvalidId;
        animal_->until = tick_ + seconds(grazeSeconds);
        setSpeed(*walker_, spec.walkSpeed);
        stop();
    }

    void flee(const AnimalSpec& spec, EntityId threat, float awayX, float awayY) {
        const float length = std::sqrt(awayX * awayX + awayY * awayY);
        if (length > 1e-4f) {
            awayX /= length;
            awayY /= length;
        } else {
            awayX = 0.0f;
            awayY = -1.0f; // угроза ровно там же: куда угодно, лишь бы прочь
        }
        const NavGrid& nav = world_.nav();
        const Cell from = Map::cellAt(position_.x, position_.y);
        for (const auto& turn : kFleeTurns) {
            const float dx = awayX * turn[0] - awayY * turn[1];
            const float dy = awayX * turn[1] + awayY * turn[0];
            const auto cell =
                nav.nearestPassable(Map::cellAt(position_.x + dx * spec.fleeDistance,
                                                position_.y + dy * spec.fleeDistance),
                                    3);
            if (!cell || *cell == from || !nav.connected(from, *cell)) {
                continue;
            }
            if (startWalkDirect(world_, entity_, *cell)) {
                animal_->state = AnimalState::Flee;
                animal_->threat = threat;
                animal_->goal = *cell;
                setSpeed(*walker_, spec.runSpeed);
                return;
            }
        }
        // Бежать некуда — замереть.
        graze(spec, kAlertMinSeconds);
    }

    // Соседнее место у родных мест, лучше — привычное для вида.
    std::optional<Cell> pickRoam(const AnimalSpec& spec) {
        const float ax = Map::cellCenter(animal_->anchor.x);
        const float ay = Map::cellCenter(animal_->anchor.y);
        const bool away = distance(position_.x, position_.y, ax, ay) > kHomeRange;
        const float cx = away ? ax : position_.x;
        const float cy = away ? ay : position_.y;
        const NavGrid& nav = world_.nav();
        const Cell from = Map::cellAt(position_.x, position_.y);
        std::optional<Cell> fallback;
        for (int attempt = 0; attempt < kRoamAttempts; ++attempt) {
            const Cell cell = Map::cellAt(cx + rng_.range(-kRoamStep, kRoamStep),
                                          cy + rng_.range(-kRoamStep, kRoamStep));
            if (!nav.contains(cell.x, cell.y) || !dryLand(world_, cell) ||
                !nav.connected(from, cell)) {
                continue;
            }
            if (habitat(world_, spec, cell)) {
                return cell;
            }
            if (!fallback) {
                fallback = cell;
            }
        }
        return fallback;
    }

    World& world_;
    entt::registry& registry_;
    Rng& rng_;
    std::uint64_t tick_;
    entt::entity entity_ = entt::null;
    Animal* animal_ = nullptr;
    Walker* walker_ = nullptr;
    Combat* combat_ = nullptr;
    Position position_;
};

// --- Птицы ---------------------------------------------------------------

// Кто-то идёт рядом или стоит вплотную — стая взлетает.
bool startled(const World& world, const Position& position) {
    const entt::registry& registry = world.registry();
    bool scared = false;
    world.spatial().forEachInRadius(position.x, position.y, kFlockStartle, [&](const SpatialHit& hit) {
        if (scared || !registry.all_of<Senses>(hit.entity)) {
            return;
        }
        if (hit.distanceSq <= kFlockTooClose * kFlockTooClose) {
            scared = true;
            return;
        }
        const auto* velocity = registry.try_get<Velocity>(hit.entity);
        scared = velocity != nullptr &&
                 velocity->x * velocity->x + velocity->y * velocity->y >= kMovingSpeed * kMovingSpeed;
    });
    return scared;
}

// Куда перелететь: точка суши в kHopMin..kHopMax метрах.
std::optional<std::pair<float, float>> pickPerch(const World& world, const Position& position,
                                                 Rng& rng) {
    for (int attempt = 0; attempt < kHopAttempts; ++attempt) {
        float dx = rng.range(-1.0f, 1.0f);
        float dy = rng.range(-1.0f, 1.0f);
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.1f || length > 1.0f) {
            continue;
        }
        const float hop = rng.range(kHopMin, kHopMax);
        const float x = position.x + dx / length * hop;
        const float y = position.y + dy / length * hop;
        const Cell cell = Map::cellAt(x, y);
        if (world.map().contains(cell) && dryLand(world, cell)) {
            return std::make_pair(Map::cellCenter(cell.x), Map::cellCenter(cell.y));
        }
    }
    return std::nullopt;
}

void updateFlocks(World& world, Rng& rng) {
    const std::uint64_t tick = world.tick();
    auto view = world.registry().view<const Id, Flock, Position, Velocity>();
    for (const auto [entity, id, flock, position, velocity] : view.each()) {
        if (flock.flying) {
            const Position before = position;
            const float d = distance(position.x, position.y, flock.toX, flock.toY);
            const float step = kFlightSpeed * kTickDt;
            if (d <= step) {
                position = {flock.toX, flock.toY};
                flock.flying = false;
                flock.until =
                    tick + gameMinutes(rng.rangeInt(kPerchMinMinutes, kPerchMaxMinutes));
            } else {
                position.x += (flock.toX - position.x) / d * step;
                position.y += (flock.toY - position.y) / d * step;
            }
            velocity = {(position.x - before.x) / kTickDt, (position.y - before.y) / kTickDt};
            continue;
        }
        velocity = {};
        if ((tick + id.value) % kAnimalThinkTicks != 0) {
            continue;
        }
        const bool alarm = startled(world, position);
        if (!alarm && tick < flock.until) {
            continue;
        }
        std::optional<std::pair<float, float>> perch;
        if (!alarm) {
            const entt::registry& registry = world.registry();
            if (const auto carrion = world.spatial().nearest(
                    position.x, position.y, kCarrionRange,
                    [&](const SpatialHit& hit) { return registry.all_of<Corpse>(hit.entity); });
                carrion && carrion->distanceSq > kFlockStartle * kFlockStartle) {
                perch = std::make_pair(carrion->x + rng.range(-kCarrionOffset, kCarrionOffset),
                                       carrion->y + rng.range(-kCarrionOffset, kCarrionOffset));
            }
        }
        if (!perch) {
            perch = pickPerch(world, position, rng);
        }
        if (perch) {
            flock.flying = true;
            flock.toX = perch->first;
            flock.toY = perch->second;
            world.emitSound(position.x, position.y, alarm ? kAlarmLoudness : kCalmLoudness,
                            SoundKind::Birds, id.value);
        } else {
            flock.until = tick + gameMinutes(kPerchMinMinutes);
        }
    }
}

} // namespace

const AnimalSpec& animalSpec(AnimalKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return kSpecs[index < std::size(kSpecs) ? index : 0];
}

entt::entity createAnimal(World& world, AnimalKind kind, Cell cell, Rng& rng) {
    entt::registry& registry = world.registry();
    const AnimalSpec& spec = animalSpec(kind);
    const entt::entity entity = world.create();
    const float jitter = Map::kCellSize * 0.35f;
    registry.emplace<Position>(entity, Map::cellCenter(cell.x) + rng.range(-jitter, jitter),
                               Map::cellCenter(cell.y) + rng.range(-jitter, jitter));
    registry.emplace<Velocity>(entity);
    Walker walker;
    walker.speed = spec.runSpeed;
    setSpeed(walker, spec.walkSpeed);
    registry.emplace<Walker>(entity, std::move(walker));
    registry.emplace<Senses>(entity, Senses{spec.sight, spec.hearing});
    registry.emplace<Awareness>(entity);
    if (kind == AnimalKind::Boar) {
        registry.emplace<Combat>(entity); // бросается на обидчика
    }
    Animal animal;
    animal.kind = kind;
    animal.anchor = cell;
    animal.goal = cell;
    animal.until = world.tick() + seconds(rng.rangeInt(0, kGrazeMaxSeconds));
    registry.emplace<Animal>(entity, animal);
    return entity;
}

entt::entity createFlock(World& world, Cell cell, int birds, Rng& rng) {
    entt::registry& registry = world.registry();
    const entt::entity entity = world.create();
    registry.emplace<Position>(entity, Map::cellCenter(cell.x), Map::cellCenter(cell.y));
    registry.emplace<Velocity>(entity);
    Flock flock;
    flock.birds = static_cast<std::uint8_t>(std::clamp(birds, 1, 255));
    flock.until = world.tick() + gameMinutes(rng.rangeInt(0, kPerchMaxMinutes));
    registry.emplace<Flock>(entity, flock);
    return entity;
}

void updateAnimals(World& world) {
    const std::uint64_t tick = world.tick();
    Rng& rng = world.rng(RngStream::Animals);
    AnimalBrain brain(world, rng);
    // Порядок — порядок хранения в реестре: он детерминирован. Компоненты по
    // ходу не добавляются и не удаляются.
    entt::registry& registry = world.registry();
    auto view = registry.view<const Id, Animal, Walker, const Position, const Awareness>();
    for (const auto [entity, id, animal, walker, position, awareness] : view.each()) {
        // Раненое решает сразу, а не в свой тик: иначе секунду стоит под выстрелами.
        const bool hurt = animal.hurtAt > 0 && animal.hurtAt + 1 == tick; // ранили в прошлом тике
        if ((tick + id.value) % kAnimalThinkTicks == 0 || (hurt && animal.state != AnimalState::Flee)) {
            brain.think(entity, animal, walker, position, awareness, registry.try_get<Combat>(entity));
        }
    }
    updateFlocks(world, rng);
}

} // namespace lw
