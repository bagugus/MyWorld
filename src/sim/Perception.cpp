#include "sim/Perception.h"

#include "sim/Climate.h"
#include "sim/Map.h"
#include "sim/Weather.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Свет, при котором видно как днём: пасмурный день и ранние сумерки.
constexpr float kFullSightLight = 0.3f;
// Меньше этой доли дальности не бывает: человека в шаге видно и в темноте.
constexpr float kMinSightFactor = 0.08f;
constexpr float kFogSightLoss = 0.8f;
constexpr float kRainSightLoss = 0.3f;
constexpr float kRainHearingLoss = 0.6f;
constexpr float kWindHearingLoss = 0.03f; // на м/с
constexpr float kAsleepHearing = 0.3f;

// Медленнее — стоит на месте.
constexpr float kMovingSpeed = 0.05f; // м/с
constexpr float kWalkSpeed = 1.4f;    // м/с: шаги с громкостью kFootstepsLoudness

} // namespace

SenseConditions senseConditions(float light, const Weather& weather) {
    // Дальность растёт как свет в степени 3/4: в безлунную ночь — около 0,1
    // дневной, в полнолуние — около половины. Степень — через sqrt (детерминизм).
    const float t = std::clamp(light / kFullSightLight, 0.0f, 1.0f);
    const float root = std::sqrt(t);
    const float byLight = std::max(kMinSightFactor, root * std::sqrt(root));
    SenseConditions conditions;
    conditions.sight = byLight * (1.0f - kFogSightLoss * weather.fog) *
                       (1.0f - kRainSightLoss * weather.precipitation);
    conditions.hearing = 1.0f / (1.0f + kRainHearingLoss * weather.precipitation +
                                 kWindHearingLoss * weather.windSpeed);
    return conditions;
}

SenseConditions senseConditions(const World& world) {
    return senseConditions(environmentAt(world).light, world.weather());
}

bool isAsleep(const entt::registry& registry, entt::entity entity) {
    return SenseLookup(registry).asleep(entity);
}

float sightRange(const Senses& senses, bool asleep, const SenseConditions& conditions) {
    return asleep ? 0.0f : senses.sight * conditions.sight;
}

float hearingFactor(const Senses& senses, bool asleep, const SenseConditions& conditions) {
    return senses.hearing * conditions.hearing * (asleep ? kAsleepHearing : 1.0f);
}

float sightCost(const Map& map, const NavGrid& nav, float ax, float ay, float bx, float by,
                float limit) {
    const float dx = bx - ax;
    const float dy = by - ay;
    float cost = std::sqrt(dx * dx + dy * dy);
    if (!(cost <= limit)) {
        return kNoSight;
    }
    const Cell a = Map::cellAt(ax, ay);
    const Cell b = Map::cellAt(bx, by);
    const bool open = forEachCellOnSegment(a, b, [&](int x, int y) {
        if ((x == a.x && y == a.y) || (x == b.x && y == b.y)) {
            return true;
        }
        if (!nav.contains(x, y)) {
            return false;
        }
        // Непроходимая суша — здание; море и озеро обзор не закрывают.
        if (nav.cost(x, y) == NavGrid::kBlocked && map.water.at(x, y) == Water::None) {
            return false;
        }
        if (map.cover.at(x, y) == Cover::Forest) {
            cost += kForestSightPenalty;
            return cost <= limit;
        }
        return true;
    });
    return open ? cost : kNoSight;
}

float bodyVisibility(const SenseLookup& lookup, entt::entity target) {
    if (lookup.asleep(target)) {
        return kLyingVisibility;
    }
    const Velocity* velocity = lookup.velocity(target);
    const bool moving = velocity != nullptr && velocity->x * velocity->x + velocity->y * velocity->y >=
                                                   kMovingSpeed * kMovingSpeed;
    return moving ? 1.0f : kStillVisibility;
}

float coverVisibility(const Map& map, float x, float y) {
    const Cell cell = Map::cellAt(x, y);
    const bool forest =
        map.cover.contains(cell.x, cell.y) && map.cover.at(cell.x, cell.y) == Cover::Forest;
    return forest ? kForestVisibility : 1.0f;
}

float visibilityOf(const World& world, entt::entity target, float x, float y) {
    return bodyVisibility(SenseLookup(world.registry()), target) * coverVisibility(world.map(), x, y);
}

Noise noiseOf(const SenseLookup& lookup, entt::entity entity) {
    Noise noise;
    const auto louder = [&noise](SoundKind kind, float loudness) {
        if (loudness > noise.loudness) {
            noise = {kind, loudness};
        }
    };
    if (const Velocity* velocity = lookup.velocity(entity)) {
        const float speedSq = velocity->x * velocity->x + velocity->y * velocity->y;
        if (speedSq >= kMovingSpeed * kMovingSpeed) {
            louder(SoundKind::Footsteps,
                   std::min(kMaxNoiseLoudness, kFootstepsLoudness * std::sqrt(speedSq) / kWalkSpeed));
        }
    }
    if (const Mind* mind = lookup.mind(entity);
        mind != nullptr && mind->phase == ActionPhase::Perform) {
        if (mind->action == Action::Scavenge) {
            louder(SoundKind::Rummage, kRummageLoudness);
        } else if (mind->action == Action::Socialize) {
            louder(SoundKind::Voice, kVoiceLoudness);
        }
    }
    return noise;
}

Noise noiseOf(const entt::registry& registry, entt::entity entity) {
    return noiseOf(SenseLookup(registry), entity);
}

} // namespace lw
