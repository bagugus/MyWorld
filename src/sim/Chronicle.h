#pragma once

#include "sim/Id.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace lw {

// Виды событий летописи. Значения попадают в хеш и сохранения: новые добавлять
// только в конец (перед Count).
enum class ChronicleKind : std::uint8_t {
    Settled,       // появился в мире; object — дом (kInvalidId — бездомный)
    Scavenged,     // обыскал здание object: взял еды a, воды b (оба 0 — пусто)
    BroughtHome,   // принёс в дом object еды a, воды b
    AteLastFood,   // съел последнюю еду в здании object
    DrankLastWater, // выпил последнюю воду в здании object
    SleptOutside,  // уснул вне дома, в клетке (a, b)
    Starving,      // голод дошёл до предела
    Dehydrated,    // жажда дошла до предела
    Exhausted,     // усталость дошла до предела
    Weakened,      // здоровье упало ниже половины
    Dying,         // здоровье упало ниже пятой части
    Recovered,     // здоровье снова выше половины
    Fled,          // побежал от зомби object; a = 1 — проснулся от него
    HordeFormed,   // орда собралась из a зомби; b = 1 — сбежались на шум (0 — с начала мира)
    HordeMerged,   // орда поглотила орду object, стало a зомби
    HordeArrived,  // орда из a зомби пришла в поселение object
    HordeScattered, // орда распалась: осталось меньше двух
    Bitten,        // укушен зомби object
    KilledZombie,  // упокоил зомби object; a — оружие (WeaponKind)
    Hunted,        // добыл животное object; a — вид (AnimalKind)
    Died,          // погиб; object — убийца (если есть), a — причина (DeathCause), b — что за тело (CorpseKind)
    Butchered,     // разделал тушу object: a порций мяса
    Turned,        // встал зомби после смерти от укуса
    Feverish,      // заражение после укуса перешло в лихорадку
    FoundWeapon,   // нашёл в здании или на теле object оружие и взял в руки: a — WeaponKind
    LootedBody,    // обыскал тело object: взял еды a, воды b
    Treated,       // перевязался (a = 0) или принял лекарство (a = 1)
    Count
};

// Запись летописи: что случилось, когда и с кем. Только данные — текст
// составляет интерфейс. Участники — по EntityId: запись переживает сущность.
struct ChronicleEvent {
    std::uint64_t tick = 0;
    ChronicleKind kind = ChronicleKind::Settled;
    EntityId subject = kInvalidId; // кто
    EntityId object = kInvalidId;  // с кем или с чем, если есть
    std::int32_t a = 0;            // подробности — по виду события
    std::int32_t b = 0;
};

// Летопись мира: журнал значимых событий в порядке, в котором они случились.
// По любому id можно получить его биографию — события, где он субъект или
// объект. Пишут системы симуляции; записи не удаляются.
//
// Летопись — часть состояния мира (её хеш входит в World::stateHash), хотя на
// решения существ пока не влияет: позже на неё опираются память и отношения.
class Chronicle {
public:
    void add(const ChronicleEvent& event);

    const std::vector<ChronicleEvent>& events() const { return events_; }
    // Номера событий (в events()) с участием id, по возрастанию.
    std::span<const std::uint32_t> eventsOf(EntityId id) const;
    std::size_t countOf(ChronicleKind kind) const {
        return counts_[static_cast<std::size_t>(kind)];
    }
    // Накопленный хеш всех записей: считать заново при каждом stateHash дорого.
    std::uint64_t hash() const { return hash_; }

private:
    std::vector<ChronicleEvent> events_;
    // Только для поиска по id; обходить нельзя — порядок не детерминирован.
    std::unordered_map<EntityId, std::vector<std::uint32_t>> byId_;
    std::size_t counts_[static_cast<std::size_t>(ChronicleKind::Count)] = {};
    std::uint64_t hash_ = 0xCBF29CE484222325ULL;
};

// Короткое английское имя вида события — для lw_headless и журналов.
const char* chronicleKindName(ChronicleKind kind);

} // namespace lw
