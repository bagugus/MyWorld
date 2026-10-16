#include "render/Palette.h"

#include "sim/Hash.h"

namespace lw {

namespace {

constexpr Color kEntityColor = {236, 200, 92, 255};   // в пути
constexpr Color kRestingColor = {150, 205, 225, 255};  // на месте
constexpr Color kSleepingColor = {70, 84, 140, 255};
constexpr Color kEatingColor = {120, 210, 110, 255};   // ест, пьёт
constexpr Color kSearchingColor = {232, 132, 64, 255}; // ищет припасы, собирает в лесу
constexpr Color kSocialColor = {226, 128, 196, 255};
constexpr Color kWorkingColor = {104, 140, 230, 255}; // на работе (этап 7.2)
constexpr Color kFleeingColor = {255, 255, 255, 255};
constexpr Color kAvengingColor = {190, 30, 40, 255}; // идёт мстить (этап 6.5)
constexpr Color kDeerColor = {150, 98, 52, 255};
constexpr Color kBoarColor = {86, 70, 60, 255};
constexpr Color kHareColor = {176, 166, 150, 255};
constexpr Color kHumanBodyColor = {110, 40, 36, 255};
constexpr Color kZombieBodyColor = {52, 62, 36, 255};
constexpr Color kCarcassColor = {84, 56, 36, 255};

} // namespace

Color buildingColor(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return {198, 164, 124, 255};
    case BuildingKind::Barn:
        return {142, 104, 74, 255};
    case BuildingKind::Apartment:
        return {176, 178, 186, 255};
    case BuildingKind::Shop:
        return {204, 132, 92, 255};
    case BuildingKind::Hospital:
        return {226, 226, 220, 255};
    case BuildingKind::PoliceStation:
        return {96, 120, 168, 255};
    case BuildingKind::School:
        return {214, 188, 96, 255};
    }
    return {198, 164, 124, 255};
}

Color corpseColor(CorpseKind kind) {
    switch (kind) {
    case CorpseKind::Human:
        return kHumanBodyColor;
    case CorpseKind::Zombie:
        return kZombieBodyColor;
    default:
        return kCarcassColor;
    }
}

Color creatureColor(const entt::registry& registry, entt::entity entity) {
    if (const auto* animal = registry.try_get<Animal>(entity)) {
        Color color = animal->kind == AnimalKind::Deer   ? kDeerColor
                      : animal->kind == AnimalKind::Boar ? kBoarColor
                                                         : kHareColor;
        if (animal->state == AnimalState::Flee) {
            color = ColorBrightness(color, 0.35f);
        }
        return color;
    }
    if (const auto* zombie = registry.try_get<Zombie>(entity)) {
        switch (zombie->state) {
        case ZombieState::Chase:
            return kZombieChaseColor;
        case ZombieState::Investigate:
            return kZombieAlertColor;
        default:
            return kZombieColor;
        }
    }
    const auto* mind = registry.try_get<Mind>(entity);
    if (mind != nullptr && mind->action == Action::Flee) {
        return kFleeingColor;
    }
    if (mind != nullptr && mind->action == Action::Avenge) {
        return kAvengingColor;
    }
    const auto* walker = registry.try_get<Walker>(entity);
    if (walker == nullptr || walker->moving()) {
        return kEntityColor;
    }
    if (mind == nullptr || mind->phase == ActionPhase::Travel) {
        return kRestingColor;
    }
    switch (mind->action) {
    case Action::Sleep:
        return kSleepingColor;
    case Action::Eat:
    case Action::Drink:
        return kEatingColor;
    case Action::Scavenge:
    case Action::Forage:
    case Action::Steal:
    case Action::Salvage:
        return kSearchingColor;
    case Action::Socialize:
    case Action::Share:
    case Action::Trade:
    case Action::Buy:
        return kSocialColor;
    case Action::Work:
        return kWorkingColor;
    default:
        return kRestingColor;
    }
}

Vector2 birdOffset(EntityId flock, int bird) {
    Hasher hasher;
    hasher.add(flock);
    hasher.add(bird);
    // У FNV младшие биты соседних bird похожи: перемешать, иначе птицы встают в линию.
    std::uint64_t bits = hasher.value();
    bits ^= bits >> 33;
    bits *= 0xFF51AFD7ED558CCDULL;
    bits ^= bits >> 33;
    return {static_cast<float>(bits & 0xFFFu) / 4095.0f * 2.0f - 1.0f,
            static_cast<float>((bits >> 12) & 0xFFFu) / 4095.0f * 2.0f - 1.0f};
}

} // namespace lw
