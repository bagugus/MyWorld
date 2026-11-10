#pragma once

// Схема состояния мира (этап С.1): один список компонентов и один список полей
// каждого — и для хеша мира (World::stateHash), и для сохранения (sim/Save.h).
//
// Компонент, влияющий на будущее мира, добавляется в SavedComponents и
// получает visit(ar, компонент) со всеми полями по порядку. Тогда он сам
// попадает и в хеш, и в файл: забыть одно из двух нельзя. Порядок списка и
// полей — порядок хеша; меняя его, меняешь хеш всех миров. Сохранение
// откажется записать мир, в реестре которого есть компонент не из этого
// списка (кроме Id), — так новый компонент не потеряется молча.
//
// Архив (Ar) — тот, кто обходит поля: хеш, запись, чтение, отпечаток схемы
// (sim/Save.cpp). Поля описываются через io(ar, поле) и помощники ниже:
//   io(ar, x)             — число, перечисление, bool, Cell или вложенная
//                           структура со своим visit;
//   ar.unhashed(x)        — только в файл: производное от других полей или
//                           подсказка интерфейсу, на будущее мира не влияет;
//   ar.text(s)            — строка: длина и байты;
//   ioVector(ar, v)       — вектор: длина и элементы;
//   ioCounted(ar, a, n)   — массив с числом занятых мест: n, затем занятые
//                           (в файл — весь массив, чтобы компонент вернулся
//                           байт в байт);
//   ioArray(ar, a)        — массив целиком.

#include "sim/Chronicle.h"
#include "sim/Components.h"
#include "sim/Hash.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/Sound.h"
#include "sim/Weather.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lw {

template <typename... T>
struct TypeList {};

// Вызвать fn(std::type_identity<T>{}) для каждого типа списка — по порядку.
template <typename... T, typename Fn>
void forEachType(TypeList<T...>, Fn&& fn) {
    (fn(std::type_identity<T>{}), ...);
}

// Компоненты состояния мира — в порядке хеша (как он был до этапа С.1).
// Id — не здесь: это сама сущность (хеш берёт его первым, сохранение — блоком
// сущностей).
using SavedComponents =
    TypeList<Position, Velocity, Settlement, Building, Person, Job, Wallet, Workplace, Age, Kin, Name,
             Group, WorldEvent, Scenario, Alarm, Quarantine, EvacuationPoint, Evacuated, Cache, Leads,
             Needs, Traits, Walker, Inventory, Mind, Senses, Lod, Awareness, Zombie, Horde, Animal,
             Flock, Weapon, Combat, Wounds, Relations, Memories, Corpse>;

// Имя компонента (блок в файле, сообщения) и метка в хеше. Метки Age и
// Scenario совпадают (35) с этапа 7.4 — так и оставлены, чтобы хеш миров не
// менялся: порядок компонентов в хеше и так фиксирован.
template <typename T>
struct ComponentInfo;

#define LW_SAVED_COMPONENT(Type, tag)                          \
    template <>                                                \
    struct ComponentInfo<Type> {                               \
        static constexpr std::uint64_t kHashTag = tag;         \
        static constexpr std::string_view kName = #Type;       \
    };
LW_SAVED_COMPONENT(Position, 1)
LW_SAVED_COMPONENT(Velocity, 2)
LW_SAVED_COMPONENT(Settlement, 4)
LW_SAVED_COMPONENT(Building, 5)
LW_SAVED_COMPONENT(Person, 6)
LW_SAVED_COMPONENT(Job, 32)
LW_SAVED_COMPONENT(Wallet, 33)
LW_SAVED_COMPONENT(Workplace, 34)
LW_SAVED_COMPONENT(Age, 35)
LW_SAVED_COMPONENT(Kin, 36)
LW_SAVED_COMPONENT(Name, 31)
LW_SAVED_COMPONENT(Group, 27)
LW_SAVED_COMPONENT(WorldEvent, 28)
LW_SAVED_COMPONENT(Scenario, 35)
LW_SAVED_COMPONENT(Alarm, 37)
LW_SAVED_COMPONENT(Quarantine, 38)
LW_SAVED_COMPONENT(EvacuationPoint, 39)
LW_SAVED_COMPONENT(Evacuated, 40)
LW_SAVED_COMPONENT(Cache, 29)
LW_SAVED_COMPONENT(Leads, 30)
LW_SAVED_COMPONENT(Needs, 9)
LW_SAVED_COMPONENT(Traits, 10)
LW_SAVED_COMPONENT(Walker, 7)
LW_SAVED_COMPONENT(Inventory, 24)
LW_SAVED_COMPONENT(Mind, 13)
LW_SAVED_COMPONENT(Senses, 14)
LW_SAVED_COMPONENT(Lod, 41)
LW_SAVED_COMPONENT(Awareness, 15)
LW_SAVED_COMPONENT(Zombie, 16)
LW_SAVED_COMPONENT(Horde, 17)
LW_SAVED_COMPONENT(Animal, 18)
LW_SAVED_COMPONENT(Flock, 19)
LW_SAVED_COMPONENT(Weapon, 20)
LW_SAVED_COMPONENT(Combat, 21)
LW_SAVED_COMPONENT(Wounds, 22)
LW_SAVED_COMPONENT(Relations, 25)
LW_SAVED_COMPONENT(Memories, 26)
LW_SAVED_COMPONENT(Corpse, 23)
#undef LW_SAVED_COMPONENT

// --- Помощники описания полей ------------------------------------------------

template <typename Ar, typename T>
void io(Ar& ar, T& value) {
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
        ar.value(value);
    } else {
        visit(ar, value);
    }
}

template <typename Ar, typename T>
void ioVector(Ar& ar, std::vector<T>& items) {
    if constexpr (Ar::kSchema) {
        // Отпечаток схемы: вид элемента, а не содержимое.
        ar.marker('v');
        T sample{};
        io(ar, sample);
        return;
    } else {
        std::uint64_t length = items.size();
        ar.length(length);
        if constexpr (Ar::kReading) {
            if (!ar.reserve(length, sizeof(T))) {
                return;
            }
            items.assign(static_cast<std::size_t>(length), T{});
        }
        for (T& item : items) {
            io(ar, item);
        }
    }
}

template <typename Ar, typename T, std::size_t N, typename Count>
void ioCounted(Ar& ar, T (&items)[N], Count& count) {
    io(ar, count);
    if constexpr (Ar::kSchema) {
        ar.marker('c');
        ar.marker(static_cast<std::uint32_t>(N));
        io(ar, items[0]);
        return;
    } else {
        if constexpr (Ar::kReading) {
            if (static_cast<std::size_t>(count) > N) {
                ar.fail("занятых мест больше вместимости");
                count = 0;
            }
        }
        const std::size_t stored = Ar::kHashing ? static_cast<std::size_t>(count) : N;
        for (std::size_t i = 0; i < stored; ++i) {
            io(ar, items[i]);
        }
    }
}

template <typename Ar, typename T, std::size_t N>
void ioArray(Ar& ar, T (&items)[N]) {
    if constexpr (Ar::kSchema) {
        ar.marker('a');
        ar.marker(static_cast<std::uint32_t>(N));
        io(ar, items[0]);
    } else {
        for (T& item : items) {
            io(ar, item);
        }
    }
}

template <typename Ar, typename T, std::size_t N>
void ioArray(Ar& ar, std::array<T, N>& items) {
    if constexpr (Ar::kSchema) {
        ar.marker('a');
        ar.marker(static_cast<std::uint32_t>(N));
        io(ar, items[0]);
    } else {
        for (T& item : items) {
            io(ar, item);
        }
    }
}

// --- Поля ----------------------------------------------------------------------
// Порядок полей — порядок хеша. Новое поле, влияющее на будущее мира, — через
// io (в хеш и в файл), производное — через ar.unhashed (только в файл).

template <typename Ar> void visit(Ar& ar, Cell& c) { io(ar, c.x); io(ar, c.y); }

template <typename Ar> void visit(Ar& ar, Position& c) { io(ar, c.x); io(ar, c.y); }
template <typename Ar> void visit(Ar& ar, Velocity& c) { io(ar, c.x); io(ar, c.y); }

template <typename Ar>
void visit(Ar& ar, Settlement& c) {
    io(ar, c.kind);
    ar.text(c.name);
    io(ar, c.center);
    io(ar, c.radius);
}

template <typename Ar>
void visit(Ar& ar, Building& c) {
    io(ar, c.kind);
    io(ar, c.origin);
    io(ar, c.width);
    io(ar, c.height);
    io(ar, c.door);
    io(ar, c.settlement);
}

template <typename Ar>
void visit(Ar& ar, Person& c) {
    io(ar, c.home);
    io(ar, c.asleep);
    io(ar, c.indoors);
    io(ar, c.group);
    io(ar, c.formerHome);
}

template <typename Ar>
void visit(Ar& ar, Job& c) {
    io(ar, c.profession);
    io(ar, c.workplace);
    io(ar, c.start);
    io(ar, c.end);
    io(ar, c.days);
}

template <typename Ar> void visit(Ar& ar, Wallet& c) { io(ar, c.money); }

template <typename Ar>
void visit(Ar& ar, Workplace& c) {
    io(ar, c.onDuty);
    io(ar, c.till);
    io(ar, c.harvest);
}

template <typename Ar> void visit(Ar& ar, Age& c) { io(ar, c.years); }

template <typename Ar> void visit(Ar& ar, Relative& c) { io(ar, c.id); io(ar, c.kind); }

template <typename Ar>
void visit(Ar& ar, Kin& c) {
    ioCounted(ar, c.relatives, c.count);
    io(ar, c.guardian);
}

template <typename Ar>
void visit(Ar& ar, Name& c) {
    if constexpr (Ar::kHashing) {
        // В хеше — как до этапа С.1: буквы имени, ноль, буквы фамилии.
        ar.chars(c.first);
        std::uint8_t separator = 0;
        io(ar, separator);
        ar.chars(c.last);
    } else {
        ar.text(c.first);
        ar.text(c.last);
    }
}

template <typename Ar>
void visit(Ar& ar, Group& c) {
    io(ar, c.leader);
    ioVector(ar, c.members);
    io(ar, c.base);
    io(ar, c.formed);
}

template <typename Ar>
void visit(Ar& ar, WorldEvent& c) {
    io(ar, c.kind);
    io(ar, c.site);
    io(ar, c.began);
    io(ar, c.signalUntil);
    io(ar, c.settlement);
    ioVector(ar, c.caches);
}

template <typename Ar>
void visit(Ar& ar, Scenario& c) {
    io(ar, c.kind);
    io(ar, c.phase);
    ioArray(ar, c.began);
    io(ar, c.outbreakAt);
    io(ar, c.population);
    io(ar, c.patientZero);
    io(ar, c.quietSince);
    io(ar, c.broadcast);
    io(ar, c.broadcastAt);
}

template <typename Ar>
void visit(Ar& ar, Alarm& c) {
    io(ar, c.level);
    io(ar, c.clue);
    io(ar, c.source);
    io(ar, c.since);
    io(ar, c.refusedAt);
}

template <typename Ar>
void visit(Ar& ar, Quarantine& c) {
    io(ar, c.since);
    io(ar, c.cases);
    io(ar, c.clearSince);
}

template <typename Ar>
void visit(Ar& ar, EvacuationPoint& c) {
    io(ar, c.settlement);
    io(ar, c.opened);
    io(ar, c.convoys);
    io(ar, c.evacuated);
}

template <typename Ar> void visit(Ar& ar, Evacuated& c) { io(ar, c.tick); io(ar, c.point); }
template <typename Ar> void visit(Ar& ar, Cache& c) { io(ar, c.event); }

template <typename Ar>
void visit(Ar& ar, Lead& c) {
    io(ar, c.event);
    io(ar, c.site);
    io(ar, c.since);
    io(ar, c.source);
    io(ar, c.visited);
    io(ar, c.empty);
}

template <typename Ar> void visit(Ar& ar, Leads& c) { ioCounted(ar, c.items, c.count); }

template <typename Ar>
void visit(Ar& ar, Needs& c) {
    io(ar, c.hunger);
    io(ar, c.thirst);
    io(ar, c.fatigue);
    io(ar, c.fear);
    io(ar, c.loneliness);
    io(ar, c.health);
}

template <typename Ar>
void visit(Ar& ar, Traits& c) {
    io(ar, c.bravery);
    io(ar, c.greed);
    io(ar, c.altruism);
    io(ar, c.aggression);
    io(ar, c.leadership);
    io(ar, c.sociability);
    io(ar, c.curiosity);
    io(ar, c.prudence);
    io(ar, c.chronotype);
}

template <typename Ar>
void visit(Ar& ar, Walker& c) {
    io(ar, c.speed);
    io(ar, c.pace);
    ioVector(ar, c.waypoints);
    io(ar, c.next);
}

template <typename Ar> void visit(Ar& ar, Inventory& c) { ioArray(ar, c.items); }

template <typename Ar> void visit(Ar& ar, Danger& c) { io(ar, c.cell); io(ar, c.until); }

template <typename Ar>
void visit(Ar& ar, Mind& c) {
    io(ar, c.action);
    io(ar, c.phase);
    io(ar, c.score);
    io(ar, c.target);
    io(ar, c.spot);
    io(ar, c.until);
    io(ar, c.nextThink);
    io(ar, c.failures);
    io(ar, c.emptySearches);
    ioArray(ar, c.searched);
    io(ar, c.searchedNext);
    io(ar, c.fledAt);
    ioArray(ar, c.dangers);
}

template <typename Ar> void visit(Ar& ar, Senses& c) { io(ar, c.sight); io(ar, c.hearing); }

template <typename Ar>
void visit(Ar& ar, Lod& c) {
    io(ar, c.level);
    ar.unhashed(c.cause); // для инспектора
    io(ar, c.fullUntil);
    ar.unhashed(c.id);    // копия Id
}

template <typename Ar>
void visit(Ar& ar, Contact& c) {
    io(ar, c.id);
    io(ar, c.x);
    io(ar, c.y);
    io(ar, c.tick);
    io(ar, c.seen);
    io(ar, c.heard);
}

template <typename Ar>
void visit(Ar& ar, Awareness& c) {
    ioCounted(ar, c.contacts, c.count);
    // Тик последнего обновления — в хеше только с LOD: без него обновления идут
    // ровно через kPerceptionIntervalTicks, поле ничего не меняет, а хеш
    // остаётся прежним (этап 6а).
    if (ar.lod()) {
        io(ar, c.updated);
    } else {
        ar.unhashed(c.updated);
    }
    ar.unhashed(c.window); // производное от Lod::level
}

template <typename Ar>
void visit(Ar& ar, Zombie& c) {
    io(ar, c.state);
    io(ar, c.target);
    io(ar, c.goal);
    io(ar, c.anchor);
    io(ar, c.horde);
    io(ar, c.until);
    io(ar, c.checked);
    io(ar, c.alerted);
    io(ar, c.lastGroan);
    io(ar, c.health);
}

template <typename Ar>
void visit(Ar& ar, Horde& c) {
    io(ar, c.leader);
    ioVector(ar, c.members);
    io(ar, c.goal);
    io(ar, c.travelling);
    io(ar, c.until);
    io(ar, c.x);
    io(ar, c.y);
    io(ar, c.settlement);
}

template <typename Ar>
void visit(Ar& ar, Animal& c) {
    io(ar, c.kind);
    io(ar, c.state);
    io(ar, c.anchor);
    io(ar, c.goal);
    io(ar, c.until);
    io(ar, c.threat);
    io(ar, c.health);
    io(ar, c.hurtAt);
    io(ar, c.hurtBy);
}

template <typename Ar>
void visit(Ar& ar, Flock& c) {
    io(ar, c.birds);
    io(ar, c.flying);
    io(ar, c.toX);
    io(ar, c.toY);
    io(ar, c.until);
}

template <typename Ar> void visit(Ar& ar, Weapon& c) { io(ar, c.kind); io(ar, c.ammo); }
template <typename Ar> void visit(Ar& ar, Combat& c) { io(ar, c.target); io(ar, c.ready); }

template <typename Ar>
void visit(Ar& ar, Wounds& c) {
    io(ar, c.bleeding);
    io(ar, c.bites);
    io(ar, c.infection);
    io(ar, c.hurtAt);
    io(ar, c.hurtBy);
    io(ar, c.route);
    io(ar, c.infectedBy);
    io(ar, c.infectedAt);
}

template <typename Ar>
void visit(Ar& ar, Bond& c) {
    io(ar, c.other);
    io(ar, c.familiarity);
    io(ar, c.affinity);
    io(ar, c.trust);
    io(ar, c.chemistry);
    io(ar, c.met);
    io(ar, c.seen);
    io(ar, c.flags);
    io(ar, c.debt);
    io(ar, c.grudge);
    io(ar, c.origin);
}

template <typename Ar> void visit(Ar& ar, Relations& c) { ioCounted(ar, c.bonds, c.count); }

template <typename Ar>
void visit(Ar& ar, Memory& c) {
    io(ar, c.kind);
    io(ar, c.who);
    io(ar, c.victim);
    io(ar, c.about);
    io(ar, c.source);
    io(ar, c.tick);
    io(ar, c.strength);
}

template <typename Ar> void visit(Ar& ar, Memories& c) { ioCounted(ar, c.items, c.count); }

template <typename Ar>
void visit(Ar& ar, Corpse& c) {
    io(ar, c.kind);
    io(ar, c.animal);
    io(ar, c.cause);
    io(ar, c.killer);
    io(ar, c.diedAt);
    io(ar, c.meat);
    io(ar, c.turnsAt);
}

// --- Состояние мира вне компонентов --------------------------------------------

template <typename Ar>
void visit(Ar& ar, Weather& c) {
    io(ar, c.kind);
    io(ar, c.minutesLeft);
    io(ar, c.cloudCover);
    io(ar, c.precipitation);
    io(ar, c.fog);
    io(ar, c.windSpeed);
    io(ar, c.windTarget);
    io(ar, c.windFrom);
    io(ar, c.anomaly);
    io(ar, c.snowCover);
}

template <typename Ar>
void visit(Ar& ar, Sound& c) {
    io(ar, c.tick);
    io(ar, c.x);
    io(ar, c.y);
    io(ar, c.loudness);
    io(ar, c.source);
    io(ar, c.kind);
}

// Летопись в хеш входит своим накопленным хешем (Chronicle::hash), в файл —
// записями по порядку.
template <typename Ar>
void visit(Ar& ar, ChronicleEvent& c) {
    io(ar, c.tick);
    io(ar, c.kind);
    io(ar, c.subject);
    io(ar, c.object);
    io(ar, c.a);
    io(ar, c.b);
    io(ar, c.third);
}

// --- Архив хеша ----------------------------------------------------------------

// Подаёт поля в Hasher (World::stateHash). Только читает: компоненты ему
// передаются через const_cast, потому что visit один на все архивы.
class HashArchive {
public:
    static constexpr bool kHashing = true;
    static constexpr bool kReading = false;
    static constexpr bool kSchema = false;

    HashArchive(Hasher& hasher, bool lod) : hasher_(hasher), lod_(lod) {}

    template <typename T>
    void value(const T& value) {
        if constexpr (std::is_enum_v<T>) {
            hasher_.add(static_cast<std::underlying_type_t<T>>(value));
        } else {
            hasher_.add(value);
        }
    }
    template <typename T>
    void unhashed(const T&) {}
    void text(const std::string& text) {
        hasher_.add(static_cast<std::uint64_t>(text.size()));
        chars(text);
    }
    void chars(const std::string& text) {
        for (const char c : text) {
            hasher_.add(static_cast<std::uint8_t>(c));
        }
    }
    void length(std::uint64_t length) { hasher_.add(length); }
    // Включён ли LOD у мира: от него зависит, входят ли в хеш поля этапа 6а.
    bool lod() const { return lod_; }

private:
    Hasher& hasher_;
    bool lod_;
};

// Поля значения — в хеш.
template <typename T>
void hashFields(Hasher& hasher, bool lod, const T& value) {
    HashArchive archive(hasher, lod);
    io(archive, const_cast<T&>(value));
}

} // namespace lw
