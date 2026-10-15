#pragma once

#include "sim/Id.h"
#include "sim/Map.h"
#include "sim/Rng.h"
#include "sim/SpatialIndex.h"
#include "sim/TimeScale.h"
#include "sim/Weather.h"
#include "sim/nav/NavGraph.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"

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
    Behavior, // распорядок людей (до этапа 4.3 — блуждание тестовых существ)
    MapGen,
    Weather,
    Traits, // черты характера при рождении
    Count
};

struct WorldConfig {
    int mapWidth = 2048;  // клетки (по Map::kCellSize метров)
    int mapHeight = 2048; // клетки
    int people = 300;     // люди; живут в жилых домах
};

// Весь мир симуляции. Не знает о графике и умеет работать без окна.
// Один seed и одно число тиков дают одно и то же состояние.
class World {
public:
    // Масштаб времени — в sim/TimeScale.h.
    static constexpr int kTicksPerSecond = lw::kTicksPerSecond;
    static constexpr float kTickDt = lw::kTickDt;

    explicit World(std::uint64_t seed, const WorldConfig& config = {});
    // Индекс подписан на сигналы реестра по адресу: мир не копируется и не перемещается.
    World(const World&) = delete;
    World& operator=(const World&) = delete;

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
    // Время симуляции в секундах (не игровое: см. sim/TimeScale.h).
    double seconds() const { return static_cast<double>(tick_) / kTicksPerSecond; }
    const Map& map() const { return map_; }
    // Погода одна на весь мир. Дата, свет и температура — sim/Climate.h.
    const Weather& weather() const { return weather_; }
    Weather& weather() { return weather_; }
    // Размер мира в метрах.
    float width() const { return map_.worldWidth(); }
    float height() const { return map_.worldHeight(); }
    std::size_t entityCount() const { return idToEntity_.size(); }
    // Поиск соседей. Снимок позиций на начало тика: см. SpatialIndex.
    const SpatialIndex& spatial() const { return spatial_; }
    // Привести индекс к текущим позициям — после того, как сущности переставили
    // вне тика (тесты, будущий редактор). Тик делает это сам.
    void syncSpatial() { spatial_.sync(registry_); }
    // Проходимость клеток и граф навигации (дороги и местность) — производные
    // данные карты и зданий.
    const NavGrid& nav() const { return nav_; }
    const NavGraph& navGraph() const { return graph_; }
    // Поиск пути для систем симуляции. Ответ зависит только от карты, зданий и
    // запроса, поэтому состояние мира он не меняет и в хеш не входит.
    Pathfinder& pathfinder() { return pathfinder_; }

    // Хеш всего состояния, не зависящий от порядка хранения сущностей.
    // Основа проверок детерминизма (и, позже, сохранения/загрузки).
    std::uint64_t stateHash() const;

private:
    // Генерирует карту, создаёт сущности поселений и зданий, строит навигацию.
    void generate();

    std::uint64_t seed_;
    WorldConfig config_;
    std::uint64_t tick_ = 0;
    EntityId nextId_ = 1;

    Map map_;
    // Карта после генерации не меняется, поэтому её хеш считается один раз.
    std::uint64_t mapHash_ = 0;

    Weather weather_;

    NavGrid nav_;
    NavGraph graph_;
    Pathfinder pathfinder_{nav_, graph_};

    // Объявлен раньше реестра, чтобы пережить его: реестр шлёт индексу сигналы.
    SpatialIndex spatial_;
    entt::registry registry_;
    std::vector<Rng> streams_;
    // Только для поиска по id; обходить нельзя — порядок не детерминирован.
    std::unordered_map<EntityId, entt::entity> idToEntity_;
};

} // namespace lw
