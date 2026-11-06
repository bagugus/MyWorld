#pragma once

#include "sim/Chronicle.h"
#include "sim/Id.h"

#include <cstdint>
#include <vector>

namespace lw {

class World;

// Предвкушение (этап 12а.2): что вот-вот случится. Летопись знает только
// свершившееся, а режиссёру нужно успеть к кульминации — поэтому здесь по
// нынешнему состоянию мира видно, к чему идёт дело: мститель идёт к обидчику,
// орда — к поселению с людьми, укушенный — среди людей, двое идут к одному
// грузу, вор — к чужому дому.
//
// Предвестия — производные данные: только читают мир, в World::stateHash не
// входят и на решения существ не влияют. Считать их можно хоть каждый кадр, но
// хватает пары раз в секунду.

// Вид предвестия. Порядок — для сортировки и имён.
enum class ForesightKind : std::uint8_t {
    Revenge,   // мститель идёт к обидчику (Action::Avenge)
    Raid,      // орда идёт к поселению, где есть люди
    Turning,   // укушенный среди людей скоро обратится (или его тело вот-вот встанет)
    CacheRace, // несколько человек идут к одному грузу ивента
    Theft,     // вор идёт к чужому дому или сбегает с запасами группы (Action::Steal)
    Count
};

struct Foresight {
    ForesightKind kind = ForesightKind::Revenge;
    // За кем следить: мститель, вожак орды, укушенный (или его тело), кто
    // ближе всех к грузу, вор.
    EntityId hero = kInvalidId;
    // С кем столкнётся: обидчик, поселение, второй претендент на груз, хозяин
    // дома (у беглеца — вожак группы); kInvalidId — нет.
    EntityId other = kInvalidId;
    // О чём: орда, ивент, дом; kInvalidId — нет.
    EntityId focus = kInvalidId;
    float x = 0.0f; // где случится, метры
    float y = 0.0f;
    float distance = 0.0f; // сколько герою осталось пройти, метры (обращение — 0)
    float eta = 0.0f;      // через сколько секунд симуляции кульминация (оценка)
    // Сколько людей под угрозой или в гонке: в поселении, рядом с укушенным,
    // претендентов на груз.
    int crowd = 0;
    // Насколько интересно сейчас, в тех же единицах, что storyInterest:
    // растёт по мере приближения кульминации.
    float interest = 0.0f;

    // Одно и то же предвестие от раза к разу: тот же вид и о чём, и тот же
    // герой. Гонка — за тот же груз, кто бы ни вёл (обгон — часть гонки);
    // орда — та же к тому же поселению.
    bool same(const Foresight& that) const {
        if (kind != that.kind || focus != that.focus) {
            return false;
        }
        switch (kind) {
        case ForesightKind::CacheRace:
            return true;
        case ForesightKind::Raid:
            return other == that.other;
        default:
            return hero == that.hero;
        }
    }
};

// Все предвестия мира по убыванию интересности (при равной — по виду и id
// героя). Только читает мир.
std::vector<Foresight> foresee(const World& world);

// Сбылось ли предвестие этим событием летописи: мститель отомстил (или
// схватился с обидчиком), орда пришла в поселение, укушенный встал зомби,
// претендент забрал груз, вор обокрал дом или сбежал с запасами.
bool fulfilledBy(const World& world, const Foresight& foresight, const ChronicleEvent& event);

// Короткое английское имя — для lw_headless.
const char* foresightKindName(ForesightKind kind);

} // namespace lw
