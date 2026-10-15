#include "sim/systems/WanderSystem.h"

#include "sim/Components.h"
#include "sim/World.h"

#include <cmath>

namespace lw {

void spawnWanderers(World& world, int count) {
    Rng& rng = world.rng(RngStream::Spawn);
    entt::registry& registry = world.registry();

    for (int i = 0; i < count; ++i) {
        const entt::entity entity = world.create();
        const float x = rng.range(0.0f, world.width());
        const float y = rng.range(0.0f, world.height());
        registry.emplace<Position>(entity, x, y);
        registry.emplace<Velocity>(entity);
        registry.emplace<Wander>(entity, 0, rng.range(0.8f, 2.0f));
    }
}

void updateWander(World& world) {
    Rng& rng = world.rng(RngStream::Wander);
    const float dt = World::kTickDt;
    const float width = world.width();
    const float height = world.height();

    auto view = world.registry().view<Position, Velocity, Wander>();
    for (const auto [entity, position, velocity, wander] : view.each()) {
        if (--wander.ticksUntilTurn <= 0) {
            // Случайное направление: точка в единичном круге, без sin/cos —
            // только арифметика и sqrt, результат одинаков на любой платформе.
            float dx = 0.0f;
            float dy = 0.0f;
            float lengthSq = 0.0f;
            do {
                dx = rng.range(-1.0f, 1.0f);
                dy = rng.range(-1.0f, 1.0f);
                lengthSq = dx * dx + dy * dy;
            } while (lengthSq > 1.0f || lengthSq < 1.0e-4f);

            const float scale = wander.speed / std::sqrt(lengthSq);
            velocity.x = dx * scale;
            velocity.y = dy * scale;
            wander.ticksUntilTurn =
                rng.rangeInt(2 * World::kTicksPerSecond, 10 * World::kTicksPerSecond);
        }

        position.x += velocity.x * dt;
        position.y += velocity.y * dt;

        // Отражение от границ мира.
        if (position.x < 0.0f) {
            position.x = 0.0f;
            velocity.x = -velocity.x;
        } else if (position.x > width) {
            position.x = width;
            velocity.x = -velocity.x;
        }
        if (position.y < 0.0f) {
            position.y = 0.0f;
            velocity.y = -velocity.y;
        } else if (position.y > height) {
            position.y = height;
            velocity.y = -velocity.y;
        }
    }
}

} // namespace lw
