#include "sim/systems/MovementSystem.h"

#include "sim/Components.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"

#include <cmath>
#include <cstdlib>
#include <utility>

namespace lw {

namespace {

// Скорость в воде относительно дороги. Стоимость воды в NavGrid включает штраф
// за холод и мокрые вещи, поэтому скорость по ней берётся не из стоимости.
constexpr float kFordFactor = 0.5f;
constexpr float kWadeFactor = 0.35f;
constexpr float kSwimFactor = 0.25f;
// Граница «по грудь» и «вплавь» в стоимостях NavGrid (брод — kWaterCost).
constexpr std::uint8_t kWadeCostMax = 2 * NavGrid::kWaterCost;

} // namespace

float walkSpeedFactor(std::uint8_t cost) {
    if (cost == NavGrid::kBlocked) {
        return 0.0f;
    }
    if (cost < NavGrid::kWaterCost) {
        return static_cast<float>(NavGrid::kRoadCost) / static_cast<float>(cost);
    }
    return cost == NavGrid::kWaterCost ? kFordFactor
           : cost <= kWadeCostMax      ? kWadeFactor
                                       : kSwimFactor;
}

bool startWalk(World& world, entt::entity entity, Cell goal) {
    entt::registry& registry = world.registry();
    const Position& position = registry.get<Position>(entity);
    Walker& walker = registry.get<Walker>(entity);
    walker.waypoints.clear();
    walker.next = 0;

    const Cell start = Map::cellAt(position.x, position.y);
    Path path = world.pathfinder().find(start, goal);
    if (!path.found()) {
        return false;
    }
    // Первая точка — своя клетка: сначала к её центру, ведь прямые между точками
    // пути проверены от центров клеток.
    walker.waypoints = std::move(path.waypoints);
    return true;
}

bool startWalkDirect(World& world, entt::entity entity, Cell goal) {
    entt::registry& registry = world.registry();
    const Position& position = registry.get<Position>(entity);
    const Cell start = Map::cellAt(position.x, position.y);
    const NavGrid& nav = world.nav();
    if (std::abs(goal.x - start.x) <= kDirectWalkCells &&
        std::abs(goal.y - start.y) <= kDirectWalkCells && nav.passable(start) &&
        nav.segmentPassable(start, goal)) {
        Walker& walker = registry.get<Walker>(entity);
        walker.waypoints.clear();
        walker.waypoints.push_back(start);
        if (goal != start) {
            walker.waypoints.push_back(goal);
        }
        walker.next = 0;
        return true;
    }
    return startWalk(world, entity, goal);
}

void updateMovement(World& world, float dt) {
    const NavGrid& nav = world.nav();
    auto view = world.registry().view<Position, Velocity, Walker>();
    for (const auto [entity, position, velocity, walker] : view.each()) {
        if (!walker.moving()) {
            velocity = {};
            continue;
        }
        const Position before = position;
        float remaining = dt;
        while (remaining > 0.0f && walker.moving()) {
            const Cell target = walker.waypoints[walker.next];
            const float tx = Map::cellCenter(target.x);
            const float ty = Map::cellCenter(target.y);
            const float dx = tx - position.x;
            const float dy = ty - position.y;
            const float distance = std::sqrt(dx * dx + dy * dy);

            // Скорость — по клетке, где сущность сейчас; сойти с непроходимой
            // клетки (не бывает, но на всякий случай) можно со скоростью вплавь.
            float factor = walkSpeedFactor(nav.cost(Map::cellAt(position.x, position.y)));
            if (factor <= 0.0f) {
                factor = kSwimFactor;
            }
            const float speed = walker.speed * walker.pace * factor;
            const float step = speed * remaining;
            if (step >= distance) {
                position = {tx, ty};
                remaining -= distance / speed;
                ++walker.next;
            } else {
                position.x += dx / distance * step;
                position.y += dy / distance * step;
                remaining = 0.0f;
            }
        }
        if (!walker.moving()) {
            walker.waypoints.clear();
            walker.next = 0;
        }
        velocity = {(position.x - before.x) / dt, (position.y - before.y) / dt};
    }
}

} // namespace lw
