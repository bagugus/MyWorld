#include "sim/Work.h"

#include "sim/Family.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

namespace lw {

namespace {

constexpr int kHour = 60;

// Профессии по порядку Profession: место работы, дневная и вечерняя смены,
// рабочих дней в неделю, с понедельника по пятницу ли, плата за час.
constexpr ProfessionSpec kSpecs[] = {
    {BuildingKind::House, 0, 0, 0, 0, 0, false, 0},                                       // без работы
    {BuildingKind::Shop, 8 * kHour, 18 * kHour, 0, 0, 6, false, 5},                       // продавец
    {BuildingKind::Barn, 6 * kHour, 14 * kHour, 0, 0, 6, false, 4},                       // фермер
    {BuildingKind::Hospital, 8 * kHour, 16 * kHour, 14 * kHour, 22 * kHour, 5, false, 9}, // врач
    {BuildingKind::PoliceStation, 8 * kHour, 16 * kHour, 14 * kHour, 22 * kHour, 5, false, 7},
    {BuildingKind::School, 8 * kHour, 14 * kHour, 0, 0, 5, true, 6},                      // учитель
    {BuildingKind::School, 8 * kHour, 14 * kHour, 0, 0, 5, true, 0},                      // школьник
};
static_assert(std::size(kSpecs) == static_cast<std::size_t>(Profession::Count));

// Сбережения при рождении: от kSavingsMin до kSavingsMax, запасливому — ещё до
// kSavingsPrudence.
constexpr int kSavingsMin = 20;
constexpr int kSavingsMax = 100;
constexpr float kSavingsPrudence = 150.0f;
// Карманные деньги детей (этап 7.3): у маленьких — до kPocketYoung, у
// подростков — до kPocketTeen.
constexpr int kPocketYoung = 5;
constexpr int kPocketTeen = 30;
constexpr int kTeenAge = 12;
// Работают взрослые младше kRetireAge; в школу ходят с kSchoolAge.
constexpr int kRetireAge = 67;
constexpr int kSchoolAge = 6;

// Сколько жителей приходится на одно место работы (больше — больше мест, но
// не больше потолка): врачи и полиция, учителя, продавцы; фермеров — по
// одному на ферму, но не больше половины деревни.
constexpr int kResidentsPerDoctor = 15;
constexpr int kMaxDoctors = 6;
constexpr int kResidentsPerOfficer = 15;
constexpr int kMaxOfficers = 6;
constexpr int kResidentsPerTeacher = 20;
constexpr int kMaxTeachers = 4;
constexpr int kResidentsPerShop = 5;
constexpr int kResidentsPerFarmer = 2;
// Случайная добавка к пригодности, чтобы на работу брали не всегда лучших.
constexpr float kFitJitter = 0.6f;

// Насколько человек подходит для профессии: черты характера.
float fitness(Profession profession, const Traits& t) {
    switch (profession) {
    case Profession::Shopkeeper:
        return t.sociability + 0.3f * t.greed;
    case Profession::Farmer:
        return (1.0f - t.chronotype) + 0.5f * t.prudence; // встаёт рано
    case Profession::Doctor:
        return t.altruism + 0.5f * t.prudence;
    case Profession::PoliceOfficer:
        return t.bravery + 0.5f * t.aggression + 0.3f * t.leadership;
    case Profession::Teacher:
        return t.sociability + 0.5f * t.curiosity + 0.3f * t.leadership;
    default:
        return 0.0f;
    }
}

float distanceSq(Cell a, Cell b) {
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    return dx * dx + dy * dy;
}

} // namespace

const ProfessionSpec& professionSpec(Profession profession) {
    const auto index = static_cast<std::size_t>(profession);
    return kSpecs[index < std::size(kSpecs) ? index : 0];
}

int priceOf(ItemKind kind) {
    switch (kind) {
    case ItemKind::Food:
        return kFoodPrice;
    case ItemKind::Water:
        return 1;
    case ItemKind::Bandage:
        return 2;
    case ItemKind::Medicine:
        return 6;
    default:
        return 0;
    }
}

bool isWorkday(const Job& job, int dayOfWeek) {
    return ((job.days >> dayOfWeek) & 1) != 0;
}

bool onShift(const Job& job, const CalendarTime& time) {
    return job.profession != Profession::None && isWorkday(job, time.dayOfWeek) &&
           time.minuteOfDay >= job.start && time.minuteOfDay < job.end;
}

std::uint64_t shiftEndTick(const Job& job, std::uint64_t tick) {
    const CalendarTime time = calendarAt(tick);
    if (job.profession == Profession::None || !isWorkday(job, time.dayOfWeek) ||
        time.minuteOfDay >= job.end) {
        return tick;
    }
    const std::uint64_t dayStart = time.gameTick - time.gameTick % kTicksPerGameDay;
    return dayStart + static_cast<std::uint64_t>(job.end) * kTicksPerGameMinute - kStartGameTick;
}

bool isOnDuty(const World& world, entt::entity entity) {
    const entt::registry& registry = world.registry();
    const auto* person = registry.try_get<Person>(entity);
    const auto* job = registry.try_get<Job>(entity);
    const auto* mind = registry.try_get<Mind>(entity);
    if (person == nullptr || job == nullptr || mind == nullptr || person->asleep ||
        !onShift(*job, calendarAt(world.tick()))) {
        return false;
    }
    const entt::entity workplace = world.find(job->workplace);
    if (workplace == entt::null) {
        return false;
    }
    const bool working = mind->action == Action::Work;
    switch (job->profession) {
    case Profession::PoliceOfficer:
        return working;
    case Profession::Farmer:
        if (working && mind->phase == ActionPhase::Perform) {
            return true;
        }
        break;
    default:
        break;
    }
    const Position& at = registry.get<Position>(entity);
    return Map::cellAt(at.x, at.y) == registry.get<Building>(workplace).door;
}

bool isOpen(const World& world, EntityId building) {
    const entt::entity entity = world.find(building);
    const auto* workplace = entity != entt::null ? world.registry().try_get<Workplace>(entity) : nullptr;
    return workplace != nullptr && workplace->onDuty > 0;
}

bool policeNearby(const World& world, float x, float y, float radius) {
    const entt::registry& registry = world.registry();
    return world.spatial()
        .nearest(x, y, radius,
                 [&](const SpatialHit& hit) {
                     const auto* job = registry.try_get<Job>(hit.entity);
                     return job != nullptr && job->profession == Profession::PoliceOfficer &&
                            isOnDuty(world, hit.entity);
                 })
        .has_value();
}

void assignJobs(World& world) {
    Rng& rng = world.rng(RngStream::Jobs);
    entt::registry& registry = world.registry();

    std::vector<std::pair<EntityId, entt::entity>> people;
    for (const auto [entity, id, person] : registry.view<const Id, const Person>().each()) {
        people.emplace_back(id.value, entity);
    }
    std::sort(people.begin(), people.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    static const Traits kAverage{};
    const auto traitsOf = [&](entt::entity entity) -> const Traits& {
        const auto* traits = registry.try_get<Traits>(entity);
        return traits != nullptr ? *traits : kAverage;
    };
    for (const auto& [id, entity] : people) {
        const int age = ageOf(registry, entity);
        const int savings = age < kTeenAge  ? rng.rangeInt(0, kPocketYoung)
                            : age < kAdultAge ? rng.rangeInt(0, kPocketTeen)
                                              : rng.rangeInt(kSavingsMin, kSavingsMax) +
                                                    static_cast<int>(std::lround(kSavingsPrudence *
                                                                                 traitsOf(entity).prudence));
        registry.emplace_or_replace<Wallet>(entity, Wallet{savings});
    }

    for (const Places::SettlementPlace& place : world.places().settlements()) {
        // Жители — по id; бездомные работы не получают.
        std::vector<std::pair<EntityId, entt::entity>> residents;
        for (const auto& [id, entity] : people) {
            const entt::entity home = world.find(registry.get<Person>(entity).home);
            if (home != entt::null && registry.get<Building>(home).settlement == place.id) {
                residents.emplace_back(id, entity);
            }
        }
        const int count = static_cast<int>(residents.size());
        if (count == 0) {
            continue;
        }
        // Здания вида kind — от ближайшего к центру, при равенстве — по id.
        const auto ofKind = [&](BuildingKind kind) {
            std::vector<const Places::BuildingPlace*> result;
            for (const Places::BuildingPlace& building : place.buildings) {
                if (building.kind == kind) {
                    result.push_back(&building);
                }
            }
            std::stable_sort(result.begin(), result.end(), [&](const auto* a, const auto* b) {
                return distanceSq(a->door, place.center) < distanceSq(b->door, place.center);
            });
            return result;
        };
        // Места работы по порядку важности: кого не хватит, тех мест не будет.
        std::vector<std::pair<Profession, EntityId>> slots;
        const auto staff = [&](BuildingKind kind, Profession profession, int perResidents, int most) {
            const auto buildings = ofKind(kind);
            if (!buildings.empty()) {
                const int workers = std::clamp(count / perResidents, 1, most);
                for (int i = 0; i < workers; ++i) {
                    slots.emplace_back(profession, buildings.front()->id);
                }
            }
        };
        const auto shops = ofKind(BuildingKind::Shop);
        const auto farms = ofKind(BuildingKind::Barn);
        const int staffedShops =
            std::min(static_cast<int>(shops.size()), std::max(1, count / kResidentsPerShop));
        const int farmers = std::min(static_cast<int>(farms.size()),
                                     std::max(1, count / kResidentsPerFarmer));
        staff(BuildingKind::Hospital, Profession::Doctor, kResidentsPerDoctor, kMaxDoctors);
        staff(BuildingKind::PoliceStation, Profession::PoliceOfficer, kResidentsPerOfficer, kMaxOfficers);
        if (staffedShops > 0) {
            slots.emplace_back(Profession::Shopkeeper, shops.front()->id);
        }
        for (int i = 0; i < farmers; ++i) {
            slots.emplace_back(Profession::Farmer, farms[static_cast<std::size_t>(i)]->id);
        }
        staff(BuildingKind::School, Profession::Teacher, kResidentsPerTeacher, kMaxTeachers);
        for (int i = 1; i < staffedShops; ++i) {
            slots.emplace_back(Profession::Shopkeeper, shops[static_cast<std::size_t>(i)]->id);
        }

        // Кто на какое место: лучше подходящий по характеру, с разбросом.
        std::vector<bool> taken(residents.size(), false);
        std::vector<std::pair<EntityId, std::size_t>> hired; // место работы, житель
        for (const auto& [profession, workplace] : slots) {
            std::size_t best = residents.size();
            float bestFit = 0.0f;
            for (std::size_t i = 0; i < residents.size(); ++i) {
                // Работают взрослые, ещё не на пенсии (этап 7.3).
                const int age = ageOf(registry, residents[i].second);
                if (taken[i] || age < kAdultAge || age >= kRetireAge) {
                    continue;
                }
                const float fit = fitness(profession, traitsOf(residents[i].second)) +
                                  rng.range(0.0f, kFitJitter);
                if (best == residents.size() || fit > bestFit) {
                    best = i;
                    bestFit = fit;
                }
            }
            if (best == residents.size()) {
                break; // жители кончились
            }
            taken[best] = true;
            hired.emplace_back(workplace, best);
            Job job;
            job.profession = profession;
            job.workplace = workplace;
            registry.emplace_or_replace<Job>(residents[best].second, job);
        }

        // Смены и выходные — по месту работы: совам — вечерняя смена, выходные
        // у товарищей по работе — в разные дни.
        std::stable_sort(hired.begin(), hired.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t begin = 0; begin < hired.size();) {
            std::size_t end = begin;
            while (end < hired.size() && hired[end].first == hired[begin].first) {
                ++end;
            }
            std::vector<std::size_t> team;
            for (std::size_t i = begin; i < end; ++i) {
                team.push_back(hired[i].second);
            }
            std::stable_sort(team.begin(), team.end(), [&](std::size_t a, std::size_t b) {
                return traitsOf(residents[a].second).chronotype <
                       traitsOf(residents[b].second).chronotype;
            });
            const EntityId workplace = hired[begin].first;
            for (std::size_t k = 0; k < team.size(); ++k) {
                const auto& [id, entity] = residents[team[k]];
                Job& job = registry.get<Job>(entity);
                const ProfessionSpec& spec = professionSpec(job.profession);
                const bool late = spec.lateStart != spec.lateEnd && k >= (team.size() + 1) / 2;
                job.start = static_cast<std::uint16_t>(late ? spec.lateStart : spec.start);
                job.end = static_cast<std::uint16_t>(late ? spec.lateEnd : spec.end);
                if (spec.weekdays) {
                    job.days = 0b0011111; // с понедельника по пятницу
                } else {
                    const int offset = static_cast<int>((workplace + 3 * k) % kDaysPerWeek);
                    job.days = 0;
                    for (int d = 0; d < spec.workdays; ++d) {
                        job.days = static_cast<std::uint8_t>(job.days | 1u << ((offset + d) % kDaysPerWeek));
                    }
                }
                const entt::entity building = world.find(workplace);
                if (!registry.all_of<Workplace>(building)) {
                    registry.emplace<Workplace>(building);
                }
                world.record(ChronicleKind::Employed, id, workplace,
                             static_cast<std::int32_t>(job.profession));
            }
            begin = end;
        }

        // Школьники (этап 7.3): дети школьного возраста ходят в школу своего
        // поселения — она есть только в городе.
        const auto schools = ofKind(BuildingKind::School);
        if (!schools.empty()) {
            const ProfessionSpec& spec = professionSpec(Profession::Pupil);
            for (const auto& [id, entity] : residents) {
                const int age = ageOf(registry, entity);
                if (age < kSchoolAge || age >= kAdultAge) {
                    continue;
                }
                Job job;
                job.profession = Profession::Pupil;
                job.workplace = schools.front()->id;
                job.start = static_cast<std::uint16_t>(spec.start);
                job.end = static_cast<std::uint16_t>(spec.end);
                job.days = 0b0011111;
                registry.emplace_or_replace<Job>(entity, job);
            }
        }
    }
    // Коллеги и одноклассники знакомы с начала мира.
    introduceColleagues(world);
}

} // namespace lw
