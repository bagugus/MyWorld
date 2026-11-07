#pragma once

#include "sim/Components.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace lw {

class World;

// Сценарий и фазы мира (этап 7.4). Мир начинается с мирной жизни; сценарий —
// то, как в нём разворачивается катастрофа. Первый (пока единственный) —
// зомби-вирус: мир → вспышка → паника → коллапс → выживание.
//
// Фаза не решает за людей — она меняет правила, на которые опираются системы:
// возят ли товар в магазины и больницу, платят ли зарплату, течёт ли вода из
// крана, кто ещё выходит на работу (а значит, открыт ли магазин, лечат ли в
// больнице, есть ли на улицах полиция), считается ли кража преступлением,
// сбиваются ли люди в группы, сколько зомби приходит издалека. Дальше — как
// всегда: люди решают по нуждам, чертам и тому, что видят.
//
// Фазы идут только вперёд. Вспышку начинает расписание (WorldConfig::
// outbreakDays) или наблюдатель; следующие наступают сами — когда беда дошла
// до порога, но не раньше и не позже своего срока (PhaseRules). Подробности и
// числа — docs/architecture.md, раздел «Сценарий и фазы мира».

inline constexpr std::size_t kEventKinds = static_cast<std::size_t>(EventKind::Count);

// Правила фазы.
struct PhaseRules {
    const char* name = ""; // короткое английское имя — для lw_headless и --phase
    // Завоз в магазины и больницу: доля обычного запаса; 0 — не возят.
    float supply = 1.0f;
    bool economy = true;  // платят зарплату и пособие
    bool tapWater = true; // водопровод
    // Кто выходит на работу: профессии (бит 1 << Profession) с чувством долга
    // (dutyOf) не ниже dutyNeeded. Остальные работу бросили.
    std::uint32_t professions = 0;
    float dutyNeeded = 0.0f;
    // Кража и нападение — преступление: лезть в чужое, красть и мстить совестно
    // и страшно (Utility AI), а свидетель осуждает и тогда, когда пострадал
    // чужой (Memory).
    bool lawful = true;
    float eveningHome = 0.0f; // вечером тянет домой (Utility AI, «Домой»); 0 — нет
    float stockpile = 1.0f;   // сколько еды держать дома — × это: в панике скупают
    bool groups = false;      // сбиваются в группы выживших (GroupSystem)
    // Приток зомби издалека (этап 5.7): их число держится не ниже zombieShare
    // от жителей к началу вспышки; орда приходит arrivals раз в сутки. Зомби
    // во вспышке и панике — только свои, бывшие жители (этап 7.5): приток
    // начинается с коллапсом, когда беда пришла и к соседям.
    float zombieShare = 0.0f;
    int arrivals = 0;
    // Ивенты фазы (этап 7.7): сколько в среднем в сутки каждого вида
    // (по EventKind), днём (EventSystem). В мирное время и во вспышке с неба
    // ничего не падает и по дорогам не ездят колонны; в панике власть
    // сбрасывает гуманитарные грузы (чаще у поселений на карантине) и гонит
    // колонны к пунктам эвакуации; в коллапсе колонны застревают, вертолёты
    // падают; в выживании — всё реже: армии почти не осталось.
    float eventsPerDay[kEventKinds] = {};
    // Когда фаза уступает следующей: не раньше minHours игровых часов после
    // начала; если пострадавших (погибли, обратились, заметно больны — укушены
    // или в лихорадке) — не меньше trigger от жителей к началу вспышки и не
    // меньше triggerMin человек, — то сразу, иначе — через maxHours.
    // maxHours = 0 — сама не кончается.
    int minHours = 0;
    int maxHours = 0;
    float trigger = 0.0f;
    int triggerMin = 0;
};

const PhaseRules& phaseRules(WorldPhase phase);
// Фаза по имени из phaseRules; nullopt — такой нет.
std::optional<WorldPhase> phaseByName(const char* name);
// Следующая фаза; у выживания — она же.
WorldPhase nextPhase(WorldPhase phase);

// Сценарий мира; nullptr — его нет (мир начат без мирной жизни).
const Scenario* scenarioOf(const World& world);
// Id сущности сценария; kInvalidId — нет.
EntityId scenarioIdOf(const World& world);
// Нулевой пациент ли id (этап 7.5): с него началась вспышка. Завозные случаи
// после него — нет.
bool isPatientZero(const World& world, EntityId id);

// Фаза мира: без сценария — выживание (катастрофа уже случилась).
WorldPhase worldPhase(const World& world);
const PhaseRules& rulesOf(const World& world);

// Чувство долга работника, 0..1: насколько он держится за работу, когда вокруг
// беда. Врач — альтруизм и смелость, полицейский — смелость и лидерство,
// продавец — жадность (в панике торговля бойкая) и смелость, фермер —
// запасливость и смелость, учитель — альтруизм и смелость.
float dutyOf(Profession profession, const Traits& traits);
// Выходит ли работник на работу в этой фазе.
bool keepsWorking(WorldPhase phase, Profession profession, const Traits& traits);
// То же для человека в мире; нет работы — false.
bool keepsWorking(const World& world, entt::entity person);

// Места работы, где хоть один живой работник ещё выходит на работу в этой
// фазе, — по возрастанию id. Только туда возят товар (WorkSystem).
std::vector<EntityId> mannedWorkplaces(const World& world);

// Полицейский на службе (этап 7.4): в смену и не спит, фаза ему это позволяет,
// и он в обходе или уже в бою. Заступается за любого, на кого напали первым
// (угроза — threatOf и isAssault, решимость — Utility AI).
bool policeOnDuty(const World& world, entt::entity person);

// Сколько зомби приток издалека (этап 5.7) держит в мире: без сценария —
// WorldConfig::zombies, со сценарием — не меньше доли фазы от жителей к
// началу вспышки.
int zombieTarget(const World& world);
// Сколько раз в сутки (поровну через день, с полуночи) приходит орда издалека.
int arrivalsPerDay(const World& world);

// Беда в цифрах — для переходов фаз, интерфейса и lw_headless.
struct ScenarioStats {
    int population = 0; // людей к началу вспышки (до неё — сейчас)
    int alive = 0;      // живых людей (с солдатами)
    int infected = 0;   // из них заражены — укусом или от больного (этап 7.5)
    int sick = 0;       // из заражённых заметно больны: укушены или в лихорадке
    int lost = 0;       // погибли и обратились с начала вспышки (жители)
    int zombies = 0;
    int residents = 0;  // из зомби — бывшие жители (с именем); остальные пришли издалека
    int evacuated = 0;  // жителей увезли из края (этап 7.6): не потеряны
    int soldiers = 0;   // из живых — солдаты: пришли извне, не жители
    // Пострадавшие: потерянные и заметно больные. Скрытый период не замечает
    // никто — он беду ещё не торопит.
    int casualties() const { return lost + sick; }
};
ScenarioStats scenarioStats(const World& world);

// Тик, не раньше которого фаза может смениться; Scenario::kNever — сама не
// кончается. Не позже — phaseDeadline.
std::uint64_t phaseEarliest(const Scenario& scenario);
std::uint64_t phaseDeadline(const Scenario& scenario);
// Сколько пострадавших переводит фазу в следующую раньше срока; 0 — порога нет.
int phaseTrigger(const Scenario& scenario);

} // namespace lw
