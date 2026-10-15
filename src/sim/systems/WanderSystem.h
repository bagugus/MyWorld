#pragma once

#include <cstdint>

namespace lw {

class World;

// ВРЕМЕННО (этап 1): тестовые существа, бесцельно блуждающие по миру.
// Нужны, чтобы было видно тик, ускорение времени и камеру.
// Удаляется на этапе 4, когда появятся люди с нуждами и Utility AI.

struct Wander {
    std::int32_t ticksUntilTurn = 0;
    float speed = 1.4f; // м/с
};

void spawnWanderers(World& world, int count);
void updateWander(World& world);

} // namespace lw
