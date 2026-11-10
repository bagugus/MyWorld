#pragma once

#include "sim/Components.h"
#include "sim/Id.h"
#include "sim/Perception.h"
#include "sim/TimeScale.h"

#include <entt/entt.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lw {

class World;

// Уровни детализации симуляции, LOD (этап 6а). Здесь — правила; уровни ставит
// LodSystem. Подробности и числа — docs/architecture.md, «LOD симуляции».
//
// Главное правило: уровень — функция состояния мира, а не камеры. Наблюдение
// мир не меняет: камера, выбор в инспекторе, режиссёр, скорость x1/x10/x100,
// бюджет кадра, число потоков и мощность машины на уровень не влияют, поэтому
// один seed даёт один мир — в окне и без него. Вмешательство (ивент кнопкой,
// позже вселение) — вход в мир, как и прежде.
//
// Уровень меняет только ритм: как часто существо смотрит по сторонам
// (восприятие) и как часто решают зомби, звери и птицы. Человек решает каждый
// тик на любом уровне — у него уровень меняет только восприятие (решения реже
// снижали число краж, свидетелей и мести, см. docs/architecture.md). Движение,
// бой, нужды, заражение идут каждый тик и каждую минуту, как без LOD, —
// скачков состояния нет, переход между уровнями — просто другой ритм.
//
// Расписания вложены: обновление с базовым интервалом base идёт в тики, где
// (tick + id) % (base × strideOf(level)) == 0, а множители 1, 4, 16 делят
// друг друга. Редкое расписание — подмножество частого, поэтому смена уровня
// не даёт ни двойного обновления, ни перерыва длиннее интервала нового уровня.
//
// Кто на каком уровне, решает LodSystem раз в kLodPeriodTicks по карте
// активности (ActivityMap): полный — где люди и зомби близко друг к другу (или
// тот, кто вот-вот встанет: тело заражённого, больной в жару) и вокруг боя,
// бегства, погони, охоты; спокойный — где люди есть, а встречи нет; дремлющий —
// зомби и звери вдали от людей. Расстояния взяты с запасом: за время между
// пересчётами сблизиться до того, чтобы увидеть друг друга, не успевают.
// Событие (удар, начало боя или бегства) поднимает до полного сразу — wake.

// Пересчёт уровней — раз в игровую минуту (5 с симуляции).
inline constexpr std::uint64_t kLodPeriodTicks = kTicksPerGameMinute;
// После встречи, боя или удара полный уровень держится столько (не меньше
// одного пересчёта): без мигания на границе и с «послесловием» сцены.
inline constexpr std::uint64_t kFullHoldTicks = 2 * kLodPeriodTicks;

// Множитель ритма уровня. Множители делят друг друга: расписания вложены.
inline constexpr std::uint64_t strideOf(Detail level) {
    return level == Detail::Full ? 1 : level == Detail::Calm ? 4 : 16;
}
inline constexpr std::uint64_t kMaxDetailStride = strideOf(Detail::Dormant);
static_assert(strideOf(Detail::Calm) % strideOf(Detail::Full) == 0);
static_assert(strideOf(Detail::Dormant) % strideOf(Detail::Calm) == 0);

// Очередь обновления с базовым интервалом base для существа уровня level.
inline constexpr bool due(std::uint64_t tick, EntityId id, std::uint64_t base, Detail level) {
    return (tick + id) % (base * strideOf(level)) == 0;
}

// Окно свежести восприятия существа уровня level: от одного его обновления до
// следующего (Awareness::window, isFresh). После повышения уровня старые
// контакты сразу считаются старыми — до ближайшего обновления (не дольше
// kPerceptionIntervalTicks); после понижения последнее увиденное остаётся
// свежим до следующего взгляда.
inline constexpr std::uint16_t senseWindow(Detail level) {
    return static_cast<std::uint16_t>(kPerceptionIntervalTicks * strideOf(level));
}

// Сколько живут разовые звуки мира: столько, чтобы их услышал и самый редко
// смотрящий слушатель. Слушатель берёт звуки с прошлого своего обновления
// (Awareness::updated), а между двумя обновлениями при смене уровня проходит
// меньше двух интервалов самого редкого уровня (спокойный → дремлющий: до 40 +
// 160 тиков) — столько звук и живёт. Без LOD — kSoundLifetimeTicks, как прежде.
std::uint64_t soundLifetime(const World& world);

// Уровень существа без поиска хранилища на каждый вызов. Нет компонента Lod —
// полный уровень: LOD выключен или стая птиц появилась после пересчёта.
class DetailLookup {
public:
    explicit DetailLookup(const entt::registry& registry) : lods_(registry.storage<Lod>()) {}

    Detail of(entt::entity entity) const {
        return lods_ != nullptr && lods_->contains(entity) ? lods_->get(entity).level : Detail::Full;
    }

private:
    const entt::storage_for_t<Lod>* lods_;
};

Detail detailOf(const entt::registry& registry, entt::entity entity);

// Действие человека, при котором нужен полный уровень: бежит, дерётся,
// охотится, мстит.
inline bool keepsFull(Action action) {
    return action == Action::Flee || action == Action::Fight || action == Action::Hunt ||
           action == Action::Avenge;
}

// Поднять существо до полного уровня сразу — событие не ждёт пересчёта: удар,
// начало боя, бегства, погони, охоты. Держится kFullHoldTicks. Без LOD и у
// существа без компонента (оно и так на полном) — ничего.
void wake(World& world, entt::entity entity);

// Поставить уровень: компонент Lod и окно свежести восприятия.
void setDetail(entt::registry& registry, entt::entity entity, Detail level, LodCause cause,
               std::uint64_t fullUntil);

// С LOD у каждого, кто воспринимает мир, с рождения есть Lod — полный уровень
// до первого пересчёта: World подключает это к появлению Awareness. Так
// восприятие выбирает очередь обходом одного хранилища Lod.
void attachLod(entt::registry& registry, entt::entity entity);

// Кто это для LOD: от вида зависит, чья близость важна.
enum class LodKind : std::uint8_t { Person, Zombie, Animal, Flock };

// Что вокруг существа по карте активности.
struct Surroundings {
    bool peopleNear = false; // люди ближе kEncounterRegions районов
    bool undeadNear = false; // зомби или тот, кто вот-вот встанет, — так же близко
    bool activeNear = false; // бой, бегство, погоня, охота ближе kActiveRegions
    bool peopleFar = false;  // люди ближе kPopulatedRegions районов
};

struct DetailVerdict {
    Detail level = Detail::Full;
    LodCause cause = LodCause::New;
};

// Уровень по виду, своему делу (active: бой, бегство, погоня, охота) и
// окрестностям — без удержания (его добавляет LodSystem). Человек — полный,
// если рядом зомби или бой, иначе спокойный; зомби — полный, если рядом люди
// или бой, спокойный, если люди недалеко, иначе дремлющий; зверь и стая
// птиц — как зомби, а зомби рядом для них — спокойный уровень: зомби зверей
// не трогают, звери только уходят от них.
DetailVerdict judgeDetail(LodKind kind, bool active, const Surroundings& around);

// Карта активности (этап 6а): районы kRegionSize метров, слои — по биту на
// район. Производные данные: LodSystem пересчитывает её целиком раз в
// kLodPeriodTicks по позициям и состояниям, в хеш она не входит (уровни
// существ — входят: Lod). Её же читают интерфейс (слой «детализация» в виде
// сверху) и, позже, наблюдатель — где в мире сейчас горячо.
class ActivityMap {
public:
    // Район — 64 м: два ряда корзин пространственного индекса.
    static constexpr float kRegionSize = 64.0f;
    // Встреча — люди и зомби ближе чем 3 района по каждой оси: точно ловится
    // всё ближе 192 м (дальше всех видит человек — 150 м днём, плюс сближение
    // за время между пересчётами: 2,5 м/с бегущий и 1,7 м/с зомби за 5 с — 21 м).
    static constexpr int kEncounterRegions = 3;
    // Вокруг боя, бегства, погони и охоты — столько же.
    static constexpr int kActiveRegions = 3;
    // Люди недалеко — 7 районов: точно всё ближе 448 м.
    static constexpr int kPopulatedRegions = 7;

    enum Layer : std::uint8_t {
        People,     // живые люди
        Undead,     // зомби; тело, которое встанет; больной в жару
        Active,     // в бою, бежит, преследует, охотится, мстит
        PeopleNear, // People, расширенный на kEncounterRegions
        UndeadNear, // Undead, расширенный на kEncounterRegions
        ActiveNear, // Active, расширенный на kActiveRegions
        PeopleFar,  // People, расширенный на kPopulatedRegions
        LayerCount
    };

    // Размер мира в метрах; слои — пустые.
    void reset(float worldWidth, float worldHeight);
    // Все слои — в ноль (перед пересчётом).
    void clear();
    void mark(Layer layer, float x, float y);
    // Слой to — слой from, расширенный на radius районов во все стороны
    // (квадрат). radius < 64.
    void dilate(Layer from, Layer to, int radius);

    int width() const { return width_; }   // районы
    int height() const { return height_; } // районы
    int regionX(float x) const;
    int regionY(float y) const;
    bool test(Layer layer, float x, float y) const { return testRegion(layer, regionX(x), regionY(y)); }
    bool testRegion(Layer layer, int rx, int ry) const;
    // Окрестности точки — для judgeDetail.
    Surroundings around(float x, float y) const;

    // Пересчитана ли хоть раз и в какой тик последний раз.
    bool computed() const { return computed_; }
    std::uint64_t updated() const { return updated_; }
    void setUpdated(std::uint64_t tick) {
        updated_ = tick;
        computed_ = true;
    }

    std::size_t memoryBytes() const;

private:
    int width_ = 0;
    int height_ = 0;
    int words_ = 0;                  // слов на строку
    std::uint64_t lastMask_ = ~0ull; // биты последнего слова строки внутри карты
    std::uint64_t updated_ = 0;
    bool computed_ = false;
    std::array<std::vector<std::uint64_t>, LayerCount> bits_;
    std::vector<std::uint64_t> scratch_; // расширение по горизонтали
};

} // namespace lw
