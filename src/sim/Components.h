#pragma once

#include "sim/Id.h"
#include "sim/Map.h"
#include "sim/Sound.h"

#include <algorithm>
#include <array>
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
    // Дома, в укрытии: выполняет действие у своей двери (этап 5.2). Зомби его
    // не видят, а сам он от зомби не выбегает. Ставит Utility AI.
    bool indoors = false;
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

// Предметы (этап 5.6). Значения попадают в хеш и сохранения: новые добавлять
// только в конец (перед Count). Вес и прочее — sim/Items.h.
enum class ItemKind : std::uint8_t {
    Food,          // порция еды: консервы, крупа, ягоды; один приём пищи
    Water,         // порция воды: бутылка, несколько глотков
    Meat,          // порция сырого мяса: сытно, но портится
    Bandage,       // бинт: останавливает кровотечение
    Medicine,      // лекарства: возвращают здоровье (заражение не лечат)
    PistolAmmo,    // патрон к пистолету
    ShotgunShells, // патрон к дробовику
    RifleAmmo,     // патрон к винтовке
    Knife,
    Axe,
    Pistol,
    Shotgun,
    Rifle,
    Count
};
inline constexpr std::size_t kItemKinds = static_cast<std::size_t>(ItemKind::Count);

// Что лежит в здании, что несёт человек, что осталось при теле: сколько
// предметов каждого вида. Оружие в руках — отдельно (Weapon).
struct Inventory {
    std::array<std::uint16_t, kItemKinds> items{};

    std::uint16_t count(ItemKind kind) const { return items[static_cast<std::size_t>(kind)]; }
    void set(ItemKind kind, int value) {
        items[static_cast<std::size_t>(kind)] =
            static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFF));
    }
    void add(ItemKind kind, int value) { set(kind, count(kind) + value); }
    // Взять до value штук; возвращает, сколько взято.
    int take(ItemKind kind, int value) {
        const int taken = std::min<int>(value, count(kind));
        set(kind, count(kind) - taken);
        return taken;
    }
    // Порций еды: и припасы, и мясо.
    int food() const { return count(ItemKind::Food) + count(ItemKind::Meat); }
    bool empty() const {
        return std::all_of(items.begin(), items.end(), [](std::uint16_t n) { return n == 0; });
    }
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
    Flee,      // бежать от зомби (этап 5.2)
    Fight,     // драться с зомби или стрелять в него (этап 5.4)
    Hunt,      // охотиться: выследить, подстрелить, разделать (этап 5.4)
    Treat,     // перевязаться или принять лекарство (этап 5.6)
    Count
};

// Сначала идти к месту действия, потом выполнять.
enum class ActionPhase : std::uint8_t { Travel, Perform };

// Опасное место: где человек заметил зомби, и до какого тика это помнит.
struct Danger {
    Cell cell;
    std::uint64_t until = 0;
};

// Решения человека: текущее действие и короткая память.
struct Mind {
    // Сколько зданий помнится обысканными: туда за едой не идут.
    static constexpr std::size_t kSearchedMemory = 16;
    // Сколько опасных мест помнится.
    static constexpr std::size_t kDangerMemory = 4;

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
    // Когда последний раз побежал от зомби: бегство меньше чем через час после
    // прошлого — тот же случай, в летопись не пишется.
    std::uint64_t fledAt = 0;
    // Где замечал зомби: туда не ходят, оттуда отходят. Новое место вытесняет
    // то, что забудется раньше всех.
    Danger dangers[kDangerMemory] = {};
};

// Связь человека с другим человеком — его взгляд, не общий: у другого своя
// связь в ответ, и она может быть другой (этап 6.2). Правила — sim/Relations.h.
struct Bond {
    // Отметки для летописи: событие пишется, когда отметка ставится, а снимается
    // она с запасом — так колебание у порога не засоряет журнал.
    static constexpr std::uint8_t kFriend = 1; // считает другом
    static constexpr std::uint8_t kFoe = 2;    // невзлюбил

    EntityId other = kInvalidId;
    float familiarity = 0.0f; // знакомство 0..1: сколько времени провели рядом
    float affinity = 0.0f;    // симпатия −1..1
    float trust = 0.0f;       // доверие −1..1
    // К чему тянется симпатия, когда они вместе: совместимость характеров,
    // у соседей по дому — с надбавкой. Считается при знакомстве.
    float chemistry = 0.0f;
    std::uint64_t met = 0;    // тик знакомства
    std::uint64_t seen = 0;   // тик, когда последний раз были вместе
    std::uint8_t flags = 0;
    // Долг 0..1: чем обязан другому — спас, заступился (этап 6.2).
    float debt = 0.0f;
    // Обида 0..1: что другой ему сделал — обокрал, бросил в беде (этап 6.2).
    float grudge = 0.0f;

    bool friendly() const { return (flags & kFriend) != 0; }
    bool hostile() const { return (flags & kFoe) != 0; }
};

// Кого человек знает. Связи остаются и после смерти другого — память о нём.
// Мест мало: новое знакомство вытесняет самое слабое, если оно почти забыто.
struct Relations {
    static constexpr std::size_t kMaxBonds = 16;

    Bond bonds[kMaxBonds] = {};
    std::uint8_t count = 0;

    const Bond* find(EntityId id) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (bonds[i].other == id) {
                return &bonds[i];
            }
        }
        return nullptr;
    }
    Bond* find(EntityId id) {
        return const_cast<Bond*>(static_cast<const Relations*>(this)->find(id));
    }
};

// Что человек помнит о других (этап 6.1). victim — с кем это сделали:
// kInvalidId — с ним самим (этап 6.3: можно помнить и чужие поступки).
// Значения попадают в хеш и сохранения: новые добавлять только в конец (перед Count).
enum class MemoryKind : std::uint8_t {
    Saved,     // who убил зомби about, который напал на victim
    Defended,  // who дрался с зомби about, который напал на victim
    Abandoned, // who убежал, когда на victim напал зомби about
    Robbed,    // who обыскивал дом about, где живёт victim
    Bereaved,  // близкий who погиб; about — убийца, если есть
    Turned,    // близкий who встал зомби
    Count
};

// Воспоминание: что случилось, с кем и когда, видел ли сам. Сила угасает со
// временем; слабое забывается (sim/Memory.h).
struct Memory {
    MemoryKind kind = MemoryKind::Saved;
    EntityId who = kInvalidId;    // о ком воспоминание: кто это сделал
    EntityId victim = kInvalidId; // с кем; kInvalidId — с самим помнящим (этап 6.3)
    EntityId about = kInvalidId;  // при чём: зомби, здание, убийца
    EntityId source = kInvalidId; // кто рассказал; kInvalidId — видел сам (этап 6.3)
    std::uint64_t tick = 0;       // когда случилось (повторилось — последний раз)
    float strength = 0.0f;        // 0..1
};

// Память человека о значимых событиях с участием других. Мест мало: новое
// воспоминание вытесняет самое слабое, если оно слабее нового.
struct Memories {
    static constexpr std::size_t kMaxMemories = 16;

    Memory items[kMaxMemories] = {};
    std::uint8_t count = 0;

    // Воспоминание о том, что who сделал с victim (kInvalidId — с самим помнящим).
    const Memory* find(MemoryKind kind, EntityId who, EntityId victim = kInvalidId) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (items[i].kind == kind && items[i].who == who && items[i].victim == victim) {
                return &items[i];
            }
        }
        return nullptr;
    }
    Memory* find(MemoryKind kind, EntityId who, EntityId victim = kInvalidId) {
        return const_cast<Memory*>(static_cast<const Memories*>(this)->find(kind, who, victim));
    }
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
    // Темп — множитель скорости: человек бежит (kRunPace), зомби бредёт
    // медленнее своей скорости погони. Выставляет тот, кто решает, куда идти.
    float pace = 1.0f;
    // Сглаженный путь из Pathfinder: точки — центры клеток. Пусто — стоит на месте.
    std::vector<Cell> waypoints;
    // Номер точки, к которой идёт.
    std::uint32_t next = 0;

    bool moving() const { return next < waypoints.size(); }
};

// Что делает зомби (ZombieSystem). Значения попадают в хеш и сохранения: новые
// добавлять только в конец (перед Count).
enum class ZombieState : std::uint8_t {
    Idle,        // стоит, покачиваясь
    Roam,        // бредёт по родным местам
    Investigate, // идёт туда, где слышал или видел добычу
    Chase,       // преследует того, кого видит
    Follow,      // идёт с ордой за вожаком
    Count
};

// Зомби. Нужд и черт нет: решает по Awareness, что видит и слышит.
struct Zombie {
    ZombieState state = ZombieState::Idle;
    EntityId target = kInvalidId; // кого преследует (Chase)
    Cell goal;                    // куда идёт
    // Родные места: одиночка бродит вокруг них (там, где обратился).
    Cell anchor;
    EntityId horde = kInvalidId;  // орда; kInvalidId — одиночка
    std::uint64_t until = 0;      // до какого тика стоять (Idle, на месте шума)
    // Тик, когда дошёл до места последнего шума: всё замеченное раньше —
    // уже проверено.
    std::uint64_t checked = 0;
    // Когда последний раз видел или слышал добычу: возбуждённые одиночки,
    // сбежавшиеся на шум, сбиваются в орду.
    std::uint64_t alerted = 0;
    std::uint64_t lastGroan = 0;  // когда последний раз созывал стоном
    float health = 1.0f;          // 0 — упокоен (этап 5.4)
};

// Орда: зомби, бредущие вместе за вожаком. Своей позиции в индексе нет — это
// не существо, а связь между существами; центр считается по участникам.
struct Horde {
    EntityId leader = kInvalidId;
    std::vector<EntityId> members; // по возрастанию id, вожак тоже здесь
    Cell goal;                     // куда ведёт вожак
    bool travelling = false;       // идёт к цели (иначе стоит у неё)
    std::uint64_t until = 0;       // до какого тика стоять у цели
    float x = 0.0f;                // центр участников, метры
    float y = 0.0f;
    EntityId settlement = kInvalidId; // поселение, где орда сейчас
};

// Животные (AnimalSystem, этап 5.3). Значения попадают в хеш и сохранения:
// новые добавлять только в конец (перед Count).
enum class AnimalKind : std::uint8_t {
    Deer, // олень: стадами, в лесу, пуглив, бегает быстрее всех
    Boar, // кабан: по одному-трое, в лесу, подпускает ближе
    Hare, // заяц: в полях и на лугах, мелкий и незаметный
    Count
};

enum class AnimalState : std::uint8_t {
    Graze, // пасётся на месте
    Roam,  // переходит к другому месту у своих мест
    Flee,  // убегает от человека или зомби
    Charge, // бросается на обидчика (кабан, этап 5.4)
    Count
};

// Животное. Нужд нет: пасётся у родных мест и убегает от тех, кого боится
// (людей и зомби), по Awareness.
struct Animal {
    AnimalKind kind = AnimalKind::Deer;
    AnimalState state = AnimalState::Graze;
    Cell anchor;                  // родные места: пасётся вокруг них
    Cell goal;                    // куда идёт или бежит
    std::uint64_t until = 0;      // до какого тика пастись на месте (бросаться — Charge)
    EntityId threat = kInvalidId; // от кого бежит (Flee) или на кого бросается (Charge)
    float health = 1.0f;          // 0 — погибло (этап 5.4)
    std::uint64_t hurtAt = 0;     // когда последний раз ранено
    EntityId hurtBy = kInvalidId; // кем
};

// Стая птиц (вороны). Не существо: восприятия у неё нет и глазами её не
// замечают, но вспугнутая она взлетает с криком, который слышно далеко.
// Летит по прямой над чем угодно, без Walker: двигает её AnimalSystem.
struct Flock {
    std::uint8_t birds = 0;
    bool flying = false;
    float toX = 0.0f;        // куда летит, метры
    float toY = 0.0f;
    std::uint64_t until = 0; // сидит до этого тика, потом перелетает сама
};

// Оружие (этап 5.4). Значения попадают в хеш и сохранения: новые добавлять
// только в конец (перед Count). Свойства — sim/Weapons.h.
enum class WeaponKind : std::uint8_t {
    Fists,
    Knife,
    Axe,
    Pistol,
    Shotgun,
    Rifle,
    Count
};

// Оружие в руках человека и патроны к нему.
struct Weapon {
    WeaponKind kind = WeaponKind::Fists;
    std::uint16_t ammo = 0;
};

// Кого бьёт или в кого стреляет существо (CombatSystem). Цель выставляет тот,
// кто решает (Utility AI, ZombieSystem, AnimalSystem); удар или выстрел
// случается, когда цель в досягаемости и оружие готово.
struct Combat {
    EntityId target = kInvalidId;
    std::uint64_t ready = 0; // с какого тика можно бить снова
};

// Раны человека (этап 5.4): кровотечение отнимает здоровье, пока не
// остановится само; укусы зомби заражают (этап 5.5).
struct Wounds {
    float bleeding = 0.0f;           // здоровья в игровой час
    std::uint8_t bites = 0;          // сколько раз укушен
    float infection = 0.0f;          // заражение от укуса: 0..1, на пределе — смерть
    std::uint64_t hurtAt = 0;        // когда последний раз ранен
    EntityId hurtBy = kInvalidId;    // кем
};

// Отчего наступила смерть. Значения попадают в хеш и сохранения: новые
// добавлять только в конец (перед Count).
enum class DeathCause : std::uint8_t {
    Wounds,      // убит в бою
    Bleeding,    // истёк кровью
    Starvation,  // умер от голода (этап 5.5)
    Dehydration, // умер от жажды
    Exhaustion,  // умер от изнеможения
    Infection,   // сгорел от заражения после укуса
    Count
};

// Чьё тело.
enum class CorpseKind : std::uint8_t { Human, Zombie, Animal, Count };

// Тело погибшего (этап 5.4). Сущность та же, что была при жизни, с тем же id:
// у неё остаются позиция и то, что было при себе, а поведение снимается.
struct Corpse {
    CorpseKind kind = CorpseKind::Human;
    AnimalKind animal = AnimalKind::Deer; // для туши
    DeathCause cause = DeathCause::Wounds;
    EntityId killer = kInvalidId;
    std::uint64_t diedAt = 0;
    std::uint8_t meat = 0; // порций мяса в туше: её можно разделать
    // Укушенный встанет зомби в этот тик (этап 5.5); 0 — не встанет.
    std::uint64_t turnsAt = 0;
};

} // namespace lw
