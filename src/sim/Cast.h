#pragma once

#include "sim/Chronicle.h"
#include "sim/Id.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lw {

class World;

// Главные герои (этап 12а.3): несколько людей с самой богатой биографией, к
// которым режиссёр возвращается из эпизода в эпизод. Состав складывается из
// летописи: у каждого человека копится вес биографии (кражи, месть, спасения,
// группы, укусы — каждое событие весит по виду и роли), между людьми — связи
// (общие события). Первые герои — самые яркие; дальше новый герой входит в
// состав только через связь с уже знакомым зрителю героем (обокрал его,
// спас, вступил в его группу, отомстил за него) — так у сериала есть ядро, а
// новые лица появляются из старых историй.
//
// Как и сюжетные линии, это производные данные: только читают летопись, в
// World::stateHash не входят и на решения существ не влияют. Смотр состава —
// раз в игровой час по тикам событий, поэтому итог не зависит от того, как
// часто звать update (каждый кадр или один раз в конце прогона).

enum class HeroStatus : std::uint8_t {
    Alive,   // в составе
    Dead,    // погиб
    Turned,  // встал зомби
    Retired, // ушёл со сцены: давно ничего не происходит, место отдали новому
    Count
};

struct Protagonist {
    static constexpr std::uint32_t kNoEvent = std::numeric_limits<std::uint32_t>::max();

    EntityId id = kInvalidId;
    std::uint64_t since = 0; // тик, когда вошёл в состав
    // Через кого введён: герой, с которым связан сильнее всех (kInvalidId —
    // из первых, без связи).
    EntityId introducedBy = kInvalidId;
    // Чем связан с ним: самое весомое общее событие (номер в Chronicle::events()).
    std::uint32_t via = kNoEvent;
    HeroStatus status = HeroStatus::Alive;
    std::uint64_t ended = 0;           // тик выбытия
    std::uint32_t endEvent = kNoEvent; // гибель или обращение

    bool alive() const { return status == HeroStatus::Alive; }
};

class CastTracker {
public:
    // Сколько живых героев в составе одновременно.
    static constexpr std::size_t kRoster = 5;

    // Дочитать новые записи летописи и провести смотры состава до тика now.
    void update(const Chronicle& chronicle, std::uint64_t now);
    void update(const World& world);

    // Все, кто когда-либо был в составе, по порядку вхождения (и выбывшие).
    const std::vector<Protagonist>& heroes() const { return heroes_; }
    const Protagonist* find(EntityId id) const;
    // Живой герой из нынешнего состава.
    bool isHero(EntityId id) const;
    std::size_t living() const;

    // Вес биографии: сумма весов всех событий с его участием.
    float weight(EntityId id) const;
    // Сколько значимых (с ненулевым весом) событий в биографии.
    std::uint32_t deeds(EntityId id) const;
    // Сила связи пары (сумма весов общих событий) и самое весомое из них.
    float link(EntityId a, EntityId b) const;
    std::uint32_t linkEvent(EntityId a, EntityId b) const;

private:
    struct Bio {
        float total = 0.0f;  // вес биографии
        float recent = 0.0f; // он же, угасающий (на тик last)
        std::uint64_t last = 0;
        std::uint32_t deeds = 0;
        bool notable = false; // уже в notable_
        bool gone = false;    // погиб или обратился
    };
    struct Link {
        float strength = 0.0f;
        float top = 0.0f; // вес самого весомого общего события
        std::uint32_t event = Protagonist::kNoEvent;
    };
    using Pair = std::pair<EntityId, EntityId>;
    struct PairHash {
        std::size_t operator()(const Pair& pair) const {
            return static_cast<std::size_t>(pair.first * 0x9E3779B97F4A7C15ULL ^ pair.second);
        }
    };
    static Pair pairOf(EntityId a, EntityId b) { return a < b ? Pair{a, b} : Pair{b, a}; }

    void consume(const ChronicleEvent& event, std::uint32_t index);
    void credit(EntityId id, float weight, std::uint64_t tick);
    void connect(EntityId a, float wa, EntityId b, float wb, std::uint32_t index);
    void leave(EntityId id, HeroStatus status, std::uint32_t index, std::uint64_t tick);
    bool isPerson(EntityId id) const;
    float recentAt(const Bio& bio, std::uint64_t tick) const;
    // Смотр состава на тик tick: учтены все события раньше него.
    void review(std::uint64_t tick);

    std::size_t cursor_ = 0;
    std::uint64_t nextReview_ = 0;
    std::vector<Protagonist> heroes_;
    std::vector<EntityId> notable_; // набравшие вес, по порядку — кандидаты
    // Только для поиска; обходить нельзя — порядок не детерминирован.
    std::unordered_set<EntityId> people_; // кто поселился в мире (Settled)
    std::unordered_map<EntityId, Bio> bios_;
    std::unordered_map<Pair, Link, PairHash> links_;
};

// Вес события биографии для участника who (по виду события и роли в нём;
// 0 — не участник или событие незначимо). Им же пользуются подписи
// режиссёра: что важного случилось с героем.
float chronicleWeight(const ChronicleEvent& event, EntityId who);

const char* heroStatusName(HeroStatus status);

} // namespace lw
