#pragma once

#include "sim/Chronicle.h"
#include "sim/Id.h"
#include "sim/Lod.h"
#include "sim/Map.h"
#include "sim/Places.h"
#include "sim/Profiler.h"
#include "sim/Resources.h"
#include "sim/Rng.h"
#include "sim/Sound.h"
#include "sim/SpatialIndex.h"
#include "sim/TimeScale.h"
#include "sim/Weather.h"
#include "sim/nav/NavGraph.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"

#include <entt/entt.hpp>

#include <cstddef>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lw {

// Независимые потоки случайных чисел. Новые добавлять в конец (перед Count):
// порядок определяет последовательности уже существующих потоков.
enum class RngStream : std::uint8_t {
    Spawn,
    Behavior, // решения людей (Utility AI); до этапа 4.3 — блуждание тестовых существ
    MapGen,
    Weather,
    Traits,   // черты характера при рождении
    Supplies, // припасы в зданиях
    Zombies,  // зомби: расселение, решения, орды
    Animals,  // животные и птицы: расселение, решения
    Combat,   // бой: оружие при рождении, попадания и урон
    Events,   // ивенты: когда и где, что в грузе, кто заметил знак (этап 7.1)
    Names,    // имена людей при рождении
    Jobs,     // профессии и сбережения при рождении (этап 7.2)
    Scenario, // сценарий: первые заражённые в начале вспышки (этап 7.4)
    Families, // семьи при рождении мира: состав домов, возраст, родня, прошлое (этап 7.3)
    Outbreak, // заражение от больных и тел: кто заразился рядом с ними (этап 7.5)
    Society,  // реакция общества: кто услышал передачу, солдаты (этап 7.6)
    Count
};

struct WorldConfig {
    int mapWidth = 2048;  // клетки (по Map::kCellSize метров)
    int mapHeight = 2048; // клетки
    int people = 300;     // люди; живут в жилых домах
    // Зомби при создании мира: одиночки в поселениях и на природе, орды. Мир
    // начинается с мирной жизни (этап 7.2), поэтому по умолчанию их нет; от
    // этого же числа считается приток издалека (этап 5.7).
    int zombies = 0;
    int animals = 160;    // олени, кабаны, зайцы — вдали от поселений
    int flocks = 24;      // стаи ворон
    // Ивенты по временному расписанию (до рассказчика, этап 7.9). В мирное
    // время грузы с неба не падают, поэтому по умолчанию выключено. Вручную
    // (интерфейс, тесты) ивент можно начать и без него: startEvent.
    bool events = false;
    // Ивенты по фазам сценария (этап 7.7, PhaseRules::eventsPerDay): с паники
    // падают грузы, застревают колонны, разбиваются вертолёты. Только у мира
    // со сценарием; выключено — ивенты только вручную (и по events).
    bool phaseEvents = true;
    // Мирная жизнь (этап 7.2): у людей профессии и деньги, магазины открыты и
    // получают завоз, в больнице лечат, в домах водопровод. Выключено — все
    // сами по себе, как в этапах 4–7.1 (так устроены старые тесты).
    bool peacetime = true;
    // Сценарий (этап 7.4): через сколько игровых суток от начала мира
    // начинается вспышка, если её не начали раньше (интерфейс, --phase, позже
    // рассказчик); 0 — не по расписанию. Только у мира с мирной жизнью: без
    // неё катастрофа уже случилась и мир сразу живёт по правилам выживания.
    double outbreakDays = 7.0;
    // Уровни детализации симуляции, LOD (этап 6а, sim/Lod.h): вдали от
    // встреч людей и зомби существа смотрят по сторонам и решают реже. Уровень
    // — от состояния мира, не от камеры: мир тот же в окне и без него.
    // Выключено — все на полном уровне, мир тот же, что до этапа 6а (для
    // сравнения и отладки: lw_headless --lod off).
    bool lod = true;
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

    // Замер времени по системам тика и шагам старта (этап 2а.1; на мир не
    // влияет): sim/Profiler.h. Шаги старта пишутся всегда, тик — после
    // setProfiling(true).
    void setProfiling(bool enabled) { profiler_.setEnabled(enabled); }
    Profiler& profiler() { return profiler_; }
    const Profiler& profiler() const { return profiler_; }
    // Сколько памяти занимают крупные структуры мира (оценка по ёмкости
    // контейнеров; для замеров, этап 2а.1).
    std::vector<MemoryItem> memoryReport() const;

    // Сущности создавать и удалять только здесь — так у каждой есть стабильный Id.
    entt::entity create();
    void destroy(entt::entity entity);
    // entt::null, если объекта с таким id (уже) нет.
    entt::entity find(EntityId id) const;

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }
    Rng& rng(RngStream stream) { return streams_[static_cast<std::size_t>(stream)]; }

    std::uint64_t seed() const { return seed_; }
    const WorldConfig& config() const { return config_; }
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
    // Карта активности — где люди, зомби, бой (этап 6а, sim/Lod.h): по ней
    // LodSystem ставит уровни детализации. Производные данные, в хеш не входят.
    const ActivityMap& activityMap() const { return activity_; }
    ActivityMap& activityMap() { return activity_; }
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
    // Поселения, здания, места для питья и сбора — справочник для решений людей.
    const Places& places() const { return places_; }
    // Возобновляемые ресурсы: ягоды и грибы в лесу, дождевая вода.
    Resources& resources() { return resources_; }
    const Resources& resources() const { return resources_; }
    // Летопись: значимые события по id участников.
    Chronicle& chronicle() { return chronicle_; }
    const Chronicle& chronicle() const { return chronicle_; }
    // Записать событие летописи в текущем тике; third — третий участник.
    void record(ChronicleKind kind, EntityId subject, EntityId object = kInvalidId,
                std::int32_t a = 0, std::int32_t b = 0, EntityId third = kInvalidId) {
        chronicle_.add({tick_, kind, subject, object, a, b, third});
    }
    // Разовые звуки последних тиков: выстрелы, крики (см. sim/Sound.h).
    const std::vector<Sound>& sounds() const { return sounds_; }
    // Издать звук в текущем тике: слышен в loudness метрах в тихую погоду.
    void emitSound(float x, float y, float loudness, SoundKind kind,
                   EntityId source = kInvalidId) {
        sounds_.push_back({tick_, x, y, loudness, source, kind});
    }
    // Забыть звуки, прозвучавшие раньше тика before (их больше никто не услышит).
    void forgetSounds(std::uint64_t before);

    // Хеш всего состояния, не зависящий от порядка хранения сущностей.
    // Основа проверок детерминизма (и, позже, сохранения/загрузки).
    std::uint64_t stateHash() const;

private:
    // Первым: старт мира (генерация в конструкторе) пишется уже в него.
    Profiler profiler_;
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
    Chronicle chronicle_;
    // По порядку издания, то есть по тикам.
    std::vector<Sound> sounds_;

    NavGrid nav_;
    NavGraph graph_;
    Pathfinder pathfinder_{nav_, graph_};
    Places places_;
    Resources resources_;
    ActivityMap activity_;

    // Объявлен раньше реестра, чтобы пережить его: реестр шлёт индексу сигналы.
    SpatialIndex spatial_;
    entt::registry registry_;
    std::vector<Rng> streams_;
    // Только для поиска по id; обходить нельзя — порядок не детерминирован.
    std::unordered_map<EntityId, entt::entity> idToEntity_;
};

} // namespace lw
