#pragma once

#include "sim/Components.h"
#include "sim/Sound.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <limits>

namespace lw {

class Map;
class NavGrid;
class World;
struct Weather;

// Восприятие: кто кого видит и слышит. Здесь — правила, чистые функции от
// состояния мира; обновляет Awareness существ PerceptionSystem. Подробности и
// все числа — в docs/architecture.md, раздел «Восприятие».
//
// Зрение. Дальность — Senses::sight, умноженная на условия (свет, туман,
// осадки) и на заметность цели (стоит, лежит, в лесу). Луч от глаз до цели
// идёт по клеткам: здание его закрывает, каждая клетка леса добавляет к пути
// kForestSightPenalty метров — в лес видно на несколько десятков метров.
// Рельеф обзор пока не закрывает.
//
// Слух. Существо шумит тем, что делает (noiseOf): шаги, обыск, разговор.
// Разовые звуки (выстрел, крик) — в World::sounds(). Звук слышен в пределах
// своей громкости в метрах, умноженной на слух слушателя и на погоду: дождь и
// ветер глушат. Стены звук пока не глушат.

// Как часто существо обновляет восприятие: раз в 0,5 с симуляции. Существа
// обновляются вразнобой (по id), за тик — десятая часть.
inline constexpr std::uint64_t kPerceptionIntervalTicks = 10;
// Разовый звук слышен столько тиков: каждый слушатель успевает обновиться.
inline constexpr std::uint64_t kSoundLifetimeTicks = kPerceptionIntervalTicks;
// Сколько помнится потерянный контакт: 30 с симуляции, 6 игровых минут.
inline constexpr std::uint64_t kContactMemoryTicks = 30 * kTicksPerSecond;

// Заметность цели — множитель дальности зрения: идущий по открытому месту — 1.
inline constexpr float kStillVisibility = 0.75f; // стоит на месте
inline constexpr float kLyingVisibility = 0.5f;  // лежит (спит)
inline constexpr float kForestVisibility = 0.6f; // в лесу — ещё раз на столько

// Контакт подтверждён последним обновлением восприятия (иначе только помнится).
// Обновление было не раньше чем kPerceptionIntervalTicks тиков назад: в тик
// tick оно для этого существа ещё может быть впереди.
inline bool isFresh(const Contact& contact, std::uint64_t tick) {
    return contact.tick + kPerceptionIntervalTicks >= tick;
}

// Лишние метры луча за каждую клетку леса между глазами и целью.
inline constexpr float kForestSightPenalty = 20.0f;

// Громкость постоянных шумов: в стольких метрах слышно в тихую погоду.
inline constexpr float kFootstepsLoudness = 12.0f; // шаги на 1,4 м/с, быстрее — громче
inline constexpr float kRummageLoudness = 35.0f;   // обыск здания
inline constexpr float kVoiceLoudness = 18.0f;     // разговор
// Громче шаги не бывают: бег (этап 5) — до 2,5 м/с.
inline constexpr float kMaxNoiseLoudness = kRummageLoudness;

inline constexpr float kNoSight = std::numeric_limits<float>::infinity();

// Условия, общие для всех в этот момент: множители дальности зрения и
// громкости звуков.
struct SenseConditions {
    float sight = 1.0f;
    float hearing = 1.0f;
};

// Ночью видно на 0,08–0,5 дневной дальности (безлунная ночь — полная луна),
// в густом тумане — на пятую часть, в ливень — на 0,7. Ливень с ветром глушат
// звуки вдвое.
SenseConditions senseConditions(float light, const Weather& weather);
SenseConditions senseConditions(const World& world);

// Компоненты, которые восприятие читает у соседей. Хранилища берутся из
// реестра один раз: в EnTT поиск хранилища по типу (registry.try_get) стоит
// дороже самого компонента, а соседей за обновление — десятки.
class SenseLookup {
public:
    explicit SenseLookup(const entt::registry& registry)
        : senses_(registry.storage<Senses>()),
          people_(registry.storage<Person>()),
          velocities_(registry.storage<Velocity>()),
          minds_(registry.storage<Mind>()) {}

    // Есть органы чувств — существо, его можно заметить.
    bool perceivable(entt::entity entity) const { return get(senses_, entity) != nullptr; }
    // Спит ли существо: спящий не видит и слышит хуже.
    bool asleep(entt::entity entity) const {
        const Person* person = get(people_, entity);
        return person != nullptr && person->asleep;
    }
    const Velocity* velocity(entt::entity entity) const { return get(velocities_, entity); }
    const Mind* mind(entt::entity entity) const { return get(minds_, entity); }

private:
    template <typename Storage>
    static auto get(const Storage* storage, entt::entity entity)
        -> decltype(&storage->get(entity)) {
        return storage != nullptr && storage->contains(entity) ? &storage->get(entity) : nullptr;
    }

    const entt::storage_for_t<Senses>* senses_;
    const entt::storage_for_t<Person>* people_;
    const entt::storage_for_t<Velocity>* velocities_;
    const entt::storage_for_t<Mind>* minds_;
};

bool isAsleep(const entt::registry& registry, entt::entity entity);

// Дальность зрения на открытом месте до самой заметной цели; 0 — не видит.
float sightRange(const Senses& senses, bool asleep, const SenseConditions& conditions);
// Множитель громкости звуков для слушателя.
float hearingFactor(const Senses& senses, bool asleep, const SenseConditions& conditions);

// Цена взгляда из точки a в точку b в «метрах открытой местности»: расстояние
// плюс штраф за лес между ними. Клетки самих a и b не считаются: стоящий у
// стены видит вдоль неё, а заметность цели в лесу — в visibilityOf. Здание или
// край карты на пути — kNoSight; обход прерывается и возвращает kNoSight, как
// только цена превысит limit.
float sightCost(const Map& map, const NavGrid& nav, float ax, float ay, float bx, float by,
                float limit = std::numeric_limits<float>::max());

// Насколько заметна цель в точке (x, y): 1 — идёт по открытому месту; стоит
// или лежит (спит) — меньше, в лесу — ещё меньше. Произведение двух частей:
// позы (bodyVisibility) и укрытия в клетке (coverVisibility).
float visibilityOf(const World& world, entt::entity target, float x, float y);
float bodyVisibility(const SenseLookup& lookup, entt::entity target);
float coverVisibility(const Map& map, float x, float y);

// Постоянный шум существа — от того, что оно делает.
struct Noise {
    SoundKind kind = SoundKind::None;
    float loudness = 0.0f; // метры в тихую погоду
};
Noise noiseOf(const SenseLookup& lookup, entt::entity entity);
Noise noiseOf(const entt::registry& registry, entt::entity entity);

} // namespace lw
