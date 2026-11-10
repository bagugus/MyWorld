#include "sim/Cast.h"

#include "sim/TimeScale.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace lw {

namespace {

// Угасание «свежего» веса биографии: за двое суток — в e раз.
constexpr float kRecentDays = 2.0f;
// С какого веса биографии человек — кандидат в герои.
constexpr float kNotable = 3.0f;
// Сколько должна набрать оценка кандидата (свежий вес + доля полного), чтобы
// войти в состав.
constexpr float kEnter = 4.0f;
constexpr float kTotalShare = 0.3f;
// Связь с героем, достаточная, чтобы ввести через него, и сколько она прибавляет.
constexpr float kMinLink = 1.5f;
constexpr float kLinkWeight = 1.0f;
constexpr float kLinkCap = 6.0f;
// Пока живых героев меньше, в состав входят и без связи (ядро сериала).
constexpr std::size_t kOpenRoster = 2;
// Кандидат должен быть на виду: последнее событие — не раньше суток назад.
constexpr std::uint64_t kFresh = kTicksPerGameDay;
// Героя, у которого столько ничего не происходит, можно сменить новым.
constexpr std::uint64_t kRetireIdle = 3 * kTicksPerGameDay;
// Смотр состава — раз в игровой час.
constexpr std::uint64_t kReviewPeriod = kTicksPerGameHour;

// Вес события для каждой роли: кто (subject), с кем (object), третий.
struct Roles {
    float subject = 0.0f;
    float object = 0.0f;
    float third = 0.0f;
};

Roles rolesOf(const ChronicleEvent& event) {
    using K = ChronicleKind;
    switch (event.kind) {
    case K::Starving:
    case K::Dehydrated:
    case K::Weakened:
    case K::Recovered:
        return {0.5f};
    case K::Exhausted:
        return {0.2f};
    case K::Dying:
        return {1.0f};
    case K::Fled:
        return event.b == 1 ? Roles{0.8f, 0.8f} : Roles{0.3f}; // b = 1 — от человека
    case K::Bitten:
        return {2.0f};
    case K::KilledZombie:
        return {0.6f};
    case K::Hunted:
    case K::LootedBody:
        return {0.3f};
    case K::Died:
        return {2.0f, 2.5f}; // погибший и убийца (если человек)
    case K::Butchered:
    case K::Treated:
        return {0.2f};
    case K::Turned:
        return {2.0f};
    case K::Feverish:
        return {1.0f};
    case K::FoundWeapon:
        return {0.4f};
    case K::Befriended:
    case K::Disliked:
        return {1.0f, 0.5f};
    case K::SavedBy:
        return {2.0f, 2.0f};
    case K::DefendedBy:
    case K::AbandonedBy:
    case K::CaughtThief:
        return {1.5f, 1.5f};
    case K::Mourned:
        return {1.5f, 1.0f, 0.3f}; // object погиб — только связь
    case K::SawTurned:
        return {1.5f, 1.0f};
    case K::Witnessed:
        return {0.8f, 1.0f, 0.5f};
    case K::HeardOf:
        return {0.5f, 0.5f, 0.3f};
    case K::Reconciled:
        return {1.5f, 1.0f};
    case K::GroupFormed:
        return {3.0f};
    case K::JoinedGroup:
        return {1.0f, 0.0f, 0.5f};
    case K::LeftGroup:
        return {1.0f, 0.0f, 0.3f};
    case K::Expelled:
        return {2.0f, 2.0f};
    case K::LedGroup:
        return {2.5f, 0.0f, 1.0f};
    case K::GroupDisbanded:
        return {0.0f, 1.0f};
    case K::Shared:
        return {1.0f, 0.8f};
    case K::Traded:
        return {0.4f, 0.4f};
    case K::Stole:
        return {2.0f, 0.0f, 1.5f};
    case K::Deserted:
        return {3.0f, 0.0f, 1.5f};
    case K::BetrayedBy:
        return {2.0f, 2.0f};
    case K::AssaultedBy:
        return {2.0f, 2.5f};
    case K::Avenged:
        return {3.5f, 2.5f};
    case K::LearnedOfEvent:
        return {0.3f};
    case K::Salvaged:
        return {event.a > 0 || event.b > 0 ? 1.5f : 0.5f};
    case K::Employed:
        return {0.1f};
    case K::SawDoctor:
        return {0.3f};
    case K::LeftWork:
        return {0.8f};
    case K::Orphaned:
        return {2.5f, 1.0f}; // сирота; погибший родитель — только связь (этап 7.3)
    case K::TookIn:
        return {2.0f, 2.0f}; // взял к себе — и ребёнок: теперь они связаны
    case K::PatientZero:
        return {2.5f}; // объект — сценарий, не человек (этап 7.5)
    case K::Infected:
        return {1.5f, 0.8f}; // заразился — и от кого: больной сам не знал
    case K::Alarmed: // этап 7.6: узнал о беде — и от кого
        return {event.a >= 2 ? 0.5f : 0.2f, 0.2f};
    case K::BrokeQuarantine:
        return {1.5f};
    case K::Evacuated:
        return {2.5f};
    case K::EvacuationRefused:
        return {2.0f, 0.0f, 1.0f};
    case K::StayedBehind:
        return {2.5f};
    case K::Reunited:
        return {2.0f, 1.5f};
    case K::EventCrew: // этап 7.7: выживший при крушении или из колонны — новый человек в краю
        return {event.a == 1 ? 2.5f : 0.5f};
    default:
        return {};
    }
}

} // namespace

void CastTracker::update(const World& world) { update(world.chronicle(), world.tick()); }

void CastTracker::update(const Chronicle& chronicle, std::uint64_t now) {
    const std::vector<ChronicleEvent>& events = chronicle.events();
    for (; cursor_ < events.size(); ++cursor_) {
        const ChronicleEvent& event = events[cursor_];
        // Смотр на тик nextReview_ видит только события раньше него.
        while (nextReview_ <= event.tick) {
            review(nextReview_);
            nextReview_ += kReviewPeriod;
        }
        consume(event, static_cast<std::uint32_t>(cursor_));
    }
    // События тика now ещё могут прийти: смотры — только до него.
    while (nextReview_ < now) {
        review(nextReview_);
        nextReview_ += kReviewPeriod;
    }
}

const Protagonist* CastTracker::find(EntityId id) const {
    for (const Protagonist& hero : heroes_) {
        if (hero.id == id) {
            return &hero;
        }
    }
    return nullptr;
}

bool CastTracker::isHero(EntityId id) const {
    const Protagonist* hero = find(id);
    return hero != nullptr && hero->alive();
}

std::size_t CastTracker::living() const {
    return static_cast<std::size_t>(std::count_if(
        heroes_.begin(), heroes_.end(), [](const Protagonist& hero) { return hero.alive(); }));
}

float CastTracker::weight(EntityId id) const {
    const auto it = bios_.find(id);
    return it != bios_.end() ? it->second.total : 0.0f;
}

std::uint32_t CastTracker::deeds(EntityId id) const {
    const auto it = bios_.find(id);
    return it != bios_.end() ? it->second.deeds : 0;
}

float CastTracker::link(EntityId a, EntityId b) const {
    const auto it = links_.find(pairOf(a, b));
    return it != links_.end() ? it->second.strength : 0.0f;
}

std::uint32_t CastTracker::linkEvent(EntityId a, EntityId b) const {
    const auto it = links_.find(pairOf(a, b));
    return it != links_.end() ? it->second.event : Protagonist::kNoEvent;
}

bool CastTracker::isPerson(EntityId id) const {
    if (id == kInvalidId || !people_.contains(id)) {
        return false;
    }
    const auto it = bios_.find(id);
    return it == bios_.end() || !it->second.gone;
}

float CastTracker::recentAt(const Bio& bio, std::uint64_t tick) const {
    const float days = static_cast<float>(tick - std::min(tick, bio.last)) /
                       static_cast<float>(kTicksPerGameDay);
    return bio.recent * std::exp(-days / kRecentDays);
}

void CastTracker::credit(EntityId id, float weight, std::uint64_t tick) {
    if (weight <= 0.0f || !isPerson(id)) {
        return;
    }
    Bio& bio = bios_[id];
    bio.recent = recentAt(bio, tick) + weight;
    bio.last = tick;
    bio.total += weight;
    ++bio.deeds;
    if (!bio.notable && bio.total >= kNotable) {
        bio.notable = true;
        notable_.push_back(id);
    }
}

void CastTracker::connect(EntityId a, float wa, EntityId b, float wb, std::uint32_t index) {
    if (a == kInvalidId || b == kInvalidId || a == b || wa <= 0.0f || wb <= 0.0f ||
        !people_.contains(a) || !people_.contains(b)) {
        return;
    }
    const float weight = std::min(wa, wb);
    Link& link = links_[pairOf(a, b)];
    link.strength += weight;
    if (wa + wb > link.top) {
        link.top = wa + wb;
        link.event = index;
    }
}

void CastTracker::leave(EntityId id, HeroStatus status, std::uint32_t index, std::uint64_t tick) {
    if (const auto it = bios_.find(id); it != bios_.end()) {
        it->second.gone = true;
    } else if (people_.contains(id)) {
        bios_[id].gone = true;
    }
    for (Protagonist& hero : heroes_) {
        if (hero.id != id) {
            continue;
        }
        if (hero.alive()) {
            hero.ended = tick;
        }
        if (hero.alive() || (hero.status == HeroStatus::Dead && status == HeroStatus::Turned)) {
            hero.status = status;
            hero.endEvent = index;
        }
    }
}

void CastTracker::consume(const ChronicleEvent& event, std::uint32_t index) {
    if (event.kind == ChronicleKind::Settled) {
        people_.insert(event.subject);
        return;
    }
    const Roles roles = rolesOf(event);
    connect(event.subject, roles.subject, event.object, roles.object, index);
    connect(event.subject, roles.subject, event.third, roles.third, index);
    connect(event.object, roles.object, event.third, roles.third, index);
    credit(event.subject, roles.subject, event.tick);
    credit(event.object, roles.object, event.tick);
    credit(event.third, roles.third, event.tick);
    if (event.kind == ChronicleKind::Died) {
        leave(event.subject, HeroStatus::Dead, index, event.tick);
    } else if (event.kind == ChronicleKind::Turned) {
        leave(event.subject, HeroStatus::Turned, index, event.tick);
    } else if (event.kind == ChronicleKind::Evacuated) {
        // Уехал из края (этап 7.6) — ушёл со сцены.
        leave(event.subject, HeroStatus::Retired, index, event.tick);
    }
}

void CastTracker::review(std::uint64_t tick) {
    // Лучший кандидат: свежий вес и доля полного, плюс связь с героем.
    EntityId best = kInvalidId;
    float bestValue = 0.0f;
    EntityId bestBy = kInvalidId;
    std::uint32_t bestVia = Protagonist::kNoEvent;
    const std::size_t alive = living();
    for (const EntityId id : notable_) {
        const Bio& bio = bios_.at(id);
        if (bio.gone || bio.last + kFresh < tick || find(id) != nullptr) {
            continue;
        }
        float value = recentAt(bio, tick) + kTotalShare * bio.total;
        // Сильнейшая связь с героем — живым или выбывшим: оба знакомы зрителю.
        EntityId by = kInvalidId;
        float strength = 0.0f;
        for (const Protagonist& hero : heroes_) {
            const auto it = links_.find(pairOf(id, hero.id));
            if (it != links_.end() && it->second.strength > strength) {
                strength = it->second.strength;
                by = hero.id;
            }
        }
        if (strength >= kMinLink) {
            value += kLinkWeight * std::min(strength, kLinkCap);
        } else if (alive >= kOpenRoster) {
            continue; // в сложившийся сериал — только через знакомых
        } else {
            by = kInvalidId;
        }
        if (value < kEnter) {
            continue;
        }
        if (best == kInvalidId || value > bestValue || (value == bestValue && id < best)) {
            best = id;
            bestValue = value;
            bestBy = by;
            bestVia = by != kInvalidId ? links_.at(pairOf(id, by)).event : Protagonist::kNoEvent;
        }
    }
    if (best == kInvalidId) {
        return;
    }
    if (alive >= kRoster) {
        // Место освобождает самый тихий из давно затихших.
        Protagonist* quiet = nullptr;
        float quietest = 0.0f;
        for (Protagonist& hero : heroes_) {
            const auto it = bios_.find(hero.id);
            if (!hero.alive() || it == bios_.end() || it->second.last + kRetireIdle > tick) {
                continue;
            }
            const float recent = recentAt(it->second, tick);
            if (quiet == nullptr || recent < quietest) {
                quiet = &hero;
                quietest = recent;
            }
        }
        if (quiet == nullptr) {
            return;
        }
        quiet->status = HeroStatus::Retired;
        quiet->ended = tick;
    }
    Protagonist& hero = heroes_.emplace_back();
    hero.id = best;
    hero.since = tick;
    hero.introducedBy = bestBy;
    hero.via = bestVia;
}

float chronicleWeight(const ChronicleEvent& event, EntityId who) {
    if (who == kInvalidId) {
        return 0.0f;
    }
    const Roles roles = rolesOf(event);
    if (who == event.subject) {
        return roles.subject;
    }
    if (who == event.object) {
        return roles.object;
    }
    return who == event.third ? roles.third : 0.0f;
}

const char* heroStatusName(HeroStatus status) {
    constexpr const char* kNames[] = {"alive", "dead", "turned", "retired"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(HeroStatus::Count));
    const auto index = static_cast<std::size_t>(status);
    return index < std::size(kNames) ? kNames[index] : "?";
}

} // namespace lw
