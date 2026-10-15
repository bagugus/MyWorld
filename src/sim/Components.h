#pragma once

#include "sim/Id.h"
#include "sim/Map.h"
#include "sim/Sound.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lw {

// Единица длины мира — метр, времени — секунда.

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

// Метры в секунду: фактическая скорость за последний тик — смещение за тик,
// делённое на kTickDt. По ней отрисовка восстанавливает позицию на начало тика
// и плавно ведёт сущность между тиками, поэтому каждая система, которая
// двигает сущности, должна так её и выставлять.
struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
};

// Значения перечислений попадают в хеш мира и в сохранения:
// новые добавлять только в конец.
enum class SettlementKind : std::uint8_t { Village, Town };
enum class BuildingKind : std::uint8_t { House, Barn, Apartment, Shop };

// Поселение: круг клеток вокруг центра. Центр всегда лежит на дороге.
struct Settlement {
    SettlementKind kind = SettlementKind::Village;
    std::string name;
    Cell center;
    int radius = 0; // клетки
};

// Здание занимает прямоугольник клеток [origin, origin + size).
struct Building {
    BuildingKind kind = BuildingKind::House;
    Cell origin;
    int width = 0;  // клетки по x
    int height = 0; // клетки по y
    // Клетка перед входом: снаружи, вплотную к зданию, со стороны дороги.
    Cell door;
    EntityId settlement = kInvalidId;
};

// Человек. Дом — жилое здание, у двери которого он ночует; kInvalidId — бездомный.
struct Person {
    EntityId home = kInvalidId;
    // Спит: нужды меняются медленнее, усталость уходит.
    bool asleep = false;
};

// Нужды человека (NeedsSystem). Голод, жажда, усталость, страх, одиночество —
// от 0 (всё хорошо) до 1 (предел): на пределе голода, жажды и усталости уходит
// здоровье. Здоровье — от 1 (здоров) до 0.
struct Needs {
    float hunger = 0.0f;
    float thirst = 0.0f;
    float fatigue = 0.0f;
    float fear = 0.0f;
    float loneliness = 0.0f;
    float health = 1.0f;
};

// Черты характера, 0..1; 0,5 — средний человек. Даются при рождении (sim/Traits.h)
// и пока не меняются. Часть черт ждёт своих систем: альтруизм, агрессия и
// лидерство заработают с отношениями и группами (этапы 5–6).
struct Traits {
    float bravery = 0.5f;     // смелость: меньше страх, не боится темноты и дальних дорог
    float greed = 0.5f;       // жадность: берёт больше припасов
    float altruism = 0.5f;    // альтруизм: помогает, делится
    float aggression = 0.5f;  // агрессия: драки, грабёж
    float leadership = 0.5f;  // лидерство: ведёт за собой
    float sociability = 0.5f; // общительность: тяжелее одиночество, ищет компанию
    float curiosity = 0.5f;   // любопытство: прогулки, дальние места
    float prudence = 0.5f;    // запасливость: ест и пьёт заранее, носит еду домой
    float chronotype = 0.5f;  // 0 — жаворонок, 1 — сова: время сна сдвигается на ±1,5 ч
};

// Припасы здания, порции: одна порция еды — один приём пищи, воды — несколько
// глотков. Их съедают жильцы и уносят те, кто ищет еду.
struct Supplies {
    std::uint16_t food = 0;
    std::uint16_t water = 0;
};

// Что человек несёт с собой: найденное в чужих домах и в лесу. Дома
// выкладывается в его запасы. Временно, до инвентаря (этап 5).
struct Carry {
    std::uint8_t food = 0;
    std::uint8_t water = 0;
};

// Действия Utility AI (UtilityAiSystem). Значения попадают в хеш и сохранения:
// новые добавлять только в конец (перед Count).
enum class Action : std::uint8_t {
    Idle,      // отдых на месте
    Sleep,     // сон дома, а вдали от дома — где застала ночь
    Eat,       // поесть: из своего запаса, дома или с собой
    Drink,     // попить: с собой, дома или у реки и озера
    Scavenge,  // обыскать чужое здание: еда и вода
    Forage,    // собирать ягоды и грибы в лесу
    Socialize, // к людям: побыть в компании
    Wander,    // прогулка к зданию своего или соседнего поселения
    GoHome,    // домой, переждать страх
    Count
};

// Сначала идти к месту действия, потом выполнять.
enum class ActionPhase : std::uint8_t { Travel, Perform };

// Решения человека: текущее действие и короткая память.
struct Mind {
    // Сколько зданий помнится обысканными: туда за едой не идут.
    static constexpr std::size_t kSearchedMemory = 16;

    Action action = Action::Idle;
    ActionPhase phase = ActionPhase::Perform;
    float score = 0.0f;               // оценка действия, когда его выбрали
    EntityId target = kInvalidId;     // здание действия (Eat, Drink, Scavenge), если есть
    Cell spot;                        // клетка, где выполняется действие
    std::uint64_t until = 0;          // тик конца выполнения
    std::uint64_t nextThink = 0;      // тик следующего пересмотра решения
    std::uint32_t failures = 0;       // подряд не нашлось пути — для отладки
    // Обыски подряд, где не нашлось еды: округа обобрана.
    std::uint8_t emptySearches = 0;
    // Обысканные здания, кольцевой буфер.
    EntityId searched[kSearchedMemory] = {};
    std::uint8_t searchedNext = 0;
};

// Органы чувств существа (PerceptionSystem). Есть у всех, кто воспринимает
// мир, и только их замечают другие: предметы пока невидимы.
struct Senses {
    // Дальность зрения, метры: днём на открытом месте человека видно настолько.
    // Ночь, туман, лес и стены сокращают её (sim/Perception.h).
    float sight = 150.0f;
    // Острота слуха: множитель громкости всего, что слышно.
    float hearing = 1.0f;
};

// Кого существо заметило: увидело или услышало.
struct Contact {
    EntityId id = kInvalidId;
    float x = 0.0f;           // где заметило в последний раз, метры
    float y = 0.0f;
    std::uint64_t tick = 0;   // когда
    bool seen = false;        // тогда видело
    SoundKind heard = SoundKind::None; // тогда слышало; None — не слышало
};

// Что существо знает об окружающих: несколько самых заметных контактов.
// Обновляется раз в kPerceptionIntervalTicks; потерянный из виду контакт
// помнится ещё kContactMemoryTicks — с местом, где его заметили последний раз.
struct Awareness {
    static constexpr std::size_t kMaxContacts = 8;

    Contact contacts[kMaxContacts] = {};
    std::uint8_t count = 0;

    const Contact* find(EntityId id) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (contacts[i].id == id) {
                return &contacts[i];
            }
        }
        return nullptr;
    }
};

// Движение пешком по пути (MovementSystem).
struct Walker {
    // Скорость по дороге, м/с; по другой местности медленнее (walkSpeedFactor).
    float speed = 1.4f;
    // Сглаженный путь из Pathfinder: точки — центры клеток. Пусто — стоит на месте.
    std::vector<Cell> waypoints;
    // Номер точки, к которой идёт.
    std::uint32_t next = 0;

    bool moving() const { return next < waypoints.size(); }
};

} // namespace lw
