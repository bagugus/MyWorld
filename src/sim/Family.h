#pragma once

#include "sim/Components.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace lw {

class Rng;
class World;

// Семьи и прошлое (этап 7.3). Подробности и числа — docs/architecture.md,
// раздел «Семьи и прошлое».
//
// Мир рождается не из случайных жильцов, а из домохозяйств: одинокие, пары,
// семьи с детьми, родитель-одиночка с детьми, три поколения под одной крышей,
// пожилые пары, соседи по квартире или взрослые братья и сёстры. Часть родни
// живёт отдельно: выросшие дети пожилых — в своих домах, иногда в другом
// поселении, их дети — внуки. Родня знает друг друга всю жизнь (связь
// BondOrigin::Kin: полное знакомство, не забывается и не вытесняется), коллеги
// и одноклассники знакомы по работе и школе, а у части людей есть прошлое:
// давний друг, давняя вражда с соседом, бывший супруг.

// --- Возраст и родство ---------------------------------------------------

// Возраст человека; без Age — 30 лет.
std::uint8_t ageOf(const entt::registry& registry, entt::entity entity);
// Несовершеннолетний: младше kAdultAge.
bool isMinor(const entt::registry& registry, entt::entity entity);
// Кем other приходится self; KinKind::Count — не родня.
KinKind kinOf(const entt::registry& registry, entt::entity self, EntityId other);
// Кем self приходится тому, кто для него kind: родитель — ребёнку и т.д.
KinKind inverseKin(KinKind kind);
// Код родства для летописи (ChronicleEvent::a или b): 0 — не родня, иначе
// 1 + 2 × KinKind + 1, если other — женщина (по имени). Так текст события
// может сказать «сына» или «дочь», не заглядывая в мир.
std::int32_t kinCode(const World& world, entt::entity self, EntityId other);
// Разобрать код: false — не родня.
bool decodeKin(std::int32_t code, KinKind& kind, bool& female);
// Живой несовершеннолетний ребёнок, о котором self заботится: его опекун —
// self или self — его родитель.
bool caresFor(const World& world, entt::entity self, EntityId child);

// --- Состав домов при рождении мира -------------------------------------

// Виды домохозяйств — для сводок и тестов.
enum class HouseholdKind : std::uint8_t {
    Single,      // один взрослый
    Couple,      // пара без детей дома
    Family,      // пара с детьми
    SingleParent, // родитель с детьми
    ThreeGenerations, // пара с детьми и дед или бабушка
    Elderly,     // пожилая пара
    Sharing,     // двое взрослых: соседи по квартире или брат с сестрой
    Count
};

// План людей до их появления в мире: возраст, пол, дом, фамилия (род) и
// родство. Чистые данные — без мира, их проверяют тесты.
struct PlannedPerson {
    std::uint8_t age = 30;
    bool female = false;
    int household = 0; // номер домохозяйства
    int lineage = 0;   // род: у одного рода одна фамилия
};
struct PlannedKin {
    int a = 0;
    int b = 0;
    KinKind kind = KinKind::Spouse; // кем b приходится a
};
struct HouseholdPlan {
    std::vector<PlannedPerson> people;
    std::vector<HouseholdKind> households;
    // Каждая связь — в обе стороны: (a, b, kind) и (b, a, inverseKin(kind)).
    std::vector<PlannedKin> kin;
    // Опекун несовершеннолетнего — номер человека; −1 — нет (взрослый).
    std::vector<int> guardian;
    int lineages = 0;
};

// Ровно count человек домохозяйствами (последнее может выйти неполным).
// Выросшие дети пожилых живут отдельно — родство тянется между домами.
HouseholdPlan planHouseholds(Rng& rng, int count);

// --- Знакомства с начала мира --------------------------------------------

// Родня знает друг друга всю жизнь: знакомство полное (живущие порознь — чуть
// меньше), симпатия — совместимость с надбавкой родства (у супругов и
// родителей с детьми — больше всего), доверие — по симпатии. Связь
// BondOrigin::Kin. Звать до introduceNeighbours: соседям по дому родня
// достаётся первой.
void introduceFamilies(World& world);

// Коллеги — работающие в одном здании — и одноклассники (школьники одной
// школы близкого возраста, учитель и ученики) знакомы. После assignJobs.
// Рандома нет.
void introduceColleagues(World& world);

// Прошлое: давние друзья, давняя вражда с соседом, бывшие супруги — у части
// взрослых (поток RngStream::Families). После introduceNeighbours.
void weavePast(World& world);

} // namespace lw
