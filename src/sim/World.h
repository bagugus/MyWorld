#pragma once

#include "sim/Id.h"
#include "sim/Map.h"
#include "sim/Rng.h"

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lw {

// Независимые потоки случайных чисел. Новые добавлять в конец (перед Count):
// порядок определяет последовательности уже существующих потоков.
enum class RngStream : std::uint8_t {
    Spawn,
    Wander,
    MapGen,
    Count
};

struct WorldConfig {
    int mapWidth = 2048;  // клетки (по Map::kCellSize метров)
    int mapHeight = 2048; // клетки
    int wanderers = 300;  // тестовые существа этапа 1
};

// Весь мир симуляции. Не знает о графике и умеет работать без окна.
// Один seed и одно число тиков дают одно и то же состояние.
class World {
public:
    static constexpr int kTicksPerSecond = 20;
    static constexpr float kTickDt = 1.0f / static_cast<float>(kTicksPerSecond);

    explicit World(std::uint64_t seed, const WorldConfig& config = {});

    // Продвигает мир ровно на один тик.
    void step();

    // Сущности создавать и удалять только здесь — так у каждой есть стабильный Id.
    entt::entity create();
    void destroy(entt::entity entity);
    // entt::null, если объекта с таким id (уже) нет.
    entt::entity find(EntityId id) const;

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }
    Rng& rng(RngStream stream) { return streams_[static_cast<std::size_t>(stream)]; }

    std::uint64_t seed() const { return seed_; }
    std::uint64_t tick() const { return tick_; }
    double seconds() const { return static_cast<double>(tick_) / kTicksPerSecond; }
    const Map& map() const { return map_; }
    // Размер мира в метрах.
    float width() const { return map_.worldWidth(); }
    float height() const { return map_.worldHeight(); }
    std::size_t entityCount() const { return idToEntity_.size(); }

    // Хеш всего состояния, не зависящий от порядка хранения сущностей.
    // Основа проверок детерминизма (и, позже, сохранения/загрузки).
    std::uint64_t stateHash() const;

private:
    // Генерирует карту и создаёт сущности поселений и зданий.
    void generate();

    std::uint64_t seed_;
    WorldConfig config_;
    std::uint64_t tick_ = 0;
    EntityId nextId_ = 1;

    Map map_;
    // Карта после генерации не меняется, поэтому её хеш считается один раз.
    std::uint64_t mapHash_ = 0;

    entt::registry registry_;
    std::vector<Rng> streams_;
    // Только для поиска по id; обходить нельзя — порядок не детерминирован.
    std::unordered_map<EntityId, entt::entity> idToEntity_;
};

} // namespace lw
