// Тесты ядра симуляции: RNG, стабильные id, карта и её генерация, детерминизм,
// драйвер времени, календарь, климат и погода, пространственный индекс, навигация,
// люди и движение, нужды, черты, Utility AI, ресурсы, летопись, восприятие, зомби,
// животные и птицы, бой, смерть и тела, инвентарь и лут, дефицит, отношения,
// память, долг и обида, чужие поступки и слухи, группы, социальные действия
// (поделиться, обмен, предательство, месть), ивенты, мирная жизнь, сценарий и
// фазы мира, сюжетные линии.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "sim/Calendar.h"
#include "sim/Cast.h"
#include "sim/Chronicle.h"
#include "sim/Climate.h"
#include "sim/Death.h"
#include "sim/Events.h"
#include "sim/Family.h"
#include "sim/Infection.h"
#include "sim/Foresight.h"
#include "sim/Components.h"
#include "sim/Items.h"
#include "sim/Map.h"
#include "sim/Memory.h"
#include "sim/Names.h"
#include "sim/Parallel.h"
#include "sim/Profiler.h"
#include "sim/Perception.h"
#include "sim/Places.h"
#include "sim/Relations.h"
#include "sim/Resources.h"
#include "sim/Rng.h"
#include "sim/Scenario.h"
#include "sim/Society.h"
#include "sim/SimDriver.h"
#include "sim/SpatialIndex.h"
#include "sim/Story.h"
#include "sim/Traits.h"
#include "sim/Weapons.h"
#include "sim/Work.h"
#include "sim/World.h"
#include "sim/nav/NavGraph.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/SocietySystem.h"
#include "sim/systems/CombatSystem.h"
#include "sim/systems/EventSystem.h"
#include "sim/systems/FamilySystem.h"
#include "sim/systems/GroupSystem.h"
#include "sim/systems/MemorySystem.h"
#include "sim/systems/MovementSystem.h"
#include "sim/systems/NeedsSystem.h"
#include "sim/systems/PerceptionSystem.h"
#include "sim/systems/ResourceSystem.h"
#include "sim/systems/InfectionSystem.h"
#include "sim/systems/ScenarioSystem.h"
#include "sim/systems/SocialSystem.h"
#include "sim/systems/UtilityAiSystem.h"
#include "sim/systems/WorkSystem.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

// Маленькая карта: тестам не нужен мир размером по умолчанию.
lw::WorldConfig smallConfig(int people) {
    lw::WorldConfig config;
    config.mapWidth = 64;
    config.mapHeight = 64;
    config.people = people;
    config.zombies = 0; // зомби, животные и птицы, ивенты — в своих тестах
    config.animals = 0;
    config.flocks = 0;
    config.events = false;
    config.peacetime = false; // мирная жизнь (этап 7.2) — в своих тестах
    return config;
}

void testRng() {
    lw::Rng a(123);
    lw::Rng b(123);
    lw::Rng c(124);
    bool same = true;
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const std::uint32_t value = a.nextU32();
        same = same && value == b.nextU32();
        differs = differs || value != c.nextU32();
    }
    CHECK(same);
    CHECK(differs);

    lw::Rng r(7);
    for (int i = 0; i < 10000; ++i) {
        CHECK(r.below(10) < 10u);
        const int n = r.rangeInt(-3, 3);
        CHECK(n >= -3 && n <= 3);
        const float u = r.unit();
        CHECK(u >= 0.0f && u < 1.0f);
    }

    // fork не зависит от того, сколько чисел уже вытянуто из родителя.
    lw::Rng fresh(99);
    lw::Rng used(99);
    for (int i = 0; i < 50; ++i) {
        used.nextU32();
    }
    CHECK(fresh.fork(5).nextU64() == used.fork(5).nextU64());
    CHECK(fresh.fork(5).nextU64() != fresh.fork(6).nextU64());
}

void testStableIds() {
    lw::World world(1, smallConfig(0));
    CHECK(world.entityCount() == 0);

    const entt::entity first = world.create();
    const entt::entity second = world.create();
    const lw::EntityId firstId = world.registry().get<lw::Id>(first).value;
    const lw::EntityId secondId = world.registry().get<lw::Id>(second).value;
    CHECK(firstId != lw::kInvalidId);
    CHECK(secondId > firstId);
    CHECK(world.find(firstId) == first);
    CHECK(world.find(secondId) == second);

    world.destroy(first);
    CHECK(world.find(firstId) == entt::null);
    CHECK(world.entityCount() == 1);

    // id удалённой сущности не выдаётся повторно, даже если entt переиспользует слот.
    const entt::entity third = world.create();
    const lw::EntityId thirdId = world.registry().get<lw::Id>(third).value;
    CHECK(thirdId > secondId);
    CHECK(world.find(firstId) == entt::null);
}

void testMap() {
    lw::Map map(12, 8);
    CHECK(map.width() == 12 && map.height() == 8);
    CHECK(map.worldWidth() == 12 * lw::Map::kCellSize);
    CHECK(map.worldHeight() == 8 * lw::Map::kCellSize);
    CHECK(map.elevation.cells().size() == 96);
    CHECK(map.road.cells().size() == 96);

    // Клетка включает свою левую и верхнюю границу, но не правую и нижнюю.
    const float cell = lw::Map::kCellSize;
    CHECK((lw::Map::cellAt(0.0f, 0.0f) == lw::Cell{0, 0}));
    CHECK((lw::Map::cellAt(cell - 0.01f, cell) == lw::Cell{0, 1}));
    CHECK((lw::Map::cellAt(-0.01f, 0.0f) == lw::Cell{-1, 0}));
    CHECK(lw::Map::cellCenter(0) == cell * 0.5f);
    CHECK((lw::Map::cellAt(lw::Map::cellCenter(11), lw::Map::cellCenter(7)) == lw::Cell{11, 7}));

    CHECK(map.contains({11, 7}));
    CHECK(!map.contains({12, 7}));
    CHECK(!map.contains({0, -1}));
    CHECK(!map.contains(lw::Map::cellAt(map.worldWidth(), 0.0f)));
    CHECK((map.clampedCellAt(1000.0f, -5.0f) == lw::Cell{11, 0}));

    map.elevation.at(3, 2) = -125;
    CHECK(map.elevationMeters({3, 2}) == -125 * lw::Map::kElevationStep);

    // Хеш зависит от размера и от каждого слоя.
    CHECK(lw::Map(12, 8).hash() == lw::Map(12, 8).hash());
    CHECK(lw::Map(12, 8).hash() != lw::Map(8, 12).hash());
    const std::uint64_t flat = lw::Map(12, 8).hash();
    CHECK(map.hash() != flat);
    map.elevation.at(3, 2) = 0;
    CHECK(map.hash() == flat);
    map.road.at(11, 7) = lw::Road::Asphalt;
    CHECK(map.hash() != flat);

    // Карта входит в хеш мира, размер мира берётся из неё.
    lw::WorldConfig config = smallConfig(0);
    lw::World square(1, config);
    config.mapHeight = 32;
    lw::World wide(1, config);
    CHECK(square.width() == 64 * cell && square.height() == 64 * cell);
    CHECK(wide.height() == 32 * cell);
    CHECK(square.stateHash() != wide.stateHash());
}

// Карта 4 × 4 км: на ней помещаются город, несколько деревень и дороги между ними.
lw::WorldConfig regionConfig() {
    lw::WorldConfig config;
    config.mapWidth = 1024;
    config.mapHeight = 1024;
    config.people = 0;
    config.zombies = 0;
    config.animals = 0;
    config.flocks = 0;
    config.events = false;
    config.peacetime = false; // мирная жизнь (этап 7.2) — в своих тестах
    return config;
}

// Один seed — одна карта и одни и те же поселения и здания.
void testMapGeneration() {
    const lw::World a(7, regionConfig());
    const lw::World b(7, regionConfig());
    const lw::World other(8, regionConfig());
    CHECK(a.map().hash() == b.map().hash());
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.map().hash() != other.map().hash());
    CHECK(a.stateHash() != other.stateHash());

    const lw::Map& map = a.map();
    const entt::registry& registry = a.registry();

    // Слои согласованы между собой.
    std::size_t land = 0;
    std::size_t sea = 0;
    std::size_t forest = 0;
    bool consistent = true;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const lw::Water water = map.water.at(x, y);
            const bool openWater = water == lw::Water::Sea || water == lw::Water::Lake;
            land += water == lw::Water::None ? 1 : 0;
            sea += water == lw::Water::Sea ? 1 : 0;
            forest += map.cover.at(x, y) == lw::Cover::Forest ? 1 : 0;

            consistent = consistent && (water == lw::Water::None) == (map.depth.at(x, y) == 0);
            consistent = consistent && (water != lw::Water::Sea || map.elevation.at(x, y) < 0);
            // Лес растёт только на суше и не на дороге; дорога через реку —
            // мост, через море и озёра дорог нет.
            if (map.cover.at(x, y) == lw::Cover::Forest) {
                consistent = consistent && water == lw::Water::None &&
                             map.road.at(x, y) == lw::Road::None;
            }
            consistent = consistent && !(openWater && map.road.at(x, y) != lw::Road::None);
        }
    }
    CHECK(consistent);
    CHECK(land > 0 && sea > 0 && forest > 0);

    // Поселения: есть город и деревни, центр каждого лежит на дороге.
    std::vector<lw::Cell> centers;
    int towns = 0;
    for (const auto [entity, settlement] : registry.view<const lw::Settlement>().each()) {
        towns += settlement.kind == lw::SettlementKind::Town ? 1 : 0;
        CHECK(!settlement.name.empty());
        CHECK(map.contains(settlement.center));
        CHECK(map.road.at(settlement.center.x, settlement.center.y) != lw::Road::None);
        centers.push_back(settlement.center);
    }
    CHECK(towns >= 1);
    CHECK(centers.size() >= 3);

    // Здания стоят на суше, не на дороге и не вплотную друг к другу.
    lw::Grid<std::uint8_t> occupied(map.width(), map.height());
    std::size_t buildings = 0;
    for (const auto [entity, building] : registry.view<const lw::Building>().each()) {
        ++buildings;
        CHECK(building.width > 0 && building.height > 0);
        CHECK(map.contains(building.origin));
        CHECK(map.contains({building.origin.x + building.width - 1,
                            building.origin.y + building.height - 1}));
        for (int y = building.origin.y; y < building.origin.y + building.height; ++y) {
            for (int x = building.origin.x; x < building.origin.x + building.width; ++x) {
                CHECK(map.water.at(x, y) == lw::Water::None);
                CHECK(map.road.at(x, y) == lw::Road::None);
                CHECK(map.cover.at(x, y) == lw::Cover::None);
                CHECK(occupied.at(x, y) == 0);
                occupied.at(x, y) = 1;
            }
        }
        const entt::entity owner = a.find(building.settlement);
        CHECK(owner != entt::null && registry.all_of<lw::Settlement>(owner));
    }
    CHECK(buildings > 50);
    for (const auto [entity, building] : registry.view<const lw::Building>().each()) {
        // Вокруг здания — свободная полоса в клетку, дверь выходит на неё.
        for (int y = building.origin.y - 1; y <= building.origin.y + building.height; ++y) {
            for (int x = building.origin.x - 1; x <= building.origin.x + building.width; ++x) {
                const bool inside = x >= building.origin.x && y >= building.origin.y &&
                                    x < building.origin.x + building.width &&
                                    y < building.origin.y + building.height;
                CHECK(inside || occupied.at(x, y) == 0);
            }
        }
        const lw::Cell door = building.door;
        CHECK(map.contains(door) && occupied.at(door.x, door.y) == 0);
        CHECK(map.water.at(door.x, door.y) == lw::Water::None);
        CHECK(door.x >= building.origin.x - 1 && door.x <= building.origin.x + building.width &&
              door.y >= building.origin.y - 1 && door.y <= building.origin.y + building.height);
    }

    // По дорогам можно добраться из любого поселения в любое.
    lw::Grid<std::uint8_t> reached(map.width(), map.height());
    std::vector<lw::Cell> queue = {centers.front()};
    reached.at(centers.front().x, centers.front().y) = 1;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const lw::Cell cell = queue[i];
        const lw::Cell neighbors[4] = {{cell.x + 1, cell.y}, {cell.x - 1, cell.y},
                                       {cell.x, cell.y + 1}, {cell.x, cell.y - 1}};
        for (const lw::Cell next : neighbors) {
            if (map.contains(next) && !reached.at(next.x, next.y) &&
                map.road.at(next.x, next.y) != lw::Road::None) {
                reached.at(next.x, next.y) = 1;
                queue.push_back(next);
            }
        }
    }
    for (const lw::Cell center : centers) {
        CHECK(reached.at(center.x, center.y) == 1);
    }
}

void testDeterminism() {
    lw::World a(42, smallConfig(300));
    lw::World b(42, smallConfig(300));
    lw::World other(43, smallConfig(300));
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.stateHash() != other.stateHash());

    const std::uint64_t initial = a.stateHash();
    for (int i = 0; i < 2000; ++i) {
        a.step();
        b.step();
        other.step();
        if (i % 500 == 0) {
            CHECK(a.stateHash() == b.stateHash());
        }
    }
    CHECK(a.tick() == 2000);
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.stateHash() != other.stateHash());
    CHECK(a.stateHash() != initial);
}

void testEntitiesStayInWorld() {
    lw::WorldConfig config;
    config.mapWidth = 12;
    config.mapHeight = 8;
    config.people = 100;
    config.zombies = 0;
    config.animals = 0;
    config.flocks = 0;
    config.events = false;
    config.peacetime = false; // мирная жизнь (этап 7.2) — в своих тестах
    // Люди и, отдельно, зомби: рядом с людьми зомби не селятся.
    lw::WorldConfig undead = config;
    undead.people = 0;
    undead.zombies = 30;
    for (const lw::WorldConfig& variant : {config, undead}) {
        lw::World world(5, variant);
        for (int i = 0; i < 5000; ++i) {
            world.step();
        }
        const lw::World& readOnly = world;
        CHECK(readOnly.registry().view<const lw::Position>().size() ==
              static_cast<std::size_t>(variant.people + variant.zombies));
        for (const auto [entity, position] : readOnly.registry().view<const lw::Position>().each()) {
            CHECK(position.x >= 0.0f && position.x <= world.width());
            CHECK(position.y >= 0.0f && position.y <= world.height());
        }
    }
}

void testSimDriver() {
    lw::World world(1, smallConfig(300));
    lw::SimDriver driver(world);
    driver.setFrameBudgetMs(10000.0);

    // x1: одна секунда реального времени — около 20 тиков.
    for (int i = 0; i < 10; ++i) {
        driver.advance(0.1);
    }
    const std::uint64_t afterX1 = world.tick();
    CHECK(afterX1 >= 19 && afterX1 <= 21);

    driver.setSpeed(10);
    for (int i = 0; i < 10; ++i) {
        driver.advance(0.1);
    }
    const std::uint64_t duringX10 = world.tick() - afterX1;
    CHECK(duringX10 >= 198 && duringX10 <= 202);

    driver.setPaused(true);
    const std::uint64_t paused = world.tick();
    driver.advance(1.0);
    CHECK(world.tick() == paused);
    driver.requestStep();
    driver.advance(1.0);
    CHECK(world.tick() == paused + 1);
    driver.advance(1.0);
    CHECK(world.tick() == paused + 1);

    // На паузе отрисовка показывает ровно последний тик.
    CHECK(driver.interpolationAlpha() == 1.0f);

    // Доля тика для интерполяции: 1,5 тика реального времени — один тик и половина.
    driver.setPaused(false);
    driver.setSpeed(1);
    driver.advance(1.5 / lw::kTicksPerSecond);
    CHECK(world.tick() == paused + 2);
    CHECK(std::abs(driver.interpolationAlpha() - 0.5f) < 0.01f);
    driver.advance(0.25 / lw::kTicksPerSecond);
    CHECK(world.tick() == paused + 2);
    CHECK(std::abs(driver.interpolationAlpha() - 0.75f) < 0.01f);

    // Исчерпанный бюджет кадра не подвешивает цикл, а отбрасывает отставание.
    driver.setPaused(false);
    driver.setSpeed(100);
    driver.setFrameBudgetMs(0.0);
    driver.advance(0.25);
    CHECK(driver.behind());
    CHECK(driver.ticksLastFrame() < 500);
}

void testCalendar() {
    using lw::kTicksPerGameDay;
    using lw::kTicksPerGameHour;
    using lw::kTicksPerGameMinute;

    // Мир начинается в 08:00 первого дня весны первого года.
    const lw::CalendarTime start = lw::calendarAt(0);
    CHECK(start.year == 1 && start.day == 1 && start.dayOfYear == 0 && start.dayOfSeason == 0);
    CHECK(start.season == lw::Season::Spring);
    CHECK(start.hour == 8 && start.minute == 0 && start.minuteOfDay == 480);

    const lw::CalendarTime later = lw::calendarAt(16 * kTicksPerGameHour + 5 * kTicksPerGameMinute + 99);
    CHECK(later.day == 2 && later.hour == 0 && later.minute == 5);

    const lw::CalendarTime summer = lw::calendarAt(15 * kTicksPerGameDay);
    CHECK(summer.season == lw::Season::Summer && summer.dayOfSeason == 0 && summer.day == 16);
    const lw::CalendarTime winterEnd = lw::calendarAt(60 * kTicksPerGameDay - lw::kStartGameTick - 1);
    CHECK(winterEnd.season == lw::Season::Winter && winterEnd.dayOfSeason == 14);
    CHECK(winterEnd.hour == 23 && winterEnd.minute == 59 && winterEnd.year == 1);
    const lw::CalendarTime nextYear = lw::calendarAt(60 * kTicksPerGameDay - lw::kStartGameTick);
    CHECK(nextYear.year == 2 && nextYear.dayOfYear == 0 && nextYear.season == lw::Season::Spring);
    CHECK(nextYear.yearFraction == 0.0);
    CHECK(winterEnd.yearFraction > 0.99 && winterEnd.yearFraction < 1.0);

    // Ближайшие 06:00 — завтра, ближайшие 20:30 — сегодня, ровно 08:00 — через сутки.
    CHECK(lw::nextTimeOfDay(0, 6, 0) == 22 * kTicksPerGameHour);
    CHECK(lw::nextTimeOfDay(0, 20, 30) == 12 * kTicksPerGameHour + 30 * kTicksPerGameMinute);
    CHECK(lw::nextTimeOfDay(0, 8, 0) == kTicksPerGameDay);
}

lw::CalendarTime timeAt(int dayOfYear, int hour) {
    const std::uint64_t gameTick = static_cast<std::uint64_t>(dayOfYear) * lw::kTicksPerGameDay +
                                   static_cast<std::uint64_t>(hour) * lw::kTicksPerGameHour;
    return lw::calendarAt(gameTick - lw::kStartGameTick);
}

void testClimate() {
    // Приближение косинуса.
    float worst = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const double t = i / 1000.0;
        const auto exact = static_cast<float>(std::cos(6.283185307179586 * t));
        worst = std::max(worst, std::abs(lw::smoothCycle(t) - exact));
    }
    CHECK(worst < 0.03f);
    CHECK(lw::smoothCycle(0.0) == 1.0f);
    CHECK(lw::smoothCycle(0.5) == -1.0f);
    CHECK(lw::smoothCycle(-1.0) == 1.0f);

    // Солнце: летом день длиннее и солнце выше, в полночь — ночь.
    const lw::SunState summerNoon = lw::sunAt(timeAt(22, 12));
    const lw::SunState winterNoon = lw::sunAt(timeAt(52, 12));
    const lw::SunState midnight = lw::sunAt(timeAt(22, 0));
    const int summerDay = summerNoon.sunsetMinute - summerNoon.sunriseMinute;
    const int winterDay = winterNoon.sunsetMinute - winterNoon.sunriseMinute;
    CHECK(summerDay > 15 * 60 && summerDay < 17 * 60);
    CHECK(winterDay > 7 * 60 && winterDay < 9 * 60);
    CHECK(summerNoon.elevation > 55.0f);
    CHECK(winterNoon.elevation > 10.0f && winterNoon.elevation < 25.0f);
    CHECK(midnight.elevation < -12.0f);

    // Свет: ясный летний полдень — почти 1, ночью темно, облака приглушают.
    const lw::Weather clear;
    lw::Weather overcast;
    overcast.cloudCover = 1.0f;
    CHECK(lw::lightLevel(summerNoon, clear) > 0.95f);
    CHECK(lw::lightLevel(midnight, clear) < 0.15f);
    CHECK(lw::lightLevel(summerNoon, overcast) < lw::lightLevel(summerNoon, clear));
    CHECK(lw::lightLevel(winterNoon, clear) < lw::lightLevel(summerNoon, clear));
    // С утренних сумерек до полудня свет только растёт (ночью его задаёт луна,
    // а её фаза за ночь немного меняется).
    float previous = -1.0f;
    bool monotonic = true;
    for (int minute = 0; minute <= 750; minute += 5) {
        lw::CalendarTime time = timeAt(22, 0);
        time.minuteOfDay = minute;
        const lw::SunState sun = lw::sunAt(time);
        if (sun.elevation < -6.0f) {
            continue;
        }
        const float light = lw::lightLevel(sun, clear);
        monotonic = monotonic && light >= previous - 1e-6f;
        previous = light;
    }
    CHECK(monotonic);

    // Температура: лето теплее зимы, день теплее ночи, в горах холоднее.
    const float summerDayTemp = lw::seaLevelTemperature(timeAt(25, 15), clear);
    const float summerNightTemp = lw::seaLevelTemperature(timeAt(25, 3), clear);
    const float winterDayTemp = lw::seaLevelTemperature(timeAt(55, 15), clear);
    CHECK(summerDayTemp > 18.0f && summerDayTemp < 30.0f);
    CHECK(summerNightTemp < summerDayTemp);
    CHECK(winterDayTemp < 3.0f && winterDayTemp > -15.0f);
    CHECK(lw::temperatureAtElevation(10.0f, 1000.0f) == 10.0f - 6.5f);
    CHECK(lw::temperatureAtElevation(10.0f, -5.0f) == 10.0f);
    CHECK(lw::isSnowing(-3.0f) && !lw::isSnowing(5.0f));
}

// Погода детерминирована, меняется, остаётся в допустимых пределах и следует сезону.
void testWeather() {
    lw::World a(11, smallConfig(0));
    lw::World b(11, smallConfig(0));
    const lw::Weather& weather = a.weather();
    CHECK(weather.minutesLeft > 0);

    constexpr int kKinds = static_cast<int>(lw::WeatherKind::Count);
    bool seen[kKinds] = {};
    int changes = 0;
    lw::WeatherKind last = weather.kind;
    bool rainFromClear = false;
    bool inRange = true;
    float summerSnow = 0.0f; // больше нуля — снег летом не растаял
    float winterSnow = 0.0f;

    // Год, проверка раз в игровой час.
    for (int hour = 0; hour < lw::kGameDaysPerYear * 24; ++hour) {
        for (std::uint64_t i = 0; i < lw::kTicksPerGameHour; ++i) {
            a.step();
            b.step();
        }
        if (weather.kind != last) {
            ++changes;
            rainFromClear = rainFromClear ||
                            (last == lw::WeatherKind::Clear && weather.kind == lw::WeatherKind::Rain);
            last = weather.kind;
        }
        seen[static_cast<int>(weather.kind)] = true;
        inRange = inRange && weather.cloudCover >= 0.0f && weather.cloudCover <= 1.0f &&
                  weather.precipitation >= 0.0f && weather.precipitation <= 1.0f &&
                  weather.fog >= 0.0f && weather.fog <= 1.0f && weather.snowCover >= 0.0f &&
                  weather.snowCover <= 1.0f && weather.windSpeed >= 0.0f &&
                  weather.windFrom >= 0.0f && weather.windFrom < 360.0f;

        const lw::CalendarTime time = lw::calendarAt(a.tick());
        if (time.season == lw::Season::Summer && time.dayOfSeason >= 5) {
            summerSnow = std::max(summerSnow, weather.snowCover);
        }
        if (time.season == lw::Season::Winter) {
            winterSnow = std::max(winterSnow, weather.snowCover);
        }
    }
    int kindsSeen = 0;
    for (const bool s : seen) {
        kindsSeen += s ? 1 : 0;
    }
    CHECK(a.stateHash() == b.stateHash());
    CHECK(changes > 20);
    CHECK(kindsSeen >= 5);
    CHECK(!rainFromClear);
    CHECK(inRange);
    CHECK(summerSnow == 0.0f);
    CHECK(winterSnow > 0.5f);

    // Другой seed — другая погода, даже на той же карте.
    lw::World same(11, smallConfig(0));
    lw::World other(12, smallConfig(0));
    CHECK(same.weather().anomaly != other.weather().anomaly);
}


// --- Пространственный индекс -------------------------------------------------

entt::entity spawnAt(entt::registry& registry, lw::EntityId id, float x, float y) {
    const entt::entity entity = registry.create();
    registry.emplace<lw::Id>(entity, id);
    registry.emplace<lw::Position>(entity, x, y);
    return entity;
}

// Эталон — полный перебор. Результат — id по возрастанию.
std::vector<lw::EntityId> bruteRadius(const entt::registry& registry, float x, float y,
                                      float radius) {
    std::vector<lw::EntityId> ids;
    for (const auto [entity, id, position] :
         registry.view<const lw::Id, const lw::Position>().each()) {
        const float dx = position.x - x;
        const float dy = position.y - y;
        if (dx * dx + dy * dy <= radius * radius) {
            ids.push_back(id.value);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Ближайший перебором; при равных расстояниях — меньший id.
// oddOnly — только сущности с нечётным id.
lw::EntityId bruteNearest(const entt::registry& registry, float x, float y, float radius,
                          bool oddOnly) {
    std::optional<std::pair<float, lw::EntityId>> best;
    for (const auto [entity, id, position] :
         registry.view<const lw::Id, const lw::Position>().each()) {
        const float dx = position.x - x;
        const float dy = position.y - y;
        const float distanceSq = dx * dx + dy * dy;
        if (distanceSq > radius * radius || (oddOnly && id.value % 2 == 0)) {
            continue;
        }
        if (!best || distanceSq < best->first ||
            (distanceSq == best->first && id.value < best->second)) {
            best = std::pair{distanceSq, id.value};
        }
    }
    return best ? best->second : lw::kInvalidId;
}

std::vector<lw::EntityId> hitIds(const std::vector<lw::SpatialHit>& hits) {
    std::vector<lw::EntityId> ids;
    for (const lw::SpatialHit& hit : hits) {
        ids.push_back(hit.id);
    }
    return ids;
}

// Сверяет запросы индекса с перебором в случайных точках, в том числе за краем мира.
void checkAgainstBruteForce(const entt::registry& registry, const lw::SpatialIndex& index,
                            lw::Rng& rng, float width, float height) {
    CHECK(index.size() == registry.view<const lw::Position>().size());
    std::vector<lw::SpatialHit> hits;
    for (int q = 0; q < 400; ++q) {
        float x = rng.range(-60.0f, width + 60.0f);
        float y = rng.range(-60.0f, height + 60.0f);
        if (q % 4 == 0) {
            // Узел решётки или середина между узлами: много равных расстояний.
            x = static_cast<float>(rng.rangeInt(0, 20)) * 10.0f;
            y = static_cast<float>(rng.rangeInt(0, 10)) * 10.0f;
        }
        const float radius = q % 3 == 0 ? rng.range(0.0f, 8.0f) : rng.range(0.0f, 150.0f);

        index.queryRadius(x, y, radius, hits);
        std::vector<lw::EntityId> ids = hitIds(hits);
        std::sort(ids.begin(), ids.end());
        CHECK(ids == bruteRadius(registry, x, y, radius));
        for (const lw::SpatialHit& hit : hits) {
            const lw::Position& position = registry.get<lw::Position>(hit.entity);
            CHECK(registry.get<lw::Id>(hit.entity).value == hit.id);
            CHECK(position.x == hit.x && position.y == hit.y);
        }

        const auto nearest = index.nearest(x, y, radius);
        CHECK((nearest ? nearest->id : lw::kInvalidId) ==
              bruteNearest(registry, x, y, radius, false));
        const auto nearestOdd = index.nearest(
            x, y, radius, [](const lw::SpatialHit& hit) { return hit.id % 2 == 1; });
        CHECK((nearestOdd ? nearestOdd->id : lw::kInvalidId) ==
              bruteNearest(registry, x, y, radius, true));
    }
}

void testSpatialIndex() {
    // 200 × 100 м — 7 × 4 корзины, крайние неполные.
    constexpr float kWidth = 200.0f;
    constexpr float kHeight = 100.0f;
    entt::registry registry;
    lw::SpatialIndex index(kWidth, kHeight);
    registry.on_destroy<lw::Position>().connect<&lw::SpatialIndex::onPositionDestroyed>(index);
    CHECK(index.columns() == 7 && index.rows() == 4);

    lw::Rng rng(11);
    lw::EntityId nextId = 1;
    std::vector<entt::entity> entities;
    for (int i = 0; i < 300; ++i) {
        // Часть — за краем мира.
        entities.push_back(spawnAt(registry, nextId++, rng.range(-30.0f, kWidth + 30.0f),
                                   rng.range(-30.0f, kHeight + 30.0f)));
    }
    entities.push_back(spawnAt(registry, nextId++, kWidth, kHeight));
    entities.push_back(spawnAt(registry, nextId++, 0.0f, 0.0f));
    for (int gx = 0; gx <= 20; gx += 2) {
        for (int gy = 0; gy <= 10; gy += 2) {
            entities.push_back(spawnAt(registry, nextId++, static_cast<float>(gx) * 10.0f,
                                       static_cast<float>(gy) * 10.0f));
        }
    }
    index.sync(registry);
    checkAgainstBruteForce(registry, index, rng, kWidth, kHeight);

    // Снимок: без sync индекс помнит старую позицию.
    lw::Position& moved = registry.get<lw::Position>(entities[0]);
    const float oldX = moved.x;
    const float oldY = moved.y;
    moved.x = oldX + 77.0f;
    std::vector<lw::SpatialHit> hits;
    index.queryRadius(oldX, oldY, 0.0f, hits);
    CHECK(std::any_of(hits.begin(), hits.end(),
                      [&](const lw::SpatialHit& hit) { return hit.entity == entities[0]; }));

    // Движение (в том числе через границы корзин), удаление, снятие Position,
    // новые сущности на освободившихся номерах реестра.
    for (int round = 0; round < 5; ++round) {
        for (const entt::entity entity : entities) {
            if (registry.valid(entity) && registry.all_of<lw::Position>(entity) &&
                rng.below(3) != 0) {
                lw::Position& position = registry.get<lw::Position>(entity);
                position.x += rng.range(-40.0f, 40.0f);
                position.y += rng.range(-40.0f, 40.0f);
            }
        }
        for (int i = 0; i < 15; ++i) {
            const entt::entity entity =
                entities[rng.below(static_cast<std::uint32_t>(entities.size()))];
            if (!registry.valid(entity)) {
                continue;
            }
            const std::size_t before = index.size();
            const bool hadPosition = registry.all_of<lw::Position>(entity);
            if (i % 3 == 0) {
                registry.remove<lw::Position>(entity);
            } else {
                registry.destroy(entity);
            }
            // Удалённые исчезают сразу, без sync.
            CHECK(index.size() == before - (hadPosition ? 1u : 0u));
        }
        for (int i = 0; i < 10; ++i) {
            entities.push_back(spawnAt(registry, nextId++, rng.range(0.0f, kWidth),
                                       rng.range(0.0f, kHeight)));
        }
        index.sync(registry);
        checkAgainstBruteForce(registry, index, rng, kWidth, kHeight);
    }

    // Отрицательный радиус и NaN ничего не находят.
    index.queryRadius(50.0f, 50.0f, -1.0f, hits);
    CHECK(hits.empty());
    CHECK(!index.nearest(50.0f, 50.0f, -1.0f));
    index.queryRadius(std::nanf(""), 50.0f, 10.0f, hits);
    CHECK(hits.empty());
}

// Содержимое индекса и порядок выдачи не зависят от порядка вставки, истории
// перемещений и номеров сущностей в реестре — только от id и позиций.
void testSpatialIndexOrderIndependent() {
    constexpr float kSize = 300.0f;
    constexpr std::size_t kCount = 500;
    std::vector<lw::Position> positions;
    lw::Rng rng(5);
    for (std::size_t i = 0; i < kCount; ++i) {
        positions.push_back({rng.range(0.0f, kSize), rng.range(0.0f, kSize)});
    }

    entt::registry forward;
    entt::registry backward;
    lw::SpatialIndex forwardIndex(kSize, kSize);
    lw::SpatialIndex backwardIndex(kSize, kSize);
    std::vector<entt::entity> forwardEntities;
    for (std::size_t i = 0; i < kCount; ++i) {
        forwardEntities.push_back(spawnAt(forward, i + 1, positions[i].x, positions[i].y));
    }
    for (std::size_t i = kCount; i-- > 0;) {
        spawnAt(backward, i + 1, positions[i].x, positions[i].y);
    }
    // Разная история: первый индекс видел промежуточные позиции, второй — нет.
    forwardIndex.sync(forward);
    for (std::size_t i = 0; i < kCount; ++i) {
        forward.get<lw::Position>(forwardEntities[i]).x = static_cast<float>(i % 300);
    }
    forwardIndex.sync(forward);
    for (std::size_t i = 0; i < kCount; ++i) {
        forward.get<lw::Position>(forwardEntities[i]) = positions[i];
    }
    forwardIndex.sync(forward);
    backwardIndex.sync(backward);

    std::vector<lw::SpatialHit> a;
    std::vector<lw::SpatialHit> b;
    bool same = true;
    for (int q = 0; q < 100; ++q) {
        const float x = rng.range(0.0f, kSize);
        const float y = rng.range(0.0f, kSize);
        const float radius = rng.range(0.0f, 120.0f);
        forwardIndex.queryRadius(x, y, radius, a);
        backwardIndex.queryRadius(x, y, radius, b);
        same = same && !a.empty() == !b.empty() && hitIds(a) == hitIds(b);
    }
    CHECK(same);
}

// Мир синхронизирует индекс в конце тика и убирает удалённые сущности сразу.
void testWorldSpatialIndex() {
    lw::World world(3, smallConfig(200));
    for (int i = 0; i < 300; ++i) {
        world.step();
    }
    const lw::World& readOnly = world;
    CHECK(world.spatial().size() == 200);
    std::vector<lw::SpatialHit> hits;
    bool allFound = true;
    for (const auto [entity, position] : readOnly.registry().view<const lw::Position>().each()) {
        world.spatial().queryRadius(position.x, position.y, 0.0f, hits);
        allFound = allFound &&
                   std::any_of(hits.begin(), hits.end(),
                               [&](const lw::SpatialHit& hit) { return hit.entity == entity; });
    }
    CHECK(allFound);

    const entt::entity victim = *readOnly.registry().view<const lw::Position>().begin();
    world.destroy(victim);
    CHECK(world.spatial().size() == 199);
}

// Индекс с тысячами сущностей (этап 2а.1): синхронизация идёт параллельно, а
// результат — тот же, что в одном потоке и у полного перебора; порядок обхода —
// ряды, столбцы, id.
void testSpatialIndexParallelSync() {
    constexpr float kSize = 2000.0f;
    constexpr std::size_t kCount = 3000; // больше порога параллельной синхронизации
    entt::registry registry;
    lw::Rng rng(11);
    std::vector<entt::entity> entities;
    for (std::size_t i = 0; i < kCount; ++i) {
        entities.push_back(spawnAt(registry, i + 1, rng.range(0.0f, kSize), rng.range(0.0f, kSize)));
    }
    const int threads = lw::parallelThreads();
    lw::SpatialIndex serial(kSize, kSize);
    lw::SpatialIndex parallel(kSize, kSize);
    bool same = true;
    bool matchesBrute = true;
    std::vector<lw::SpatialHit> a;
    std::vector<lw::SpatialHit> b;
    for (int round = 0; round < 6; ++round) {
        // Часть сущностей переходит в другие корзины, часть сдвигается внутри своей.
        for (std::size_t i = 0; i < kCount; ++i) {
            lw::Position& position = registry.get<lw::Position>(entities[i]);
            const float step = (i + static_cast<std::size_t>(round)) % 3 == 0 ? 60.0f : 1.0f;
            position.x = std::clamp(position.x + rng.range(-step, step), 0.0f, kSize);
            position.y = std::clamp(position.y + rng.range(-step, step), 0.0f, kSize);
        }
        lw::setParallelThreads(1);
        serial.sync(registry);
        lw::setParallelThreads(4);
        parallel.sync(registry);
        for (int q = 0; q < 40; ++q) {
            const float x = rng.range(0.0f, kSize);
            const float y = rng.range(0.0f, kSize);
            const float radius = rng.range(0.0f, 200.0f);
            serial.queryRadius(x, y, radius, a);
            parallel.queryRadius(x, y, radius, b);
            bool sameOrder = a.size() == b.size();
            for (std::size_t i = 0; sameOrder && i < a.size(); ++i) {
                sameOrder = a[i].id == b[i].id && a[i].x == b[i].x && a[i].y == b[i].y;
            }
            same = same && sameOrder;
            std::vector<lw::EntityId> found = hitIds(b);
            std::sort(found.begin(), found.end());
            matchesBrute = matchesBrute && found == bruteRadius(registry, x, y, radius);
            const auto nearA = serial.nearest(x, y, 300.0f);
            const auto nearB = parallel.nearest(x, y, 300.0f);
            same = same && nearA.has_value() == nearB.has_value() && (!nearA || nearA->id == nearB->id);
        }
    }
    lw::setParallelThreads(threads);
    CHECK(same);
    CHECK(matchesBrute);
    CHECK(parallel.size() == kCount);
}

// Параллельные проходы (восприятие, движение, синхронизация индекса — этап 2а.1)
// не меняют мир: хеш один и тот же при любом числе потоков. Мир — с тысячами
// существ, чтобы все проходы и правда шли параллельно.
void testParallelThreadsSameHash() {
    lw::WorldConfig config;
    config.mapWidth = 512;
    config.mapHeight = 512;
    config.people = 300;
    config.zombies = 1900;
    config.animals = 60;
    config.flocks = 4;
    config.peacetime = false;
    const int threads = lw::parallelThreads();
    std::vector<std::uint64_t> hashes;
    for (const int count : {1, 3, 8}) {
        lw::setParallelThreads(count);
        lw::World world(7, config);
        for (int i = 0; i < 1200; ++i) {
            world.step();
        }
        hashes.push_back(world.stateHash());
    }
    lw::setParallelThreads(threads);
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
}

// Профайлер (этап 2а.1) только меряет: с ним и без него мир один и тот же, а
// шаги старта и системы тика записаны.
void testProfilerDoesNotChangeWorld() {
    lw::World plain(5, smallConfig(80));
    lw::World measured(5, smallConfig(80));
    measured.setProfiling(true);
    for (int i = 0; i < 400; ++i) {
        plain.step();
        measured.step();
    }
    CHECK(plain.stateHash() == measured.stateHash());
    CHECK(measured.profiler().ticks() == 400);
    CHECK(plain.profiler().ticks() == 0);
    CHECK(measured.profiler().totals(lw::profileSection("perception")).calls == 400);
    bool hasMap = false;
    for (const lw::Profiler::StartupStep& step : plain.profiler().startup()) {
        hasMap = hasMap || step.name == "world/map/elevation";
    }
    CHECK(hasMap);
    std::size_t memory = 0;
    for (const lw::MemoryItem& item : plain.memoryReport()) {
        memory += item.bytes;
    }
    CHECK(memory > 0);
}

// --- Навигация ---------------------------------------------------------------

void testSegmentWalk() {
    const auto walk = [](lw::Cell a, lw::Cell b) {
        std::vector<lw::Cell> cells;
        lw::forEachCellOnSegment(a, b, [&](int x, int y) {
            cells.push_back({x, y});
            return true;
        });
        return cells;
    };
    CHECK((walk({2, 3}, {2, 3}) == std::vector<lw::Cell>{{2, 3}}));
    CHECK((walk({0, 0}, {3, 0}) == std::vector<lw::Cell>{{0, 0}, {1, 0}, {2, 0}, {3, 0}}));
    // Через угол клеток задеты обе клетки по бокам.
    CHECK((walk({0, 0}, {1, 1}) == std::vector<lw::Cell>{{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    CHECK((walk({0, 0}, {2, 1}) == std::vector<lw::Cell>{{0, 0}, {1, 0}, {1, 1}, {2, 1}}));
    CHECK((walk({0, 0}, {3, 1}) ==
           std::vector<lw::Cell>{{0, 0}, {1, 0}, {2, 0}, {1, 1}, {2, 1}, {3, 1}}));
    // В обратную сторону — те же клетки.
    std::vector<lw::Cell> back = walk({3, 1}, {0, 0});
    std::vector<lw::Cell> forth = walk({0, 0}, {3, 1});
    const auto byRow = [](lw::Cell l, lw::Cell r) { return l.y != r.y ? l.y < r.y : l.x < r.x; };
    std::sort(back.begin(), back.end(), byRow);
    std::sort(forth.begin(), forth.end(), byRow);
    CHECK(back == forth);
    // Обход прерывается, как только visit вернул false.
    int visited = 0;
    CHECK(!lw::forEachCellOnSegment({0, 0}, {9, 0}, [&](int x, int) {
        ++visited;
        return x < 4;
    }));
    CHECK(visited == 5);
}

void testNavGrid() {
    lw::Map map(16, 12);
    // Столбец моря x = 8 делит карту на две половины.
    for (int y = 0; y < 12; ++y) {
        map.water.at(8, y) = lw::Water::Sea;
        map.depth.at(8, y) = 30;
    }
    map.road.at(1, 1) = lw::Road::Dirt;
    map.ground.at(2, 1) = lw::Ground::Field;
    map.ground.at(3, 1) = lw::Ground::Sand;
    map.cover.at(4, 1) = lw::Cover::Forest;
    map.ground.at(5, 1) = lw::Ground::Rock;
    map.ground.at(6, 1) = lw::Ground::Rock;
    map.cover.at(6, 1) = lw::Cover::Forest;
    const int depths[] = {4, 12, 25};
    for (int i = 0; i < 3; ++i) {
        map.water.at(1 + i, 3) = lw::Water::River;
        map.depth.at(1 + i, 3) = static_cast<std::uint8_t>(depths[i]);
    }
    map.water.at(4, 3) = lw::Water::River; // мост
    map.depth.at(4, 3) = 25;
    map.road.at(4, 3) = lw::Road::Asphalt;
    map.water.at(5, 3) = lw::Water::Lake;
    map.depth.at(5, 3) = 20;

    entt::registry registry;
    const entt::entity house = registry.create();
    registry.emplace<lw::Building>(house, lw::BuildingKind::House, lw::Cell{1, 6}, 2, 3, lw::Cell{3, 7},
                                   lw::kInvalidId);
    lw::NavGrid nav;
    nav.build(map, registry);

    CHECK(nav.width() == 16 && nav.height() == 12);
    CHECK(nav.cost({0, 0}) == 11);
    CHECK(nav.cost({1, 1}) == lw::NavGrid::kRoadCost);
    CHECK(nav.cost({2, 1}) == 13 && nav.cost({3, 1}) == 13);
    CHECK(nav.cost({4, 1}) == 15);
    CHECK(nav.cost({5, 1}) == 22 && nav.cost({6, 1}) == 22);
    CHECK(nav.cost({1, 3}) == 30 && nav.cost({2, 3}) == 60 && nav.cost({3, 3}) == 250);
    CHECK(nav.cost({4, 3}) == lw::NavGrid::kRoadCost);
    CHECK(!nav.passable({5, 3}) && !nav.passable({8, 5}));
    CHECK(!nav.passable({1, 6}) && !nav.passable({2, 8}) && nav.passable({3, 7}));
    CHECK(!nav.passable({-1, 0}) && !nav.passable({16, 0}));

    CHECK(lw::NavGrid::classOf(0) == lw::NavClass::Blocked);
    CHECK(lw::NavGrid::classOf(nav.cost({1, 1})) == lw::NavClass::Road);
    CHECK(lw::NavGrid::classOf(nav.cost({5, 1})) == lw::NavClass::Land);
    CHECK(lw::NavGrid::classOf(nav.cost({1, 3})) == lw::NavClass::Water);

    // Шаги: по стороне и по диагонали, мимо угла непроходимой клетки — нельзя.
    CHECK(nav.stepCost({0, 0}, {1, 0}) == lw::NavGrid::kStraightFactor * 22);
    CHECK(nav.stepCost({0, 0}, {1, 1}) == lw::NavGrid::kDiagonalFactor * (11 + 10 + 11 + 11));
    CHECK(nav.stepCost({1, 1}, {0, 0}) == nav.stepCost({0, 0}, {1, 1}));
    CHECK(nav.stepCost({4, 2}, {4, 3}) == lw::NavGrid::kStraightFactor * 21); // на мост
    CHECK(nav.stepCost({0, 2}, {1, 3}) == lw::NavGrid::kDiagonalFactor * (11 + 30 + 11 + 11)); // в брод
    CHECK(nav.stepCost({4, 2}, {5, 3}) == 0); // в озеро
    CHECK(nav.stepCost({5, 2}, {6, 3}) == 0); // мимо угла озера
    CHECK(nav.stepCost({0, 6}, {1, 5}) == 0); // мимо угла дома
    CHECK(nav.stepCost({3, 6}, {2, 5}) == 0);
    CHECK(nav.stepCost({3, 5}, {4, 6}) > 0);
    CHECK(nav.stepCost({4, 4}, {5, 2}) == 0); // не соседи

    // Области: море делит карту пополам, дом и озеро — нет.
    CHECK(nav.regionCount() == 2);
    CHECK(nav.connected({0, 0}, {7, 11}));
    CHECK(nav.connected({9, 0}, {15, 11}));
    CHECK(!nav.connected({0, 0}, {15, 0}));
    CHECK(!nav.connected({0, 0}, {8, 0}));
    CHECK(nav.region({8, 0}) == lw::NavGrid::kNoRegion);

    // Ближайшая проходимая клетка: из моря — по соседству, при равенстве — верхняя.
    CHECK((nav.nearestPassable({0, 0}, 3) == lw::Cell{0, 0}));
    CHECK((nav.nearestPassable({8, 5}, 3) == lw::Cell{7, 5}));
    CHECK((nav.nearestPassable({1, 7}, 3) == lw::Cell{0, 7}));
    CHECK(!nav.nearestPassable({8, 5}, 0).has_value());
}

// Карта со всеми видами местности, водой, дорогами и зданиями — для поиска пути.
struct NavFixture {
    NavFixture(std::uint64_t seed, int width, int height) : map(width, height) {
        lw::Rng rng(seed);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float roll = rng.unit();
                map.ground.at(x, y) = roll < 0.7f   ? lw::Ground::Grass
                                      : roll < 0.8f ? lw::Ground::Field
                                      : roll < 0.9f ? lw::Ground::Sand
                                                    : lw::Ground::Rock;
                map.cover.at(x, y) = rng.chance(0.3f) ? lw::Cover::Forest : lw::Cover::None;
            }
        }
        const auto setWater = [&](int x, int y, lw::Water water, int depth) {
            if (map.contains({x, y})) {
                map.water.at(x, y) = water;
                map.depth.at(x, y) = static_cast<std::uint8_t>(depth);
                map.cover.at(x, y) = lw::Cover::None;
            }
        };
        // Озёра — прямоугольники; бывают и такие, что отрезают кусок карты.
        for (int n = rng.rangeInt(2, 5); n > 0; --n) {
            const int x0 = rng.rangeInt(0, width - 4);
            const int y0 = rng.rangeInt(0, height - 4);
            const int w = rng.rangeInt(2, width / 3);
            const int h = rng.rangeInt(2, height / 3);
            for (int y = y0; y < y0 + h; ++y) {
                for (int x = x0; x < x0 + w; ++x) {
                    setWater(x, y, lw::Water::Lake, 20);
                }
            }
        }
        // Река с юга на север: ширина и глубина меняются по ходу.
        int riverX = rng.rangeInt(width / 4, 3 * width / 4);
        const int depths[] = {4, 12, 25};
        for (int y = 0; y < height; ++y) {
            riverX = std::clamp(riverX + rng.rangeInt(-1, 1), 1, width - 4);
            const int depth = depths[(y / 8 + static_cast<int>(seed)) % 3];
            for (int k = 0; k < 1 + (y / 5) % 3; ++k) {
                setWater(riverX + k, y, lw::Water::River, depth);
            }
        }
        // Дороги: прямые, через реку — мосты.
        for (int n = 0; n < 2; ++n) {
            const int row = rng.rangeInt(0, height - 1);
            const int column = rng.rangeInt(0, width - 1);
            for (int x = 0; x < width; ++x) {
                if (map.water.at(x, row) != lw::Water::Lake) {
                    map.road.at(x, row) = lw::Road::Dirt;
                    map.cover.at(x, row) = lw::Cover::None;
                }
            }
            for (int y = 0; y < height; ++y) {
                if (map.water.at(column, y) != lw::Water::Lake) {
                    map.road.at(column, y) = lw::Road::Asphalt;
                    map.cover.at(column, y) = lw::Cover::None;
                }
            }
        }
        for (int n = 0; n < width * height / 60; ++n) {
            const lw::Cell origin{rng.rangeInt(0, width - 3), rng.rangeInt(0, height - 4)};
            bool free = true;
            for (int y = origin.y; y < origin.y + 3; ++y) {
                for (int x = origin.x; x < origin.x + 2; ++x) {
                    free = free && map.road.at(x, y) == lw::Road::None &&
                           map.water.at(x, y) == lw::Water::None;
                }
            }
            if (free) {
                registry.emplace<lw::Building>(registry.create(), lw::BuildingKind::House, origin, 2, 3,
                                               lw::Cell{origin.x - 1, origin.y}, lw::kInvalidId);
            }
        }
        nav.build(map, registry);
        graph.build(nav);
    }

    lw::Cell randomPassable(lw::Rng& rng) const {
        for (;;) {
            const lw::Cell cell{rng.rangeInt(0, nav.width() - 1), rng.rangeInt(0, nav.height() - 1)};
            if (nav.passable(cell)) {
                return cell;
            }
        }
    }

    lw::Map map;
    entt::registry registry;
    lw::NavGrid nav;
    lw::NavGraph graph;
};

constexpr std::uint32_t kNoPath = 0xFFFFFFFFu;

// Стоимость кратчайшего пути от source до каждой клетки — Дейкстра по всем клеткам.
std::vector<std::uint32_t> shortestCosts(const lw::NavGrid& nav, lw::Cell source) {
    const int width = nav.width();
    std::vector<std::uint32_t> cost(static_cast<std::size_t>(width) * static_cast<std::size_t>(nav.height()),
                                    kNoPath);
    using Entry = std::pair<std::uint32_t, std::uint32_t>;
    std::vector<Entry> heap;
    const auto index = [width](int x, int y) { return static_cast<std::uint32_t>(y * width + x); };
    cost[index(source.x, source.y)] = 0;
    heap.emplace_back(0, index(source.x, source.y));
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<>{});
        const auto [g, i] = heap.back();
        heap.pop_back();
        if (g != cost[i]) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;
        for (int d = 0; d < 8; ++d) {
            const std::uint32_t step = nav.stepCost(x, y, d);
            if (step == 0) {
                continue;
            }
            const std::uint32_t n = index(x + lw::kNavDx[d], y + lw::kNavDy[d]);
            if (g + step < cost[n]) {
                cost[n] = g + step;
                heap.emplace_back(g + step, n);
                std::push_heap(heap.begin(), heap.end(), std::greater<>{});
            }
        }
    }
    return cost;
}

// Путь связен, начинается и кончается где надо, его стоимость сходится,
// сглаженные отрезки проходимы, а их концы — клетки пути по порядку.
bool validPath(const lw::NavGrid& nav, const lw::Path& path, lw::Cell start, lw::Cell goal) {
    if (path.cells.empty() || !(path.cells.front() == start) || !(path.cells.back() == goal)) {
        return false;
    }
    std::uint64_t sum = 0;
    for (std::size_t i = 1; i < path.cells.size(); ++i) {
        const std::uint32_t step = nav.stepCost(path.cells[i - 1], path.cells[i]);
        if (step == 0) {
            return false;
        }
        sum += step;
    }
    if (sum != path.cost || path.waypoints.empty() || !(path.waypoints.front() == start) ||
        !(path.waypoints.back() == goal)) {
        return false;
    }
    std::size_t next = 0;
    for (std::size_t i = 0; i < path.waypoints.size(); ++i) {
        if (i > 0 && !nav.segmentPassable(path.waypoints[i - 1], path.waypoints[i])) {
            return false;
        }
        while (next < path.cells.size() && !(path.cells[next] == path.waypoints[i])) {
            ++next;
        }
        if (next == path.cells.size()) {
            return false;
        }
    }
    return true;
}

// Граф навигации: узлы — куски клеток одного вида в своём секторе, каждая
// проходимая клетка принадлежит ровно одному узлу, рёбра симметричны.
void checkNavGraph(const lw::NavGrid& nav, const lw::NavGraph& graph) {
    const std::vector<lw::NavGraph::Node>& nodes = graph.nodes();
    CHECK(!nodes.empty());
    bool nodesOk = true;
    bool edgesOk = true;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const lw::NavGraph::Node& node = nodes[i];
        nodesOk = nodesOk && lw::NavGrid::classOf(nav.cost(node.cell)) == node.kind &&
                  graph.nodeOf(nav, node.cell) == i && node.cost >= lw::NavGrid::kRoadCost;
        for (const lw::NavGraph::Edge& edge : graph.edges(i)) {
            const auto back = graph.edges(edge.to);
            const bool symmetric = std::any_of(back.begin(), back.end(), [&](const lw::NavGraph::Edge& e) {
                return e.to == i && e.cost == edge.cost;
            });
            const lw::Cell a = node.cell;
            const lw::Cell b = nodes[edge.to].cell;
            const auto dx = static_cast<std::uint32_t>(std::abs(a.x - b.x));
            const auto dy = static_cast<std::uint32_t>(std::abs(a.y - b.y));
            const std::uint32_t octile = lw::NavGrid::kRoadStraightStep * (std::max(dx, dy) - std::min(dx, dy)) +
                                         lw::NavGrid::kRoadDiagonalStep * std::min(dx, dy);
            edgesOk = edgesOk && symmetric && edge.to != i && edge.cost >= octile;
        }
    }
    CHECK(nodesOk);
    CHECK(edgesOk);

    bool cellsOk = true;
    for (int y = 0; y < nav.height(); ++y) {
        for (int x = 0; x < nav.width(); ++x) {
            const std::uint32_t node = graph.nodeOf(nav, {x, y});
            if (!nav.passable({x, y})) {
                cellsOk = cellsOk && node == lw::NavGraph::kNoNode;
                continue;
            }
            cellsOk = cellsOk && node < nodes.size() &&
                      nodes[node].kind == lw::NavGrid::classOf(nav.cost({x, y})) &&
                      (nodes[node].cell.x >> lw::NavGraph::kSectorShift) == (x >> lw::NavGraph::kSectorShift) &&
                      (nodes[node].cell.y >> lw::NavGraph::kSectorShift) == (y >> lw::NavGraph::kSectorShift);
        }
    }
    CHECK(cellsOk);
}

// Прямой поиск оптимален, граф даёт верный путь не дешевле оптимального, пути нет —
// только между разными областями. Проверяется на случайных картах.
void testPathfinderOnRandomMaps() {
    lw::PathfinderSettings graphOnly;
    graphOnly.directRange = -1;    // всегда через граф
    graphOnly.corridorMargin = 0;  // коридор — только секторы маршрута
    graphOnly.refineSpan = 5;      // много промежуточных целей
    lw::PathfinderSettings graphWide;
    graphWide.directRange = -1;
    graphWide.corridorMargin = 16;
    graphWide.refineSpan = 1000;

    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        const NavFixture fixture(seed, 72, 56);
        const lw::NavGrid& nav = fixture.nav;
        checkNavGraph(nav, fixture.graph);

        lw::Pathfinder direct(nav, fixture.graph);
        lw::Pathfinder viaGraph(nav, fixture.graph, graphOnly);
        lw::Pathfinder viaWideGraph(nav, fixture.graph, graphWide);
        lw::Rng rng(seed * 977);
        bool optimal = true;
        bool valid = true;
        bool graphOk = true;
        bool reachability = true;
        int unreachable = 0;
        for (int q = 0; q < 25; ++q) {
            const lw::Cell start = fixture.randomPassable(rng);
            const std::vector<std::uint32_t> best = shortestCosts(nav, start);
            for (int k = 0; k < 6; ++k) {
                const lw::Cell goal = fixture.randomPassable(rng);
                const std::uint32_t expected =
                    best[static_cast<std::size_t>(goal.y * nav.width() + goal.x)];
                const lw::Path a = direct.find(start, goal);
                const lw::Path b = viaGraph.find(start, goal);
                const lw::Path c = viaWideGraph.find(start, goal);
                if (expected == kNoPath) {
                    ++unreachable;
                    reachability = reachability && a.status == lw::PathStatus::Unreachable &&
                                   b.status == lw::PathStatus::Unreachable && a.expanded == 0;
                    continue;
                }
                optimal = optimal && a.found() && a.method == lw::PathMethod::Direct && a.cost == expected;
                valid = valid && validPath(nav, a, start, goal) && validPath(nav, b, start, goal) &&
                        validPath(nav, c, start, goal);
                graphOk = graphOk && b.method == lw::PathMethod::Graph && b.cost >= expected &&
                          c.method == lw::PathMethod::Graph && c.cost >= expected;
            }
        }
        CHECK(optimal);
        CHECK(valid);
        CHECK(graphOk);
        CHECK(reachability);
        (void)unreachable;
    }

    // Непроходимые начало или цель; путь в саму себя.
    const NavFixture fixture(3, 72, 56);
    lw::Pathfinder finder(fixture.nav, fixture.graph);
    lw::Cell blocked{-1, -1};
    for (const auto [entity, building] : fixture.registry.view<const lw::Building>().each()) {
        blocked = building.origin;
        break;
    }
    lw::Rng rng(5);
    const lw::Cell somewhere = fixture.randomPassable(rng);
    CHECK(finder.find(blocked, somewhere).status == lw::PathStatus::Blocked);
    CHECK(finder.find(somewhere, blocked).status == lw::PathStatus::Blocked);
    CHECK(finder.find(somewhere, {-5, 3}).status == lw::PathStatus::Blocked);
    const lw::Path self = finder.find(somewhere, somewhere);
    CHECK(self.found() && self.cost == 0 && self.cells.size() == 1 && self.waypoints.size() == 1);

    // Без графа остаётся запасной поиск по всей карте.
    const lw::NavGraph empty;
    lw::PathfinderSettings noDirect;
    noDirect.directRange = -1;
    lw::Pathfinder fallback(fixture.nav, empty, noDirect);
    lw::Pathfinder reference(fixture.nav, fixture.graph);
    for (int q = 0; q < 10; ++q) {
        const lw::Cell start = fixture.randomPassable(rng);
        const lw::Cell goal = fixture.randomPassable(rng);
        const lw::Path path = fallback.find(start, goal);
        const lw::Path best = reference.find(start, goal);
        CHECK(path.status == best.status);
        if (path.found()) {
            CHECK(path.method == lw::PathMethod::Full && path.cost == best.cost);
        }
    }
}

// Ответ не зависит от предыдущих запросов и от того, какой Pathfinder его считает.
void testPathfinderDeterminism() {
    const NavFixture fixture(11, 96, 80);
    lw::PathfinderSettings settings;
    settings.directRange = 20; // и прямой поиск, и граф
    lw::Pathfinder first(fixture.nav, fixture.graph, settings);
    lw::Pathfinder second(fixture.nav, fixture.graph, settings);
    lw::Rng rng(21);
    std::vector<std::pair<lw::Cell, lw::Cell>> queries;
    for (int i = 0; i < 40; ++i) {
        queries.emplace_back(fixture.randomPassable(rng), fixture.randomPassable(rng));
    }
    std::vector<lw::Path> forward;
    for (const auto& [start, goal] : queries) {
        forward.push_back(first.find(start, goal));
    }
    bool same = true;
    for (std::size_t i = queries.size(); i-- > 0;) {
        const lw::Path path = second.find(queries[i].first, queries[i].second);
        same = same && path.status == forward[i].status && path.method == forward[i].method &&
               path.cost == forward[i].cost && path.cells == forward[i].cells &&
               path.waypoints == forward[i].waypoints;
    }
    CHECK(same);
}

// Навигация сгенерированного мира: дома непроходимы, двери и центры поселений
// связаны, пути между поселениями идут по графу и почти не дороже оптимальных.
void testWorldNavigation() {
    const lw::World world(7, regionConfig());
    const lw::NavGrid& nav = world.nav();
    const entt::registry& registry = world.registry();

    std::vector<lw::Cell> centers;
    for (const auto [entity, settlement] : registry.view<const lw::Settlement>().each()) {
        centers.push_back(settlement.center);
    }
    bool buildings = true;
    for (const auto [entity, building] : registry.view<const lw::Building>().each()) {
        buildings = buildings && !nav.passable(building.origin) && nav.passable(building.door) &&
                    nav.connected(building.door, centers.front());
    }
    CHECK(buildings);
    checkNavGraph(nav, world.navGraph());

    lw::Pathfinder finder(nav, world.navGraph());
    const std::vector<std::uint32_t> best = shortestCosts(nav, centers.front());
    bool found = true;
    bool valid = true;
    bool viaGraph = false;
    double worst = 0.0;
    for (std::size_t i = 1; i < centers.size(); ++i) {
        const lw::Cell goal = centers[i];
        const lw::Path path = finder.find(centers.front(), goal);
        const std::uint32_t optimum = best[static_cast<std::size_t>(goal.y * nav.width() + goal.x)];
        found = found && path.found();
        if (!path.found()) {
            continue;
        }
        valid = valid && validPath(nav, path, centers.front(), goal) && path.cost >= optimum;
        viaGraph = viaGraph || path.method == lw::PathMethod::Graph;
        worst = std::max(worst, static_cast<double>(path.cost) / optimum);
    }
    CHECK(found);
    CHECK(valid);
    CHECK(viaGraph);
    CHECK(worst < 1.1);

    // Тот же мир — те же пути.
    const lw::World twin(7, regionConfig());
    lw::Pathfinder twinFinder(twin.nav(), twin.navGraph());
    const lw::Path a = finder.find(centers.front(), centers.back());
    const lw::Path b = twinFinder.find(centers.front(), centers.back());
    CHECK(a.cells == b.cells && a.cost == b.cost);
}


// --- Люди --------------------------------------------------------------------

void testWalkSpeed() {
    CHECK(lw::walkSpeedFactor(lw::NavGrid::kBlocked) == 0.0f);
    CHECK(lw::walkSpeedFactor(lw::NavGrid::kRoadCost) == 1.0f);
    CHECK(lw::walkSpeedFactor(11) < 1.0f && lw::walkSpeedFactor(15) < lw::walkSpeedFactor(11));
    const float ford = lw::walkSpeedFactor(lw::NavGrid::kWaterCost);
    const float swim = lw::walkSpeedFactor(250);
    CHECK(ford < lw::walkSpeedFactor(15) && swim < ford && swim > 0.0f);
}

void testPeople() {
    lw::WorldConfig config = regionConfig();
    config.people = 150;
    lw::World world(7, config);
    const entt::registry& registry = world.registry();
    const lw::NavGrid& nav = world.nav();

    // Все живут в жилых домах и начинают у своей двери.
    bool housed = true;
    for (const auto [entity, person, position] :
         registry.view<const lw::Person, const lw::Position>().each()) {
        const entt::entity home = world.find(person.home);
        housed = housed && home != entt::null;
        if (home == entt::null) {
            continue;
        }
        const lw::Building& building = registry.get<lw::Building>(home);
        housed = housed &&
                 (building.kind == lw::BuildingKind::House ||
                  building.kind == lw::BuildingKind::Apartment) &&
                 lw::Map::cellAt(position.x, position.y) == building.door;
    }
    CHECK(registry.view<const lw::Person>().size() == 150);
    CHECK(housed);

    const auto atHome = [&]() {
        std::size_t count = 0;
        for (const auto [entity, person, position] :
             registry.view<const lw::Person, const lw::Position>().each()) {
            const lw::Building& building = registry.get<lw::Building>(world.find(person.home));
            count += lw::Map::cellAt(position.x, position.y) == building.door ? 1 : 0;
        }
        return count;
    };

    // Тот же seed — те же маршруты.
    lw::World twin(7, config);
    for (int i = 0; i < 3000; ++i) {
        world.step();
        twin.step();
    }
    CHECK(world.stateHash() == twin.stateHash());

    // Днём (до 12:00) люди расходятся и ходят только по проходимым клеткам.
    bool onLand = true;
    std::size_t walked = 0;
    while (lw::calendarAt(world.tick()).hour < 12) {
        world.step();
        if (world.tick() % 100 == 0) {
            for (const auto [entity, position, walker] :
                 registry.view<const lw::Position, const lw::Walker>().each()) {
                onLand = onLand && nav.passable(lw::Map::cellAt(position.x, position.y));
                walked += walker.moving() ? 1 : 0;
            }
        }
    }
    CHECK(onLand);
    CHECK(walked > 0);
    CHECK(atHome() < 120);

    // К трём часам ночи почти все дома (кто ушёл навестить родню или друзей
    // в соседнее поселение, ночует там — этап 7.3).
    const std::uint64_t night = lw::nextTimeOfDay(world.tick(), 3, 0);
    while (world.tick() < night) {
        world.step();
    }
    CHECK(atHome() >= 135);
}

// Шаг движения может быть длиннее тика: за один шаг человек проходит весь путь.
void testLongMovementStep() {
    lw::WorldConfig config = regionConfig();
    config.people = 1;
    lw::World world(7, config);
    entt::registry& registry = world.registry();
    const entt::entity person = *registry.view<lw::Person>().begin();

    lw::Cell goal{};
    float farthest = 0.0f;
    const lw::Position start = registry.get<lw::Position>(person);
    for (const auto [entity, building] : registry.view<const lw::Building>().each()) {
        const float dx = lw::Map::cellCenter(building.door.x) - start.x;
        const float dy = lw::Map::cellCenter(building.door.y) - start.y;
        if (dx * dx + dy * dy > farthest && world.nav().connected(building.door,
                                                                  lw::Map::cellAt(start.x, start.y))) {
            farthest = dx * dx + dy * dy;
            goal = building.door;
        }
    }
    CHECK(farthest > 0.0f);
    CHECK(lw::startWalk(world, person, goal));
    CHECK(registry.get<lw::Walker>(person).moving());
    lw::updateMovement(world, 1.0e6f);
    const lw::Position& end = registry.get<lw::Position>(person);
    CHECK(lw::Map::cellAt(end.x, end.y) == goal);
    CHECK(!registry.get<lw::Walker>(person).moving());
    lw::updateMovement(world, lw::kTickDt);
    const lw::Velocity& velocity = registry.get<lw::Velocity>(person);
    CHECK(velocity.x == 0.0f && velocity.y == 0.0f);
}

// На этом держится интерполяция при отрисовке: Velocity — смещение за последний
// тик, делённое на kTickDt, поэтому позиция минус Velocity · kTickDt — позиция
// на начало тика.
void testVelocityIsTickDisplacement() {
    lw::WorldConfig config = regionConfig();
    config.people = 60;
    lw::World world(7, config);
    entt::registry& registry = world.registry();
    std::vector<std::pair<entt::entity, lw::Position>> before;
    std::size_t moved = 0;
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        before.clear();
        for (const auto [entity, position] : registry.view<const lw::Position>().each()) {
            before.emplace_back(entity, position);
        }
        world.step();
        for (const auto& [entity, start] : before) {
            const lw::Position& now = registry.get<lw::Position>(entity);
            const lw::Velocity& velocity = registry.get<lw::Velocity>(entity);
            const float dx = now.x - velocity.x * lw::kTickDt - start.x;
            const float dy = now.y - velocity.y * lw::kTickDt - start.y;
            worst = std::max(worst, std::max(std::abs(dx), std::abs(dy)));
            moved += now.x != start.x || now.y != start.y ? 1 : 0;
        }
    }
    CHECK(moved > 0);
    CHECK(worst < 2e-3f); // миллиметры: погрешность float на координатах в километры
}

// Нужды: скорости в игровых часах, сон, компания, здоровье на пределе.
void testNeeds() {
    lw::World world(3, smallConfig(2));
    entt::registry& registry = world.registry();
    std::vector<entt::entity> people;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        people.push_back(entity);
    }
    CHECK(people.size() == 2);
    if (people.size() != 2) {
        return;
    }
    const entt::entity a = people[0];
    const entt::entity b = people[1];
    // Далеко друг от друга, днём, без дома: одиноки и не боятся.
    registry.get<lw::Position>(a) = {20.0f, 20.0f};
    registry.get<lw::Position>(b) = {200.0f, 200.0f};
    registry.get<lw::Needs>(a) = {};
    registry.get<lw::Needs>(b) = {};
    registry.get<lw::Traits>(a) = {}; // средний характер: скорости без поправок
    registry.get<lw::Traits>(b) = {};
    registry.get<lw::Person>(b).asleep = true;
    registry.get<lw::Needs>(b).fatigue = 0.8f;
    world.syncSpatial();

    for (int i = 0; i < 120; ++i) {
        lw::updateNeeds(world, 1.0f);
    }
    const lw::Needs awake = registry.get<lw::Needs>(a);
    const lw::Needs sleeping = registry.get<lw::Needs>(b);
    // Два часа: голод — 2/48, жажда — 2/30, усталость — 2/18.
    CHECK(std::abs(awake.hunger - 2.0f / 48.0f) < 1e-3f);
    CHECK(std::abs(awake.thirst - 2.0f / 30.0f) < 1e-3f);
    CHECK(std::abs(awake.fatigue - 2.0f / 18.0f) < 1e-3f);
    CHECK(awake.loneliness > 0.05f && sleeping.loneliness == 0.0f);
    CHECK(sleeping.hunger < awake.hunger && sleeping.fatigue < 0.8f - 0.15f);
    CHECK(awake.health == 1.0f);

    // Один шаг на два часа — то же, что 120 шагов по минуте.
    registry.get<lw::Needs>(a) = {};
    lw::updateNeeds(world, 120.0f);
    const lw::Needs coarse = registry.get<lw::Needs>(a);
    CHECK(std::abs(coarse.hunger - awake.hunger) < 1e-4f);
    CHECK(std::abs(coarse.fatigue - awake.fatigue) < 1e-4f);

    // Рядом — компания: одиночество уходит.
    registry.get<lw::Needs>(a).loneliness = 0.5f;
    registry.get<lw::Position>(b) = {25.0f, 20.0f};
    world.syncSpatial();
    lw::updateNeeds(world, 60.0f);
    CHECK(registry.get<lw::Needs>(a).loneliness < 0.5f - 0.2f);

    // На пределе голода и жажды здоровье уходит, в норме — восстанавливается.
    lw::Needs& starving = registry.get<lw::Needs>(a);
    starving = {};
    starving.hunger = 1.0f;
    starving.thirst = 1.0f;
    lw::updateNeeds(world, 60.0f * 12.0f);
    const float hurt = registry.get<lw::Needs>(a).health;
    CHECK(hurt < 1.0f - 0.4f);
    CHECK(registry.get<lw::Needs>(a).fear > 0.1f); // плохо — страшно
    registry.get<lw::Needs>(a).hunger = 0.0f;
    registry.get<lw::Needs>(a).thirst = 0.0f;
    lw::updateNeeds(world, 60.0f);
    CHECK(registry.get<lw::Needs>(a).health > hurt);
}

// Черты: колокол вокруг 0,5, лидерство тянется к смелости, один seed — одни характеры.
void testTraits() {
    lw::Rng rng(17);
    constexpr int kCount = 4000;
    std::vector<lw::Traits> all;
    for (int i = 0; i < kCount; ++i) {
        all.push_back(lw::generateTraits(rng));
    }
    bool inRange = true;
    double meanBravery = 0.0;
    double meanLeadership = 0.0;
    int extreme = 0;
    for (const lw::Traits& t : all) {
        for (const float v : {t.bravery, t.greed, t.altruism, t.aggression, t.leadership,
                              t.sociability, t.curiosity, t.prudence, t.chronotype}) {
            inRange = inRange && v >= 0.0f && v <= 1.0f;
        }
        meanBravery += t.bravery;
        meanLeadership += t.leadership;
        extreme += t.curiosity < 0.1f || t.curiosity > 0.9f ? 1 : 0;
    }
    meanBravery /= kCount;
    meanLeadership /= kCount;
    CHECK(inRange);
    CHECK(std::abs(meanBravery - 0.5) < 0.02);
    CHECK(extreme < kCount / 50); // у колокола края редки: около 0,9 %
    double covariance = 0.0;
    for (const lw::Traits& t : all) {
        covariance += (t.bravery - meanBravery) * (t.leadership - meanLeadership);
    }
    CHECK(covariance > 0.0);

    // Люди мира получают черты при рождении, один seed — одни характеры.
    const lw::World a(5, smallConfig(20));
    const lw::World b(5, smallConfig(20));
    std::vector<float> braveryA;
    std::vector<float> braveryB;
    for (const auto [entity, traits] : a.registry().view<const lw::Traits>().each()) {
        braveryA.push_back(traits.bravery);
    }
    for (const auto [entity, traits] : b.registry().view<const lw::Traits>().each()) {
        braveryB.push_back(traits.bravery);
    }
    CHECK(braveryA.size() == 20);
    CHECK(braveryA == braveryB);
}

// --- Utility AI --------------------------------------------------------------

// Справочник мест: поселения с зданиями, места для питья и сбора достижимы и
// лежат там, где обещано.
void testPlaces() {
    const lw::World world(7, regionConfig());
    const lw::Places& places = world.places();
    const lw::Map& map = world.map();
    const lw::NavGrid& nav = world.nav();
    CHECK(!places.settlements().empty());
    CHECK(places.waterSpotCount() > 0 && places.forageSpotCount() > 0);
    std::size_t buildings = 0;
    bool sorted = true;
    for (const auto& settlement : places.settlements()) {
        buildings += settlement.buildings.size();
        sorted = sorted && std::is_sorted(settlement.buildings.begin(), settlement.buildings.end(),
                                          [](const auto& a, const auto& b) { return a.id < b.id; });
        CHECK(places.settlement(settlement.id) == &settlement);
    }
    CHECK(buildings == world.registry().view<const lw::Building>().size());
    CHECK(sorted);

    const auto fresh = [&](lw::Cell c) {
        const lw::Water w = map.water.at(c.x, c.y);
        return w == lw::Water::Lake || w == lw::Water::River;
    };
    lw::Rng rng(3);
    int found = 0;
    bool valid = true;
    for (int i = 0; i < 200; ++i) {
        const lw::Cell from{static_cast<int>(rng.below(1024)), static_cast<int>(rng.below(1024))};
        if (!nav.passable(from)) {
            continue;
        }
        if (const auto water = places.nearestWater(nav, from, 1500.0f)) {
            ++found;
            bool shore = fresh(*water);
            for (int d = 0; d < 4; ++d) {
                const lw::Cell next{water->x + lw::kNavDx[d], water->y + lw::kNavDy[d]};
                shore = shore || (map.contains(next) && fresh(next));
            }
            const float dx = lw::Map::cellCenter(water->x) - lw::Map::cellCenter(from.x);
            const float dy = lw::Map::cellCenter(water->y) - lw::Map::cellCenter(from.y);
            valid = valid && shore && nav.connected(from, *water) &&
                    dx * dx + dy * dy <= 1500.0f * 1500.0f;
        }
        if (const auto forest = places.nearestForage(nav, from, 1500.0f)) {
            valid = valid && map.cover.at(forest->x, forest->y) == lw::Cover::Forest &&
                    nav.connected(from, *forest);
        }
    }
    CHECK(found > 0);
    CHECK(valid);
}

void testPersonalNight() {
    // Жаворонок: 21:00–5:30, сова: 23:30–8:00.
    CHECK(lw::isPersonalNight(0.0f, 21 * 60) && lw::isPersonalNight(0.0f, 3 * 60));
    CHECK(!lw::isPersonalNight(0.0f, 6 * 60) && !lw::isPersonalNight(0.0f, 20 * 60));
    CHECK(!lw::isPersonalNight(1.0f, 22 * 60) && lw::isPersonalNight(1.0f, 7 * 60));
    CHECK(lw::isPersonalNight(1.0f, 23 * 60 + 45) && !lw::isPersonalNight(1.0f, 8 * 60 + 5));
}

// Мир из одного человека: его сущность и дом.
struct Lone {
    lw::World world;
    entt::entity person = entt::null;
    entt::entity home = entt::null;

    Lone() : world(7, [] {
                 lw::WorldConfig config = regionConfig();
                 config.people = 1;
                 return config;
             }()) {
        entt::registry& registry = world.registry();
        person = *registry.view<lw::Person>().begin();
        home = world.find(registry.get<lw::Person>(person).home);
        registry.get<lw::Traits>(person) = {};
        registry.get<lw::Needs>(person) = {};
    }

    void run(int gameMinutes) {
        const std::uint64_t end =
            world.tick() + static_cast<std::uint64_t>(gameMinutes) * lw::kTicksPerGameMinute;
        while (world.tick() < end) {
            world.step();
        }
    }
};

// Голодный дома ест из своих запасов.
void testAiEatsAtHome() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    CHECK(lone.home != entt::null);
    if (lone.home == entt::null) {
        return;
    }
    registry.get<lw::Inventory>(lone.home).set(lw::ItemKind::Food, 5);
    registry.get<lw::Needs>(lone.person).hunger = 0.9f;
    const lw::ActionScores scores = lw::evaluateActions(lone.world, lone.person);
    CHECK(scores[static_cast<std::size_t>(lw::Action::Eat)] >
          scores[static_cast<std::size_t>(lw::Action::Wander)]);
    lone.run(90);
    CHECK(registry.get<lw::Needs>(lone.person).hunger < 0.6f);
    CHECK(registry.get<lw::Inventory>(lone.home).food() < 5);
}

// Дома нет воды — жажду утоляют у реки или в чужих домах.
void testAiFindsWater() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    if (lone.home == entt::null) {
        return;
    }
    registry.get<lw::Inventory>(lone.home).set(lw::ItemKind::Water, 0);
    registry.get<lw::Needs>(lone.person).thirst = 0.95f;
    lone.run(8 * 60);
    CHECK(registry.get<lw::Needs>(lone.person).thirst < 0.6f);
    CHECK(registry.get<lw::Needs>(lone.person).health > 0.9f);
}

// В домах нет еды — человек идёт искать её в чужих зданиях или в лес и
// помнит, где уже искал.
void testAiScavengesWhenHungry() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    for (const auto [entity, supplies] : registry.view<lw::Inventory>().each()) {
        supplies.set(lw::ItemKind::Food, 0);
    }
    registry.get<lw::Needs>(lone.person).hunger = 0.8f;
    bool searched = false;
    for (int minute = 0; minute < 8 * 60; minute += 10) {
        lone.run(10);
        const lw::Mind& mind = registry.get<lw::Mind>(lone.person);
        searched = searched || mind.action == lw::Action::Scavenge ||
                   mind.action == lw::Action::Forage;
    }
    CHECK(searched);
    const lw::Mind& mind = registry.get<lw::Mind>(lone.person);
    const bool remembers = std::any_of(std::begin(mind.searched), std::end(mind.searched),
                                       [](lw::EntityId id) { return id != lw::kInvalidId; });
    CHECK(remembers || mind.action == lw::Action::Forage);
}

// Найденное в чужом доме несут домой и выкладывают в запасы.
void testAiCarriesHome() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    if (lone.home == entt::null) {
        return;
    }
    registry.get<lw::Inventory>(lone.home) = {};
    registry.get<lw::Inventory>(lone.person).set(lw::ItemKind::Food, 3);
    registry.get<lw::Inventory>(lone.person).set(lw::ItemKind::Water, 2);
    // Уйти от дома и вернуться: на пороге запасы перекладываются.
    const lw::Cell door = registry.get<lw::Building>(lone.home).door;
    const auto away = lone.world.nav().nearestPassable({door.x + 12, door.y + 12}, 8);
    CHECK(away.has_value());
    if (!away) {
        return;
    }
    registry.get<lw::Position>(lone.person) = {lw::Map::cellCenter(away->x),
                                               lw::Map::cellCenter(away->y)};
    registry.get<lw::Needs>(lone.person).fear = 1.0f; // страшно — домой
    lw::Mind& mind = registry.get<lw::Mind>(lone.person);
    mind.phase = lw::ActionPhase::Perform;
    mind.action = lw::Action::Idle;
    mind.until = lone.world.tick();
    lone.world.syncSpatial();
    lone.run(60);
    const lw::Inventory& supplies = registry.get<lw::Inventory>(lone.home);
    const lw::Inventory& carry = registry.get<lw::Inventory>(lone.person);
    // Что-то могли уже съесть и выпить, но остальное — дома, а не в руках.
    CHECK(carry.food() == 0 && carry.count(lw::ItemKind::Water) == 0);
    CHECK(supplies.food() + supplies.count(lw::ItemKind::Water) > 0);
}

// Путь оборвался не у цели — действие там не выполняется: нельзя поесть из
// домашних запасов вдали от дома.
void testAiArrivesElsewhere() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    if (lone.home == entt::null) {
        return;
    }
    const lw::Cell door = registry.get<lw::Building>(lone.home).door;
    const auto away = lone.world.nav().nearestPassable({door.x + 12, door.y + 12}, 8);
    CHECK(away.has_value());
    if (!away) {
        return;
    }
    registry.get<lw::Position>(lone.person) = {lw::Map::cellCenter(away->x),
                                               lw::Map::cellCenter(away->y)};
    registry.get<lw::Inventory>(lone.home).set(lw::ItemKind::Food, 5);
    registry.get<lw::Needs>(lone.person).hunger = 0.7f;
    registry.get<lw::Walker>(lone.person).waypoints.clear();
    lw::Mind& mind = registry.get<lw::Mind>(lone.person);
    mind.action = lw::Action::Eat;
    mind.phase = lw::ActionPhase::Travel;
    mind.target = registry.get<lw::Id>(lone.home).value;
    mind.spot = door;
    lone.world.syncSpatial();

    // Выполняет действие — значит, стоит на его клетке; ест из домашних
    // запасов — значит, у своей двери.
    bool inPlace = true;
    bool ateAtHome = true;
    int food = registry.get<lw::Inventory>(lone.home).food();
    for (int i = 0; i < 60 * static_cast<int>(lw::kTicksPerGameMinute); ++i) {
        lone.world.step();
        const lw::Position& position = registry.get<lw::Position>(lone.person);
        const lw::Cell cell = lw::Map::cellAt(position.x, position.y);
        const lw::Mind& now = registry.get<lw::Mind>(lone.person);
        inPlace = inPlace && (now.phase != lw::ActionPhase::Perform || cell == now.spot);
        const int left = registry.get<lw::Inventory>(lone.home).food();
        ateAtHome = ateAtHome && (left == food || cell == door);
        food = left;
    }
    CHECK(inPlace);
    CHECK(ateAtHome);
    CHECK(food < 5); // дошёл и поел
}

// Запасливый ходит за едой, пока дома пусто, но не с полными руками.
void testAiStocksUp() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    if (lone.home == entt::null) {
        return;
    }
    registry.get<lw::Traits>(lone.person).prudence = 1.0f;
    registry.get<lw::Inventory>(lone.home).set(lw::ItemKind::Food, 0);
    const auto scavenge = [&] {
        return lw::evaluateActions(lone.world, lone.person)[static_cast<std::size_t>(
            lw::Action::Scavenge)];
    };
    CHECK(scavenge() > 0.0f); // 08:00, сыт, но дома пусто
    registry.get<lw::Inventory>(lone.person).set(lw::ItemKind::Food, 8);
    CHECK(scavenge() == 0.0f);
}

// До дома не дойти (человек в другой связной области) — ночью он спит там,
// где стоит, а не пытается без конца проложить путь.
void testAiSleepsWhereStranded() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    if (lone.home == entt::null) {
        return;
    }
    const lw::NavGrid& nav = lone.world.nav();
    const lw::NavGrid::Region mainRegion =
        nav.region(registry.get<lw::Building>(lone.home).door);
    // Клетки других связных областей (острова, закутки между водой и домами).
    std::vector<lw::Cell> islands;
    for (int y = 0; y < nav.height(); ++y) {
        for (int x = 0; x < nav.width(); ++x) {
            const lw::NavGrid::Region region = nav.region({x, y});
            if (region != lw::NavGrid::kNoRegion && region != lw::NavGrid::kUnknownRegion &&
                region != mainRegion) {
                islands.push_back({x, y});
            }
        }
    }
    // Здание, от двери которого до такой клетки ближе всего, — новый дом.
    std::optional<lw::Cell> stranded;
    entt::entity home = entt::null;
    long long best = 0;
    for (const auto [entity, building] : registry.view<const lw::Building>().each()) {
        if (nav.region(building.door) != mainRegion) {
            continue;
        }
        for (const lw::Cell cell : islands) {
            const long long dx = cell.x - building.door.x;
            const long long dy = cell.y - building.door.y;
            if (!stranded || dx * dx + dy * dy < best) {
                stranded = cell;
                home = entity;
                best = dx * dx + dy * dy;
            }
        }
    }
    constexpr long long kMaxCells = 350; // 1,4 км: дом ещё «рядом», ночью к нему идут
    CHECK(stranded.has_value() && best <= kMaxCells * kMaxCells);
    if (!stranded || best > kMaxCells * kMaxCells) {
        return;
    }
    registry.get<lw::Person>(lone.person).home = registry.get<lw::Id>(home).value;
    registry.get<lw::Position>(lone.person) = {lw::Map::cellCenter(stranded->x),
                                               lw::Map::cellCenter(stranded->y)};
    registry.get<lw::Walker>(lone.person).waypoints.clear();
    lw::Mind& mind = registry.get<lw::Mind>(lone.person);
    mind.action = lw::Action::Idle;
    mind.phase = lw::ActionPhase::Perform;
    mind.until = lone.world.tick();
    lone.world.syncSpatial();

    const std::uint64_t night = lw::nextTimeOfDay(lone.world.tick(), 1, 0);
    while (lone.world.tick() < night) {
        lone.world.step();
    }
    CHECK(registry.get<lw::Person>(lone.person).asleep);
    CHECK(registry.get<lw::Mind>(lone.person).action == lw::Action::Sleep);
}

// Двое суток мира: люди едят, пьют и спят, здоровье не падает; ночью спят.
// Летопись: биография по id (субъект и объект), счётчики, накопленный хеш.
// Все лесные места по блокам Places.
std::vector<lw::Cell> forageSpots(const lw::World& world) {
    std::vector<lw::Cell> spots;
    for (const auto& spot : world.places().forageSpots().cells()) {
        if (spot) {
            spots.push_back(*spot);
        }
    }
    return spots;
}

// Запас леса: убывает при сборе, растёт по сезону до предела, зимой пропадает;
// дождь копится долями порции.
void testResources() {
    lw::World world(7, regionConfig());
    lw::Resources& resources = world.resources();
    const std::vector<lw::Cell> spots = forageSpots(world);
    CHECK(!spots.empty() && resources.forestBlocks() == static_cast<int>(spots.size()));
    if (spots.empty()) {
        return;
    }
    const lw::Cell spot = spots.front();
    CHECK(resources.forestAt(spot) == lw::Resources::kForestStart);
    CHECK(resources.forestTotal() ==
          static_cast<long long>(spots.size()) * lw::Resources::kForestStart);
    CHECK(resources.forestAt({-1, -1}) == 0 && resources.forestAt({100000, 5}) == 0);
    // Нелесной блок: запаса нет.
    bool barrenFound = false;
    for (int y = 0; y < world.map().height() && !barrenFound; y += lw::Places::kSpotBlock) {
        for (int x = 0; x < world.map().width() && !barrenFound; x += lw::Places::kSpotBlock) {
            if (!world.places().forageSpots().at(x / lw::Places::kSpotBlock,
                                                 y / lw::Places::kSpotBlock)) {
                barrenFound = true;
                CHECK(resources.forestAt({x, y}) == 0 && !resources.takeForest({x, y}));
            }
        }
    }
    CHECK(barrenFound);

    // Сбор: до нуля, дальше пусто.
    for (int i = 0; i < lw::Resources::kForestStart; ++i) {
        CHECK(resources.takeForest(spot));
    }
    CHECK(resources.forestAt(spot) == 0 && !resources.takeForest(spot));
    CHECK(resources.forestEmpty() == 1);
    // Пустое место ближайшим не считается.
    const auto nearest = world.places().nearestForage(world.nav(), spot, 500.0f);
    const auto stocked = world.places().nearestForage(
        world.nav(), spot, 500.0f, [&](lw::Cell c) { return resources.forestAt(c) > 0; });
    CHECK(nearest && *nearest == spot);
    CHECK(!stocked || !(*stocked == spot));

    // Весной порция за двое суток, летом — каждые сутки, до предела.
    resources.growForest(lw::Season::Spring, 1);
    CHECK(resources.forestAt(spot) == 0);
    resources.growForest(lw::Season::Spring, 2);
    CHECK(resources.forestAt(spot) == 1);
    for (int day = 0; day < 10; ++day) {
        resources.growForest(lw::Season::Summer, day);
    }
    CHECK(resources.forestAt(spot) == lw::Resources::kForestMax);
    CHECK(resources.forestEmpty() == 0);
    // Зима: за kForestMax суток пусто везде.
    for (int day = 0; day < lw::Resources::kForestMax; ++day) {
        resources.growForest(lw::Season::Winter, day);
    }
    CHECK(resources.forestTotal() == 0);
    CHECK(resources.forestEmpty() == resources.forestBlocks());

    // Ливень наполняет порцию за 180 минут, вполсилы — вдвое дольше.
    int portions = 0;
    for (int minute = 0; minute < 180; ++minute) {
        portions += resources.collectRain(1.0f, 1.0f);
    }
    CHECK(portions == 1);
    for (int minute = 0; minute < 350; ++minute) {
        portions += resources.collectRain(0.5f, 1.0f);
    }
    CHECK(portions == 1);
    for (int minute = 0; minute < 20; ++minute) {
        portions += resources.collectRain(0.5f, 1.0f);
    }
    CHECK(portions == 2 && resources.rainPortions() == 2);
    CHECK(resources.collectRain(0.0f, 1000.0f) == 0);

    // Запас — часть состояния.
    const lw::World other(7, regionConfig());
    CHECK(world.resources().hash() != other.resources().hash());
}

// Тёплый ливень наполняет бочки домов, многоэтажек и сараев до предела;
// в магазинах воды не прибавляется.
void testRainFillsBarrels() {
    lw::World world(7, regionConfig());
    entt::registry& registry = world.registry();
    for (const auto [entity, supplies] : registry.view<lw::Inventory>().each()) {
        supplies.set(lw::ItemKind::Water, 0);
    }
    lw::Weather& weather = world.weather();
    weather.kind = lw::WeatherKind::Storm;
    weather.minutesLeft = 100000;
    weather.cloudCover = 1.0f;
    weather.precipitation = 1.0f;
    weather.anomaly = 20.0f; // тепло: дождь, а не снег
    const std::uint64_t end = world.tick() + 24 * lw::kTicksPerGameHour;
    while (world.tick() < end) {
        world.step();
    }
    CHECK(world.resources().rainPortions() >= 4);
    bool filled = false;
    bool withinBarrel = true;
    bool shopsDry = true;
    for (const auto [entity, building, supplies] :
         registry.view<const lw::Building, const lw::Inventory>().each()) {
        const std::uint16_t barrel = lw::rainBarrel(building.kind);
        filled = filled || supplies.count(lw::ItemKind::Water) > 0;
        withinBarrel = withinBarrel && supplies.count(lw::ItemKind::Water) <= barrel;
        shopsDry = shopsDry && (building.kind != lw::BuildingKind::Shop ||
                                 supplies.count(lw::ItemKind::Water) == 0);
    }
    CHECK(filled && withinBarrel && shopsDry);
    // Сутки ливня — дом полон.
    for (const auto [entity, building, supplies] :
         registry.view<const lw::Building, const lw::Inventory>().each()) {
        if (building.kind == lw::BuildingKind::House) {
            CHECK(supplies.count(lw::ItemKind::Water) == lw::rainBarrel(building.kind));
            break;
        }
    }
}

// Голодный в обобранной округе после обыска без еды идёт в лес и вычерпывает
// запас; в обобранный лес не ходит.
void testAiForagesFromStock() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    for (const auto [entity, supplies] : registry.view<lw::Inventory>().each()) {
        supplies.set(lw::ItemKind::Food, 0);
    }
    lw::Resources& resources = lone.world.resources();
    const long long before = resources.forestTotal();
    registry.get<lw::Needs>(lone.person).hunger = 0.8f;
    bool foraged = false;
    for (int minute = 0; minute < 12 * 60; minute += 10) {
        lone.run(10);
        foraged = foraged || registry.get<lw::Mind>(lone.person).action == lw::Action::Forage;
    }
    CHECK(foraged);
    CHECK(resources.forestTotal() < before);

    // Лес пуст — в лес не идут, даже голодные.
    Lone empty;
    entt::registry& emptyRegistry = empty.world.registry();
    for (const auto [entity, supplies] : emptyRegistry.view<lw::Inventory>().each()) {
        supplies.set(lw::ItemKind::Food, 0);
    }
    for (const lw::Cell spot : forageSpots(empty.world)) {
        while (empty.world.resources().takeForest(spot)) {
        }
    }
    emptyRegistry.get<lw::Needs>(empty.person).hunger = 0.9f;
    CHECK(lw::evaluateActions(empty.world, empty.person)[static_cast<std::size_t>(
              lw::Action::Forage)] == 0.0f);
    bool wentToForest = false;
    for (int minute = 0; minute < 6 * 60; minute += 10) {
        empty.run(10);
        wentToForest = wentToForest ||
                       emptyRegistry.get<lw::Mind>(empty.person).action == lw::Action::Forage;
    }
    CHECK(!wentToForest);
}

void testChronicle() {
    lw::Chronicle chronicle;
    CHECK(chronicle.eventsOf(5).empty());
    const std::uint64_t empty = chronicle.hash();
    chronicle.add({10, lw::ChronicleKind::Settled, 5, 100, 0, 0});
    chronicle.add({20, lw::ChronicleKind::Scavenged, 6, 100, 2, 1});
    chronicle.add({30, lw::ChronicleKind::Starving, 5, lw::kInvalidId, 0, 0});
    chronicle.add({40, lw::ChronicleKind::Settled, 7, 7, 0, 0}); // объект — он сам
    CHECK(chronicle.events().size() == 4);
    CHECK(chronicle.eventsOf(5).size() == 2);
    CHECK(chronicle.eventsOf(5)[0] == 0 && chronicle.eventsOf(5)[1] == 2);
    CHECK(chronicle.eventsOf(100).size() == 2);
    CHECK(chronicle.eventsOf(7).size() == 1);
    CHECK(chronicle.eventsOf(lw::kInvalidId).empty());
    CHECK(chronicle.countOf(lw::ChronicleKind::Settled) == 2);
    CHECK(chronicle.countOf(lw::ChronicleKind::Dying) == 0);
    CHECK(chronicle.hash() != empty);

    // Тот же набор в другом порядке — другой хеш: порядок событий — часть истории.
    lw::Chronicle reordered;
    reordered.add({20, lw::ChronicleKind::Scavenged, 6, 100, 2, 1});
    reordered.add({10, lw::ChronicleKind::Settled, 5, 100, 0, 0});
    reordered.add({30, lw::ChronicleKind::Starving, 5, lw::kInvalidId, 0, 0});
    reordered.add({40, lw::ChronicleKind::Settled, 7, 7, 0, 0});
    CHECK(reordered.hash() != chronicle.hash());
}

// Системы пишут в летопись: появление, последняя еда дома, голод.
void testChronicleRecords() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    const lw::EntityId id = registry.get<lw::Id>(lone.person).value;
    const lw::Chronicle& chronicle = lone.world.chronicle();
    const auto has = [&](lw::ChronicleKind kind, lw::EntityId object) {
        for (const std::uint32_t index : chronicle.eventsOf(id)) {
            const lw::ChronicleEvent& event = chronicle.events()[index];
            if (event.kind == kind && event.subject == id && event.object == object) {
                return true;
            }
        }
        return false;
    };
    const lw::EntityId homeId = registry.get<lw::Person>(lone.person).home;
    CHECK(has(lw::ChronicleKind::Settled, homeId));
    CHECK(chronicle.countOf(lw::ChronicleKind::Settled) == 1);
    if (lone.home == entt::null) {
        return;
    }

    // Последняя порция дома.
    registry.get<lw::Inventory>(lone.home).set(lw::ItemKind::Food, 1);
    registry.get<lw::Needs>(lone.person).hunger = 0.9f;
    lone.run(90);
    CHECK(registry.get<lw::Inventory>(lone.home).food() == 0);
    CHECK(has(lw::ChronicleKind::AteLastFood, homeId));
    CHECK(chronicle.eventsOf(homeId).size() >= 2); // и дом помнит

    // Голод доходит до предела — одно событие, а не по событию каждую минуту.
    for (const auto [entity, supplies] : registry.view<lw::Inventory>().each()) {
        supplies.set(lw::ItemKind::Food, 0);
    }
    registry.get<lw::Inventory>(lone.person) = {};
    registry.get<lw::Needs>(lone.person).hunger = 0.995f;
    lone.run(30);
    CHECK(chronicle.countOf(lw::ChronicleKind::Starving) == 1);
}

void testAiSurvives() {
    lw::WorldConfig config = regionConfig();
    config.people = 120;
    lw::World world(7, config);
    const entt::registry& registry = world.registry();
    std::size_t asleepAtNight = 0;
    const std::uint64_t night = lw::nextTimeOfDay(world.tick(), 2, 0);
    const std::uint64_t end = world.tick() + 2 * lw::kTicksPerGameDay;
    while (world.tick() < end) {
        world.step();
        if (world.tick() == night) {
            for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
                asleepAtNight += person.asleep ? 1 : 0;
            }
        }
    }
    float worst = 1.0f;
    float hunger = 0.0f;
    float thirst = 0.0f;
    std::size_t actions = 0;
    bool kinds[static_cast<std::size_t>(lw::Action::Count)] = {};
    // Здоровье — у тех, на кого не нападали: без мирной жизни родня мстит
    // вору за своих (этап 7.3), и драки бывают уже в первые сутки.
    std::vector<lw::EntityId> assaulted;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind == lw::ChronicleKind::AssaultedBy) {
            assaulted.push_back(event.subject);
        }
    }
    for (const auto [entity, id, needs, mind] :
         registry.view<const lw::Id, const lw::Needs, const lw::Mind>().each()) {
        if (std::find(assaulted.begin(), assaulted.end(), id.value) == assaulted.end()) {
            worst = std::min(worst, needs.health);
        }
        hunger = std::max(hunger, needs.hunger);
        thirst = std::max(thirst, needs.thirst);
        kinds[static_cast<std::size_t>(mind.action)] = true;
    }
    for (const bool kind : kinds) {
        actions += kind ? 1 : 0;
    }
    CHECK(asleepAtNight >= 110);
    CHECK(worst > 0.95f);
    CHECK(hunger < 0.9f && thirst < 0.9f);
    CHECK(actions >= 3); // люди заняты разным
}

// Индекс шире 64 корзин: карта занятости — несколько слов на строку.
void testSpatialIndexWide() {
    constexpr float kWidth = 4000.0f; // 125 корзин по x
    constexpr float kHeight = 300.0f;
    entt::registry registry;
    lw::SpatialIndex index(kWidth, kHeight);
    registry.on_destroy<lw::Position>().connect<&lw::SpatialIndex::onPositionDestroyed>(index);
    lw::Rng rng(5);
    lw::EntityId nextId = 1;
    std::vector<entt::entity> entities;
    for (int i = 0; i < 400; ++i) {
        entities.push_back(spawnAt(registry, nextId++, rng.range(-20.0f, kWidth + 20.0f),
                                   rng.range(0.0f, kHeight)));
    }
    index.sync(registry);
    // Часть сущностей удалена: опустевшие корзины не должны мешать.
    for (std::size_t i = 0; i < 150; ++i) {
        registry.destroy(entities[i * 2]);
    }
    for (int query = 0; query < 200; ++query) {
        const float x = rng.range(-50.0f, kWidth + 50.0f);
        const float y = rng.range(0.0f, kHeight);
        const float radius = rng.range(0.0f, 2500.0f);
        std::vector<lw::EntityId> found;
        index.forEachInRadius(x, y, radius,
                              [&](const lw::SpatialHit& hit) { found.push_back(hit.id); });
        std::vector<lw::EntityId> expected;
        for (const auto [entity, id, position] :
             registry.view<const lw::Id, const lw::Position>().each()) {
            const float dx = position.x - x;
            const float dy = position.y - y;
            if (dx * dx + dy * dy <= radius * radius) {
                expected.push_back(id.value);
            }
        }
        std::sort(found.begin(), found.end());
        std::sort(expected.begin(), expected.end());
        CHECK(found == expected);
    }
}

// Условия зрения и слуха: ночь, луна, туман, дождь и ветер.
void testSenseConditions() {
    const lw::Weather calm;
    const lw::SenseConditions day = lw::senseConditions(1.0f, calm);
    CHECK(day.sight == 1.0f && day.hearing == 1.0f);
    CHECK(lw::senseConditions(0.3f, calm).sight == 1.0f); // пасмурный день — как ясный
    const float dark = lw::senseConditions(0.015f, calm).sight;
    const float moon = lw::senseConditions(0.11f, calm).sight;
    CHECK(dark >= 0.08f && dark < 0.15f);
    CHECK(moon > 0.4f && moon < 0.55f);
    CHECK(lw::senseConditions(0.0f, calm).sight == 0.08f);

    lw::Weather fog;
    fog.fog = 1.0f;
    CHECK(std::abs(lw::senseConditions(1.0f, fog).sight - 0.2f) < 1e-5f);
    lw::Weather storm;
    storm.precipitation = 1.0f;
    storm.windSpeed = 15.0f;
    const lw::SenseConditions stormy = lw::senseConditions(1.0f, storm);
    CHECK(stormy.sight < 0.75f && stormy.hearing < 0.55f && stormy.hearing > 0.4f);

    const lw::Senses senses;
    CHECK(lw::sightRange(senses, false, day) == senses.sight);
    CHECK(lw::sightRange(senses, true, day) == 0.0f);
    CHECK(lw::hearingFactor(senses, true, day) < lw::hearingFactor(senses, false, day));
}

// Луч зрения: стены закрывают, лес съедает дальность, вода — нет.
void testSightCost() {
    lw::Map map(40, 12);
    // Ряд y = 2: лес в клетках 10..14 (5 клеток).
    for (int x = 10; x < 15; ++x) {
        map.cover.at(x, 2) = lw::Cover::Forest;
    }
    // Ряд y = 6: озеро в клетках 10..19.
    for (int x = 10; x < 20; ++x) {
        map.water.at(x, 6) = lw::Water::Lake;
        map.depth.at(x, 6) = 20;
    }
    entt::registry registry;
    // Дом 2 × 3 клетки: x = 10..11, y = 9..11.
    const entt::entity house = registry.create();
    registry.emplace<lw::Building>(house, lw::BuildingKind::House, lw::Cell{10, 9}, 2, 3,
                                   lw::Cell{12, 10}, lw::kInvalidId);
    lw::NavGrid nav;
    nav.build(map, registry);
    const auto at = [](int cell) { return lw::Map::cellCenter(cell); };

    // Открытое место: цена — расстояние.
    CHECK(std::abs(lw::sightCost(map, nav, at(2), at(0), at(30), at(0)) - 112.0f) < 1e-3f);
    // Через лес: плюс штраф за 5 клеток.
    CHECK(std::abs(lw::sightCost(map, nav, at(2), at(2), at(30), at(2)) -
                   (112.0f + 5.0f * lw::kForestSightPenalty)) < 1e-3f);
    // Клетки наблюдателя и цели не считаются: из леса в соседнюю клетку леса — без штрафа.
    CHECK(std::abs(lw::sightCost(map, nav, at(10), at(2), at(11), at(2)) - 4.0f) < 1e-3f);
    // Через озеро видно.
    CHECK(std::abs(lw::sightCost(map, nav, at(5), at(6), at(25), at(6)) - 80.0f) < 1e-3f);
    // Через дом — нет; у стены — видно вдоль неё.
    CHECK(lw::sightCost(map, nav, at(5), at(10), at(25), at(10)) == lw::kNoSight);
    CHECK(lw::sightCost(map, nav, at(5), at(8), at(25), at(8)) < lw::kNoSight);
    // Дальше предела — kNoSight, даже на открытом месте.
    CHECK(lw::sightCost(map, nav, at(2), at(0), at(30), at(0), 100.0f) == lw::kNoSight);
    CHECK(lw::sightCost(map, nav, at(2), at(2), at(30), at(2), 150.0f) == lw::kNoSight);
}

// Мир из двух человек на открытом месте: A смотрит, B — цель. Проверяются
// правила восприятия на полном уровне детализации, поэтому LOD выключен:
// без зомби двое были бы спокойными и смотрели вчетверо реже (LOD — в своих
// тестах, этап 6а).
struct Pair {
    static constexpr int kRowLength = 30;

    lw::World world;
    entt::entity a = entt::null;
    entt::entity b = entt::null;
    lw::EntityId idA = lw::kInvalidId;
    lw::EntityId idB = lw::kInvalidId;
    lw::Cell row; // начало ряда клеток без леса, воды и зданий
    bool open = false;

    Pair() : world(7, [] {
                 lw::WorldConfig config = regionConfig();
                 config.people = 2;
                 config.lod = false;
                 return config;
             }()) {
        entt::registry& registry = world.registry();
        std::vector<entt::entity> people;
        for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
            people.push_back(entity);
        }
        if (people.size() != 2) {
            return;
        }
        a = people[0];
        b = people[1];
        idA = registry.get<lw::Id>(a).value;
        idB = registry.get<lw::Id>(b).value;
        const lw::Map& map = world.map();
        const lw::NavGrid& nav = world.nav();
        for (int y = 0; y < map.height() && !open; ++y) {
            int run = 0;
            for (int x = 0; x < map.width() && !open; ++x) {
                const bool clear = nav.passable({x, y}) && map.water.at(x, y) == lw::Water::None &&
                                   map.cover.at(x, y) == lw::Cover::None;
                run = clear ? run + 1 : 0;
                if (run == kRowLength) {
                    row = {x - kRowLength + 1, y};
                    open = true;
                }
            }
        }
        // Оба стоят и ничего не делают: шума нет.
        for (const entt::entity entity : {a, b}) {
            registry.get<lw::Velocity>(entity) = {};
            registry.get<lw::Mind>(entity).action = lw::Action::Idle;
            registry.get<lw::Person>(entity).asleep = false;
        }
        place(a, 0);
        place(b, 20);
    }

    // Поставить в клетку ряда со сдвигом offset клеток.
    void place(entt::entity entity, int offset) {
        world.registry().get<lw::Position>(entity) = {lw::Map::cellCenter(row.x + offset),
                                                      lw::Map::cellCenter(row.y)};
        world.registry().get<lw::Person>(entity).indoors = false; // на улице
        world.syncSpatial();
    }

    const lw::Contact* contact() const {
        return world.registry().get<lw::Awareness>(a).find(idB);
    }
};

void testPerception() {
    Pair pair;
    CHECK(pair.open);
    if (!pair.open) {
        return;
    }
    entt::registry& registry = pair.world.registry();
    const lw::SenseConditions day{1.0f, 1.0f};
    const lw::SenseConditions night{0.1f, 1.0f}; // 15 м

    // Днём стоящего в 80 м видно, шагов не слышно.
    lw::perceive(pair.world, pair.a, day);
    const lw::Contact* contact = pair.contact();
    CHECK(contact != nullptr && contact->seen && contact->heard == lw::SoundKind::None);
    CHECK(contact != nullptr && contact->tick == pair.world.tick());

    // Ночью не видно; идущего слышно только вблизи.
    registry.get<lw::Awareness>(pair.a) = {};
    lw::perceive(pair.world, pair.a, night);
    CHECK(pair.contact() == nullptr);
    registry.get<lw::Velocity>(pair.b) = {1.4f, 0.0f};
    pair.place(pair.b, 2); // 8 м
    lw::perceive(pair.world, pair.a, night);
    contact = pair.contact();
    CHECK(contact != nullptr && contact->seen && contact->heard == lw::SoundKind::Footsteps);

    // Спящий не видит и слышит шаги только совсем рядом.
    registry.get<lw::Person>(pair.a).asleep = true;
    registry.get<lw::Awareness>(pair.a) = {};
    lw::perceive(pair.world, pair.a, day);
    CHECK(pair.contact() == nullptr);
    pair.place(pair.b, 0); // в той же клетке
    lw::perceive(pair.world, pair.a, day);
    contact = pair.contact();
    CHECK(contact != nullptr && !contact->seen && contact->heard == lw::SoundKind::Footsteps);
    registry.get<lw::Person>(pair.a).asleep = false;

    // Обыск слышно дальше шагов.
    registry.get<lw::Velocity>(pair.b) = {};
    registry.get<lw::Mind>(pair.b).action = lw::Action::Scavenge;
    registry.get<lw::Mind>(pair.b).phase = lw::ActionPhase::Perform;
    pair.place(pair.b, 7); // 28 м
    registry.get<lw::Awareness>(pair.a) = {};
    lw::perceive(pair.world, pair.a, night);
    contact = pair.contact();
    CHECK(contact != nullptr && !contact->seen && contact->heard == lw::SoundKind::Rummage);
    registry.get<lw::Mind>(pair.b).action = lw::Action::Idle;

    // Разовый звук: слышен в пределах громкости, контакт — там, где прозвучал;
    // звук без живого источника (или чужой издалека) контакта не даёт.
    pair.place(pair.b, 25); // 100 м
    registry.get<lw::Awareness>(pair.a) = {};
    const float ax = registry.get<lw::Position>(pair.a).x;
    const float ay = registry.get<lw::Position>(pair.a).y;
    pair.world.emitSound(ax + 50.0f, ay, 60.0f, lw::SoundKind::Voice, pair.idB);
    pair.world.emitSound(ax + 80.0f, ay, 60.0f, lw::SoundKind::Voice, 999999);
    pair.world.emitSound(ax + 10.0f, ay, 60.0f, lw::SoundKind::Voice);
    lw::perceive(pair.world, pair.a, night);
    contact = pair.contact();
    CHECK(contact != nullptr && !contact->seen && contact->heard == lw::SoundKind::Voice);
    CHECK(contact != nullptr && contact->x == ax + 50.0f);
    CHECK(registry.get<lw::Awareness>(pair.a).find(999999) == nullptr);
    CHECK(registry.get<lw::Awareness>(pair.a).count == 1);
}

// В мире: за kPerceptionIntervalTicks восприятие обновляется у всех, звуки
// забываются, потерянный контакт помнится kContactMemoryTicks.
void testPerceptionInWorld() {
    Pair pair;
    if (!pair.open) {
        return;
    }
    entt::registry& registry = pair.world.registry();
    pair.place(pair.b, 3);
    // Пусть стоят, пока смотрим: решения — не раньше чем через час.
    for (const entt::entity entity : {pair.a, pair.b}) {
        lw::Mind& mind = registry.get<lw::Mind>(entity);
        mind.phase = lw::ActionPhase::Perform;
        mind.nextThink = pair.world.tick() + lw::kTicksPerGameHour;
        mind.until = mind.nextThink;
    }
    pair.world.emitSound(0.0f, 0.0f, 10.0f, lw::SoundKind::Voice, pair.idB);
    for (std::uint64_t i = 0; i < lw::kPerceptionIntervalTicks; ++i) {
        pair.world.step();
    }
    const lw::Contact* seen = pair.contact();
    const lw::Contact* back = registry.get<lw::Awareness>(pair.b).find(pair.idA);
    CHECK(seen != nullptr && seen->seen &&
          lw::isFresh(*seen, registry.get<lw::Awareness>(pair.a), pair.world.tick()));
    CHECK(back != nullptr && back->seen);
    for (std::uint64_t i = 0; i < lw::kSoundLifetimeTicks; ++i) {
        pair.world.step();
    }
    CHECK(pair.world.sounds().empty());

    // Унести B за пределы зрения и слуха: контакт помнится, потом забывается.
    const lw::Position where = registry.get<lw::Position>(pair.b);
    registry.get<lw::Position>(pair.b) = {where.x + 2000.0f, where.y};
    pair.world.syncSpatial();
    for (std::uint64_t i = 0; i < lw::kPerceptionIntervalTicks; ++i) {
        pair.world.step();
    }
    seen = pair.contact();
    CHECK(seen != nullptr && seen->x == where.x &&
          !lw::isFresh(*seen, registry.get<lw::Awareness>(pair.a), pair.world.tick()));
    for (std::uint64_t i = 0; i < lw::kContactMemoryTicks; ++i) {
        pair.world.step();
    }
    CHECK(pair.contact() == nullptr);
}


// --- Зомби (этап 5.2) ------------------------------------------------------

// Мир 4 × 4 км с людьми, ряд открытых клеток для опытов и ясная погода: днём
// на ряду видно на всю дальность.
struct Field {
    static constexpr int kRowLength = 30;

    lw::World world;
    lw::Cell row;
    bool open = false;
    lw::Rng rng{99};
    std::vector<entt::entity> people;

    // Поле проверяет правила поведения тик в тик («решил — в этом же тике»,
    // «услышал — через секунду»), поэтому на полном уровне детализации: LOD
    // выключен (он — в своих тестах, этап 6а; полный уровень — тот же мир,
    // что без LOD).
    explicit Field(int peopleCount) : world(7, [peopleCount] {
                                          lw::WorldConfig config = regionConfig();
                                          config.people = peopleCount;
                                          config.lod = false;
                                          return config;
                                      }()) {
        const lw::Map& map = world.map();
        const lw::NavGrid& nav = world.nav();
        for (int y = 0; y < map.height() && !open; ++y) {
            int run = 0;
            for (int x = 0; x < map.width() && !open; ++x) {
                const bool clear = nav.passable({x, y}) && map.water.at(x, y) == lw::Water::None &&
                                   map.cover.at(x, y) == lw::Cover::None;
                run = clear ? run + 1 : 0;
                if (run == kRowLength) {
                    row = {x - kRowLength + 1, y};
                    open = true;
                }
            }
        }
        lw::Weather& weather = world.weather();
        weather.kind = lw::WeatherKind::Clear;
        weather.minutesLeft = 100000;
        weather.cloudCover = 0.0f;
        weather.precipitation = 0.0f;
        weather.fog = 0.0f;
        for (const auto [entity, person] : world.registry().view<const lw::Person>().each()) {
            people.push_back(entity);
        }
        std::sort(people.begin(), people.end(), [this](entt::entity a, entt::entity b) {
            return id(a) < id(b);
        });
        // Люди поля — неродственные взрослые без прошлого: семьи (этап 7.3) —
        // в своих тестах.
        entt::registry& registry = world.registry();
        for (const entt::entity person : people) {
            registry.get<lw::Age>(person).years = 30;
            registry.get<lw::Kin>(person) = {};
            lw::Walker& walker = registry.get<lw::Walker>(person);
            walker.speed = std::max(walker.speed, 1.2f);
            lw::Relations& relations = registry.get<lw::Relations>(person);
            std::size_t kept = 0;
            for (std::size_t i = 0; i < relations.count; ++i) {
                const lw::BondOrigin origin = relations.bonds[i].origin;
                if (origin == lw::BondOrigin::Met || origin == lw::BondOrigin::Housemate ||
                    origin == lw::BondOrigin::Neighbour) {
                    relations.bonds[kept++] = relations.bonds[i];
                }
            }
            for (std::size_t i = kept; i < relations.count; ++i) {
                relations.bonds[i] = {};
            }
            relations.count = static_cast<std::uint8_t>(kept);
        }
    }

    lw::EntityId id(entt::entity entity) const {
        return world.registry().get<lw::Id>(entity).value;
    }
    lw::Cell at(int offset) const { return {row.x + offset, row.y}; }

    entt::entity zombie(int offset) {
        const entt::entity entity = lw::createZombie(world, at(offset), rng);
        world.syncSpatial();
        return entity;
    }

    // Поставить в клетку ряда со сдвигом offset клеток.
    void place(entt::entity entity, int offset, float dy = 0.0f) {
        world.registry().get<lw::Position>(entity) = {lw::Map::cellCenter(row.x + offset),
                                                      lw::Map::cellCenter(row.y) + dy};
        lw::Walker& walker = world.registry().get<lw::Walker>(entity);
        walker.waypoints.clear();
        walker.next = 0;
        if (auto* person = world.registry().try_get<lw::Person>(entity)) {
            person->indoors = false; // на улице
        }
        world.syncSpatial();
    }

    // Человек без решений: стоит, где поставили (Utility AI его не трогает).
    void freeze(entt::entity person, int offset) {
        world.registry().remove<lw::Mind>(person);
        world.registry().get<lw::Person>(person).asleep = false;
        place(person, offset);
    }

    // Убрать человека подальше от ряда, чтобы не мешал.
    void dismiss(entt::entity person) {
        world.registry().remove<lw::Mind>(person);
        const lw::Position& position = world.registry().get<lw::Position>(person);
        const float far = std::hypot(position.x - lw::Map::cellCenter(row.x),
                                     position.y - lw::Map::cellCenter(row.y));
        if (far < 500.0f) {
            world.registry().get<lw::Position>(person) = {
                lw::Map::cellCenter(row.x) + (row.x < 512 ? 1500.0f : -1500.0f),
                lw::Map::cellCenter(row.y)};
        }
        world.syncSpatial();
    }

    void steps(std::uint64_t count) {
        for (std::uint64_t i = 0; i < count; ++i) {
            world.step();
        }
    }

    float distance(entt::entity a, entt::entity b) const {
        const lw::Position& pa = world.registry().get<lw::Position>(a);
        const lw::Position& pb = world.registry().get<lw::Position>(b);
        return std::hypot(pa.x - pb.x, pa.y - pb.y);
    }

    std::size_t count(lw::ChronicleKind kind) const { return world.chronicle().countOf(kind); }
};

// Расселение: сколько просили, подальше от людей, четверть — в ордах; один
// seed — один мир и с зомби.
void testZombieSpawn() {
    lw::WorldConfig config = regionConfig();
    config.people = 40;
    config.zombies = 120;
    lw::World a(11, config);
    lw::World b(11, config);
    const entt::registry& registry = a.registry();
    CHECK(registry.view<const lw::Zombie>().size() == 120);
    CHECK(registry.view<const lw::Horde>().size() >= 1);

    std::size_t inHordes = 0;
    float closest = 1e9f;
    for (const auto [entity, zombie, position] :
         registry.view<const lw::Zombie, const lw::Position>().each()) {
        CHECK((registry.all_of<lw::Senses, lw::Awareness, lw::Walker, lw::Velocity>(entity)));
        if (zombie.horde != lw::kInvalidId) {
            ++inHordes;
            const entt::entity horde = a.find(zombie.horde);
            CHECK(horde != entt::null);
            if (horde != entt::null) {
                const auto& members = registry.get<lw::Horde>(horde).members;
                CHECK(std::binary_search(members.begin(), members.end(),
                                         registry.get<lw::Id>(entity).value));
            }
        }
        for (const auto [other, person, at] :
             registry.view<const lw::Person, const lw::Position>().each()) {
            closest = std::min(closest, std::hypot(at.x - position.x, at.y - position.y));
        }
    }
    CHECK(inHordes >= 120 / 4 - 12 && inHordes <= 120 / 4);
    CHECK(closest >= 150.0f - 4.0f); // в клетку от расчётного расстояния
    for (const auto [entity, horde] : registry.view<const lw::Horde>().each()) {
        CHECK(horde.members.size() >= 2);
        CHECK(std::is_sorted(horde.members.begin(), horde.members.end()));
        CHECK(horde.leader == horde.members.front());
    }
    CHECK(a.chronicle().countOf(lw::ChronicleKind::HordeFormed) ==
          registry.view<const lw::Horde>().size());

    CHECK(a.stateHash() == b.stateHash());
    for (int i = 0; i < 3000; ++i) {
        a.step();
        b.step();
    }
    CHECK(a.stateHash() == b.stateHash());
}

// Восприятие: зомби не замечает других зомби, человек ставит зомби вперёд.
void testZombiePerception() {
    Field field(2);
    CHECK(field.open && field.people.size() == 2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    const entt::entity a = field.people[0];
    const entt::entity b = field.people[1];
    field.freeze(a, 0);
    field.freeze(b, 2);
    const entt::entity z1 = field.zombie(12);
    const entt::entity z2 = field.zombie(13);
    const lw::SenseConditions day{1.0f, 1.0f};
    lw::perceive(field.world, a, day);
    lw::perceive(field.world, z1, day);

    const lw::Awareness& seen = field.world.registry().get<lw::Awareness>(a);
    CHECK(seen.count == 3);
    // Ближе всех — человек b, но первым идёт зомби.
    CHECK(seen.count > 0 && seen.contacts[0].id == field.id(z1));
    CHECK(seen.find(field.id(b)) != nullptr);
    const lw::Awareness& hunter = field.world.registry().get<lw::Awareness>(z1);
    CHECK(hunter.find(field.id(z2)) == nullptr);
    CHECK(hunter.find(field.id(a)) != nullptr && hunter.find(field.id(a))->seen);

    // Угроза: видит зомби в 48 м — заметная, прочь от него (на запад по ряду).
    const lw::Threat threat = lw::threatOf(field.world, a);
    CHECK(threat.count == 2);
    CHECK(threat.level > 0.2f && threat.level < 0.5f);
    CHECK(threat.awayX < -0.9f);
    CHECK(threat.nearest == field.id(z1));
}

// Погоня: увидел — преследует, созывает стоном, догоняет и держится рядом.
// Стон слышит другой зомби, который добычу не видит, и идёт на него.
void testZombieChase() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.freeze(person, 0);
    const entt::entity hunter = field.zombie(15); // 60 м
    const entt::entity listener = field.zombie(29); // 116 м от человека, 56 м от первого
    const entt::registry& registry = field.world.registry();

    // Стон звучит kSoundLifetimeTicks: проверять сразу в тик, когда погнался.
    for (std::uint64_t i = 0; i < lw::kZombieThinkTicks &&
                              registry.get<lw::Zombie>(hunter).state != lw::ZombieState::Chase;
         ++i) {
        field.steps(1);
    }
    const lw::Zombie& zombie = registry.get<lw::Zombie>(hunter);
    CHECK(zombie.state == lw::ZombieState::Chase);
    CHECK(zombie.target == field.id(person));
    CHECK(registry.get<lw::Walker>(hunter).moving());
    const bool called = std::any_of(
        field.world.sounds().begin(), field.world.sounds().end(), [&](const lw::Sound& sound) {
            return sound.kind == lw::SoundKind::Groan && sound.source == field.id(hunter);
        });
    CHECK(called);

    field.steps(lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Zombie>(listener).state == lw::ZombieState::Investigate);

    // 60 м на 1,3–1,7 м/с — меньше 50 с.
    field.steps(50 * lw::kTicksPerSecond);
    CHECK(field.distance(hunter, person) <= 2.0f);
    CHECK(registry.get<lw::Zombie>(hunter).state == lw::ZombieState::Chase);
}

// Шум: слышит — идёт туда, стоит и успокаивается; повторно то же место не проверяет.
void testZombieInvestigates() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.dismiss(person);
    const entt::entity zombie = field.zombie(0);
    const entt::registry& registry = field.world.registry();
    const lw::Position start = registry.get<lw::Position>(zombie);
    const float noiseX = lw::Map::cellCenter(field.at(10).x);
    const float noiseY = lw::Map::cellCenter(field.at(10).y);
    // Источник далеко и не виден: слышен только сам звук.
    field.world.emitSound(noiseX, noiseY, 60.0f, lw::SoundKind::Voice, field.id(person));
    field.steps(lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Zombie>(zombie).state == lw::ZombieState::Investigate);
    CHECK(registry.get<lw::Zombie>(zombie).alerted > 0);
    CHECK(std::abs(registry.get<lw::Walker>(zombie).speed * registry.get<lw::Walker>(zombie).pace -
                   lw::kInvestigateSpeed) < 1e-3f);

    // 40 м на 0,9 м/с — 45 с, потом стоит до 40 с.
    bool arrived = false;
    for (int second = 0; second < 100; ++second) {
        field.steps(lw::kTicksPerSecond);
        const lw::Position& at = registry.get<lw::Position>(zombie);
        arrived = arrived || std::hypot(at.x - noiseX, at.y - noiseY) <= 3.0f;
    }
    CHECK(arrived);
    const lw::Zombie& calm = registry.get<lw::Zombie>(zombie);
    CHECK(calm.state != lw::ZombieState::Investigate && calm.state != lw::ZombieState::Chase);
    CHECK(calm.checked > 0);
    (void)start;
}

// Человек бежит от зомби бегом, будит его угроза; запоминает опасное место
// и потом, когда зомби не видно, просто отходит оттуда шагом.
void testPersonFlees() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    entt::registry& registry = field.world.registry();
    field.place(person, 20);
    // Безоружный и не храбрец: бежит, а не дерётся.
    registry.get<lw::Weapon>(person) = {};
    registry.get<lw::Traits>(person) = {};
    lw::Mind& mind = registry.get<lw::Mind>(person);
    mind.action = lw::Action::Sleep;
    mind.phase = lw::ActionPhase::Perform;
    mind.nextThink = field.world.tick() + lw::kTicksPerGameHour;
    mind.until = ~std::uint64_t{0};
    registry.get<lw::Person>(person).asleep = true;
    // Спящий не видит, но зомби в 4 м видит его, гонится и стонет — это
    // слышно и во сне.
    const entt::entity zombie = field.zombie(19);
    field.steps(2 * lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Mind>(person).action == lw::Action::Flee);
    CHECK(!registry.get<lw::Person>(person).asleep);
    CHECK(std::abs(registry.get<lw::Walker>(person).pace - 1.8f) < 1e-3f);
    CHECK(field.count(lw::ChronicleKind::Fled) == 1);
    CHECK(std::any_of(std::begin(registry.get<lw::Mind>(person).dangers),
                      std::end(registry.get<lw::Mind>(person).dangers),
                      [&](const lw::Danger& danger) { return danger.until > field.world.tick(); }));
    const auto& events = field.world.chronicle().events();
    const auto fled =
        std::find_if(events.begin(), events.end(), [](const lw::ChronicleEvent& event) {
            return event.kind == lw::ChronicleKind::Fled;
        });
    CHECK(fled != events.end() && fled->object == field.id(zombie) && fled->a == 1);

    // Бегом быстрее погони: отрыв растёт хотя бы на 0,46 м/с.
    field.steps(20 * lw::kTicksPerSecond);
    CHECK(field.distance(person, zombie) > 8.0f);

    // Зомби нет, а место помнится: стоя там, человек хочет уйти, и идёт шагом.
    field.world.destroy(zombie);
    field.place(person, 15);
    lw::Mind& wary = registry.get<lw::Mind>(person);
    for (lw::Danger& danger : wary.dangers) {
        danger = {};
    }
    wary.dangers[0] = {field.at(16), field.world.tick() + lw::kTicksPerGameHour};
    wary.action = lw::Action::Idle;
    wary.phase = lw::ActionPhase::Perform;
    wary.until = field.world.tick();
    lw::Needs& needs = registry.get<lw::Needs>(person);
    needs = {};
    const lw::ActionScores scores = lw::evaluateActions(field.world, person);
    CHECK(scores[static_cast<std::size_t>(lw::Action::Flee)] > 0.2f);
    field.steps(1);
    CHECK(registry.get<lw::Mind>(person).action == lw::Action::Flee);
    CHECK(registry.get<lw::Walker>(person).pace == 1.0f);
    CHECK(field.count(lw::ChronicleKind::Fled) == 1); // отход — не бегство
}

// Орда держится вожака; одиночки, сбежавшиеся на шум, сбиваются в орду;
// близкие орды сливаются; одиночку у орды она подбирает; из одного — распадается.
void testHordes() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    field.dismiss(field.people[0]);
    entt::registry& registry = field.world.registry();

    // Сбор на шум.
    // Пусть стоят, пока не соберутся: иначе разбредутся.
    const auto still = [&](entt::entity zombie) {
        registry.get<lw::Zombie>(zombie).until = field.world.tick() + 100000;
        return zombie;
    };
    std::vector<lw::EntityId> gathered;
    for (int i = 0; i < 6; ++i) {
        const entt::entity zombie = still(field.zombie(i));
        registry.get<lw::Zombie>(zombie).alerted = field.world.tick() + 1;
        gathered.push_back(field.id(zombie));
    }
    field.steps(lw::kHordeUpdateTicks);
    CHECK(registry.view<const lw::Horde>().size() == 1);
    CHECK(field.count(lw::ChronicleKind::HordeFormed) == 1);
    lw::EntityId first = lw::kInvalidId;
    for (const auto [entity, id, horde] : registry.view<const lw::Id, const lw::Horde>().each()) {
        first = id.value;
        CHECK(horde.members == gathered);
        CHECK(horde.leader == gathered.front());
    }
    CHECK(field.world.chronicle().events().back().b == 1);

    // Вторая орда рядом — сливаются в одну.
    std::vector<lw::EntityId> second;
    for (int i = 0; i < 3; ++i) {
        second.push_back(field.id(still(field.zombie(9 + i))));
    }
    lw::formHorde(field.world, second, lw::HordeOrigin::Start);
    field.steps(lw::kHordeUpdateTicks);
    CHECK(registry.view<const lw::Horde>().size() == 1);
    CHECK(field.count(lw::ChronicleKind::HordeMerged) == 1);
    const entt::entity horde = field.world.find(first);
    CHECK(horde != entt::null);
    if (horde == entt::null) {
        return;
    }
    CHECK(registry.get<lw::Horde>(horde).members.size() == 9);

    // Одиночка у орды пристаёт к ней (вероятность 0,3 за обновление).
    const entt::entity loner = still(field.zombie(5));
    for (int i = 0; i < 30 && registry.get<lw::Zombie>(loner).horde == lw::kInvalidId; ++i) {
        field.steps(lw::kHordeUpdateTicks);
    }
    CHECK(registry.get<lw::Zombie>(loner).horde == first);

    // Две минуты: все держатся вожака.
    field.steps(120 * lw::kTicksPerSecond);
    const lw::Horde& info = registry.get<lw::Horde>(horde);
    const entt::entity leader = field.world.find(info.leader);
    for (const lw::EntityId member : info.members) {
        CHECK(field.distance(field.world.find(member), leader) < 30.0f);
        CHECK(registry.get<lw::Zombie>(field.world.find(member)).state ==
              (member == info.leader ? registry.get<lw::Zombie>(leader).state
                                     : lw::ZombieState::Follow));
    }

    // Остался один — орда распалась.
    const std::vector<lw::EntityId> members = info.members;
    for (std::size_t i = 1; i < members.size(); ++i) {
        field.world.destroy(field.world.find(members[i]));
    }
    field.steps(lw::kHordeUpdateTicks);
    CHECK(field.world.find(first) == entt::null);
    CHECK(field.count(lw::ChronicleKind::HordeScattered) == 1);
    CHECK(registry.get<lw::Zombie>(field.world.find(members[0])).horde == lw::kInvalidId);
}


// Дом — укрытие: зомби у двери не видит человека, а тот не выбегает.
void testShelter() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    entt::registry& registry = field.world.registry();
    CHECK(registry.get<lw::Person>(person).indoors); // утро застаёт дома
    lw::Mind& mind = registry.get<lw::Mind>(person);
    mind.nextThink = field.world.tick() + lw::kTicksPerGameHour;
    mind.until = mind.nextThink;
    const lw::Position& at = registry.get<lw::Position>(person);
    const auto near = field.world.nav().nearestPassable(
        lw::Map::cellAt(at.x + 3.0f * lw::Map::kCellSize, at.y), 2);
    CHECK(near.has_value());
    if (!near) {
        return;
    }
    const entt::entity zombie = lw::createZombie(field.world, *near, field.rng);
    field.world.syncSpatial();
    field.steps(2 * lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Zombie>(zombie).state != lw::ZombieState::Chase);
    CHECK(registry.get<lw::Awareness>(zombie).find(field.id(person)) == nullptr);
    // Сам он зомби видит, боится, но сидит дома.
    CHECK(registry.get<lw::Awareness>(person).find(field.id(zombie)) != nullptr);
    CHECK(registry.get<lw::Mind>(person).action != lw::Action::Flee);
    CHECK(registry.get<lw::Person>(person).indoors);
    CHECK(field.count(lw::ChronicleKind::Fled) == 0);
}


// --- Животные и птицы (этап 5.3) ---------------------------------------------

// Расселение: сколько просили, по видам, вдали от людей; один seed — один мир.
void testAnimalSpawn() {
    lw::WorldConfig config = regionConfig();
    config.people = 40;
    config.animals = 40;
    config.flocks = 6;
    lw::World a(11, config);
    lw::World b(11, config);
    const entt::registry& registry = a.registry();
    CHECK(registry.view<const lw::Animal>().size() == 40);
    CHECK(registry.view<const lw::Flock>().size() == 6);
    std::size_t kinds[3] = {};
    float closest = 1e9f;
    for (const auto [entity, animal, position] :
         registry.view<const lw::Animal, const lw::Position>().each()) {
        CHECK((registry.all_of<lw::Senses, lw::Awareness, lw::Walker, lw::Velocity>(entity)));
        ++kinds[static_cast<std::size_t>(animal.kind)];
        for (const auto [other, person, at] :
             registry.view<const lw::Person, const lw::Position>().each()) {
            closest = std::min(closest, std::hypot(at.x - position.x, at.y - position.y));
        }
    }
    CHECK(kinds[0] == 20 && kinds[1] == 10 && kinds[2] == 10);
    CHECK(closest >= 300.0f - 4.0f * lw::Map::kCellSize);

    CHECK(a.stateHash() == b.stateHash());
    for (int i = 0; i < 3000; ++i) {
        a.step();
        b.step();
    }
    CHECK(a.stateHash() == b.stateHash());
}

// Олень видит человека — убегает бегом и отрывается.
void testAnimalFlees() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.freeze(person, 0);
    const entt::entity deer = lw::createAnimal(field.world, lw::AnimalKind::Deer, field.at(12), field.rng);
    field.world.syncSpatial();
    const entt::registry& registry = field.world.registry();
    field.steps(2 * lw::kAnimalThinkTicks);
    const lw::Animal& animal = registry.get<lw::Animal>(deer);
    CHECK(animal.state == lw::AnimalState::Flee);
    CHECK(animal.threat == field.id(person));
    const lw::Walker& walker = registry.get<lw::Walker>(deer);
    CHECK(walker.moving());
    CHECK(std::abs(walker.speed * walker.pace - lw::animalSpec(lw::AnimalKind::Deer).runSpeed) < 1e-3f);
    field.steps(5 * lw::kTicksPerSecond);
    CHECK(field.distance(deer, person) > 70.0f);
}

// Зомби животных не замечают, а животные от зомби убегают.
void testZombieIgnoresAnimals() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    field.dismiss(field.people[0]);
    const entt::entity boar = lw::createAnimal(field.world, lw::AnimalKind::Boar, field.at(0), field.rng);
    const entt::entity zombie = field.zombie(5);
    const entt::registry& registry = field.world.registry();
    field.steps(2 * lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Awareness>(zombie).find(field.id(boar)) == nullptr);
    CHECK(registry.get<lw::Zombie>(zombie).state != lw::ZombieState::Chase);
    CHECK(registry.get<lw::Awareness>(boar).find(field.id(zombie)) != nullptr);
    CHECK(registry.get<lw::Animal>(boar).state == lw::AnimalState::Flee);
}

// Человек рядом — стая взлетает с криком; зомби, который человека не видит,
// идёт на крик. Стая улетает на сотни метров и садится.
void testBirdsScatter() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.freeze(person, 1);
    const entt::entity flock = lw::createFlock(field.world, field.at(0), 12, field.rng);
    const entt::entity zombie = field.zombie(22); // 88 м: человека не видит, крик слышит
    const entt::registry& registry = field.world.registry();
    const lw::Position start = registry.get<lw::Position>(flock);
    for (std::uint64_t i = 0; i < lw::kAnimalThinkTicks && !registry.get<lw::Flock>(flock).flying; ++i) {
        field.steps(1);
    }
    CHECK(registry.get<lw::Flock>(flock).flying);
    const bool cried = std::any_of(
        field.world.sounds().begin(), field.world.sounds().end(), [&](const lw::Sound& sound) {
            return sound.kind == lw::SoundKind::Birds && sound.source == field.id(flock);
        });
    CHECK(cried);
    field.steps(lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Awareness>(zombie).find(field.id(person)) == nullptr ||
          !registry.get<lw::Awareness>(zombie).find(field.id(person))->seen);
    CHECK(registry.get<lw::Zombie>(zombie).state == lw::ZombieState::Investigate);

    for (int second = 0; second < 90 && registry.get<lw::Flock>(flock).flying; ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(!registry.get<lw::Flock>(flock).flying);
    const lw::Position& end = registry.get<lw::Position>(flock);
    CHECK(std::hypot(end.x - start.x, end.y - start.y) >= 290.0f);
}


// --- Бой (этап 5.4) ---------------------------------------------------------

// Поставить существо в точку ряда со сдвигом в метрах.
void putAt(Field& field, entt::entity entity, float meters) {
    field.place(entity, 0);
    field.world.registry().get<lw::Position>(entity).x += meters;
    field.world.syncSpatial();
}

std::size_t countBodies(const lw::World& world, lw::CorpseKind kind) {
    std::size_t count = 0;
    for (const auto [entity, corpse] : world.registry().view<const lw::Corpse>().each()) {
        count += corpse.kind == kind ? 1 : 0;
    }
    return count;
}

// Топор против зомби вплотную: зомби упокоен и стал телом с тем же id.
void testMeleeKillsZombie() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.freeze(person, 0);
    entt::registry& registry = field.world.registry();
    registry.get<lw::Weapon>(person) = {lw::WeaponKind::Axe, 0};
    const entt::entity zombie = field.zombie(0);
    putAt(field, zombie, 1.0f);
    const lw::EntityId zombieId = field.id(zombie);
    registry.get<lw::Combat>(person).target = zombieId;
    for (int second = 0; second < 30 && !registry.all_of<lw::Corpse>(zombie); ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(field.world.find(zombieId) == zombie);
    CHECK(registry.all_of<lw::Corpse>(zombie));
    CHECK((!registry.any_of<lw::Zombie, lw::Walker, lw::Senses, lw::Awareness, lw::Combat>(zombie)));
    CHECK(registry.all_of<lw::Position>(zombie));
    if (const auto* corpse = registry.try_get<lw::Corpse>(zombie)) {
        CHECK(corpse->kind == lw::CorpseKind::Zombie);
        CHECK(corpse->killer == field.id(person));
    }
    CHECK(field.count(lw::ChronicleKind::KilledZombie) == 1);
    const auto& events = field.world.chronicle().events();
    const auto killed = std::find_if(events.begin(), events.end(), [](const lw::ChronicleEvent& e) {
        return e.kind == lw::ChronicleKind::KilledZombie;
    });
    CHECK(killed != events.end() && killed->a == static_cast<int>(lw::WeaponKind::Axe));
    // Тело не мешает живым: человек больше никого не бьёт.
    field.steps(lw::kTicksPerSecond);
    CHECK(registry.get<lw::Combat>(person).target == lw::kInvalidId);
}

// Выстрел тратит патрон, слышен далеко: зомби, который стрелка не видит, идёт на звук.
void testGunshotIsHeard() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.freeze(person, 0);
    entt::registry& registry = field.world.registry();
    registry.get<lw::Weapon>(person) = {lw::WeaponKind::Rifle, 10};
    const entt::entity target = field.zombie(20); // 80 м: человека не видит
    const lw::EntityId targetId = field.id(target);
    // Слушатель — в конце ряда, в 116 м: человека не видит, выстрел винтовки слышен в 450 м.
    const entt::entity listener = field.zombie(29);
    registry.get<lw::Combat>(person).target = targetId;
    field.steps(1);
    CHECK(registry.get<lw::Weapon>(person).ammo == 9);
    const bool shot = std::any_of(field.world.sounds().begin(), field.world.sounds().end(),
                                  [&](const lw::Sound& sound) {
                                      return sound.kind == lw::SoundKind::Gunshot &&
                                             sound.source == field.id(person);
                                  });
    CHECK(shot);
    field.steps(lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Zombie>(listener).state == lw::ZombieState::Investigate);
    for (int second = 0; second < 20 && !registry.all_of<lw::Corpse>(target); ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(registry.all_of<lw::Corpse>(target));
    CHECK(registry.get<lw::Weapon>(person).ammo < 9);
}

// Зомби бьёт того, кого догнал: раны, кровотечение, крик; без защиты человек гибнет
// и становится телом.
void testZombieKillsPerson() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    const lw::EntityId personId = field.id(person);
    field.freeze(person, 0);
    entt::registry& registry = field.world.registry();
    const entt::entity zombie = field.zombie(0);
    putAt(field, zombie, 1.0f);
    bool screamed = false;
    for (int tick = 0; tick < 20 * lw::kTicksPerSecond; ++tick) {
        field.steps(1);
        screamed = screamed || std::any_of(field.world.sounds().begin(), field.world.sounds().end(),
                                           [&](const lw::Sound& sound) {
                                               return sound.kind == lw::SoundKind::Scream &&
                                                      sound.source == personId;
                                           });
    }
    CHECK(screamed);
    CHECK(registry.all_of<lw::Corpse>(person) || registry.get<lw::Needs>(person).health < 0.9f);
    if (registry.all_of<lw::Wounds>(person)) {
        CHECK(registry.get<lw::Wounds>(person).hurtBy == field.id(zombie));
    }
    for (int second = 0; second < 300 && !registry.all_of<lw::Corpse>(person); ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(registry.all_of<lw::Corpse>(person));
    CHECK((!registry.any_of<lw::Person, lw::Mind, lw::Needs, lw::Walker>(person)));
    CHECK((registry.all_of<lw::Weapon, lw::Traits>(person))); // при теле
    CHECK(field.world.find(personId) == person);
    CHECK(field.count(lw::ChronicleKind::Died) == 1);
    CHECK(countBodies(field.world, lw::CorpseKind::Human) == 1);
    // Тело зомби больше не интересно: он не бьёт и не гонится.
    field.steps(2 * lw::kZombieThinkTicks);
    CHECK(registry.get<lw::Combat>(zombie).target == lw::kInvalidId);
    CHECK(registry.get<lw::Zombie>(zombie).state != lw::ZombieState::Chase);
}

// Смелый с топором не бежит от одного зомби, а дерётся и побеждает.
void testPersonFightsZombie() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    entt::registry& registry = field.world.registry();
    field.place(person, 0);
    registry.get<lw::Weapon>(person) = {lw::WeaponKind::Axe, 0};
    lw::Traits& traits = registry.get<lw::Traits>(person);
    traits.bravery = 1.0f;
    traits.aggression = 1.0f;
    registry.get<lw::Needs>(person) = {};
    lw::Mind& mind = registry.get<lw::Mind>(person);
    mind.action = lw::Action::Idle;
    mind.phase = lw::ActionPhase::Perform;
    mind.until = field.world.tick() + lw::kTicksPerGameHour;
    mind.nextThink = mind.until;
    const entt::entity zombie = field.zombie(8); // 32 м
    for (int i = 0; i < 4 && registry.get<lw::Mind>(person).action != lw::Action::Fight; ++i) {
        field.steps(lw::kPerceptionIntervalTicks);
    }
    CHECK(registry.get<lw::Mind>(person).action == lw::Action::Fight);
    CHECK(registry.get<lw::Combat>(person).target == field.id(zombie));
    for (int second = 0; second < 90 && !registry.all_of<lw::Corpse>(zombie); ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(registry.all_of<lw::Corpse>(zombie));
    CHECK(field.count(lw::ChronicleKind::KilledZombie) == 1);
    // Угрозы нет — бой кончился, человек занят другим.
    field.steps(lw::kTicksPerSecond);
    if (registry.all_of<lw::Mind>(person)) {
        CHECK(registry.get<lw::Mind>(person).action != lw::Action::Fight);
        CHECK(registry.get<lw::Combat>(person).target == lw::kInvalidId);
    }
}

// Голодный с винтовкой, когда в лесу пусто, охотится: выслеживает оленя,
// стреляет, разделывает тушу и несёт мясо.
void testHunting() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    lw::World& world = field.world;
    entt::registry& registry = world.registry();
    // Лес пуст: собирать нечего.
    const lw::Places::SpotGrid& spots = world.places().forageSpots();
    for (int y = 0; y < spots.height(); ++y) {
        for (int x = 0; x < spots.width(); ++x) {
            if (const auto& spot = spots.at(x, y)) {
                while (world.resources().takeForest(*spot)) {
                }
            }
        }
    }
    const entt::entity hunter = field.people[0];
    field.place(hunter, 0);
    registry.get<lw::Weapon>(hunter) = {lw::WeaponKind::Rifle, 8};
    lw::Needs& needs = registry.get<lw::Needs>(hunter);
    needs = {};
    needs.hunger = 0.9f;
    registry.get<lw::Inventory>(hunter) = {};
    if (const entt::entity home = world.find(registry.get<lw::Person>(hunter).home); home != entt::null) {
        registry.get<lw::Inventory>(home).set(lw::ItemKind::Food, 0);
    }
    lw::Mind& mind = registry.get<lw::Mind>(hunter);
    mind.emptySearches = 255; // округа обобрана
    mind.action = lw::Action::Idle;
    mind.phase = lw::ActionPhase::Perform;
    mind.until = world.tick();
    const entt::entity deer = lw::createAnimal(world, lw::AnimalKind::Deer, field.at(15), field.rng);
    world.syncSpatial();
    const lw::ActionScores scores = lw::evaluateActions(world, hunter);
    CHECK(scores[static_cast<std::size_t>(lw::Action::Hunt)] > 0.5f);
    field.steps(1);
    CHECK(registry.get<lw::Mind>(hunter).action == lw::Action::Hunt);
    for (int minute = 0; minute < 180 && field.count(lw::ChronicleKind::Butchered) == 0; ++minute) {
        field.steps(lw::kTicksPerGameMinute);
    }
    CHECK(field.count(lw::ChronicleKind::Hunted) == 1);
    CHECK(field.count(lw::ChronicleKind::Butchered) == 1);
    CHECK(registry.all_of<lw::Corpse>(deer));
    if (const auto* carcass = registry.try_get<lw::Corpse>(deer)) {
        CHECK(carcass->kind == lw::CorpseKind::Animal && carcass->animal == lw::AnimalKind::Deer);
        CHECK(carcass->meat < 8);
    }
    if (registry.all_of<lw::Inventory>(hunter)) {
        CHECK(registry.get<lw::Inventory>(hunter).food() > 0);
        CHECK(registry.get<lw::Needs>(hunter).hunger < 0.9f);
        CHECK(registry.get<lw::Weapon>(hunter).ammo < 8);
    }
}


// --- Смерть, тела, заражение (этап 5.5) ---------------------------------------

// Укушенный сгорает от заражения и встаёт зомби той же сущностью; оружие при нём.
void testInfectionTurns() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    const lw::EntityId id = field.id(person);
    field.freeze(person, 0);
    entt::registry& registry = field.world.registry();
    registry.get<lw::Weapon>(person) = {lw::WeaponKind::Knife, 0};
    lw::Wounds& wounds = registry.get<lw::Wounds>(person);
    wounds.bites = 1;
    wounds.infection = 0.97f;
    for (int minute = 0; minute < 120 && !registry.all_of<lw::Corpse>(person); ++minute) {
        field.steps(lw::kTicksPerGameMinute);
    }
    CHECK(registry.all_of<lw::Corpse>(person));
    if (const auto* corpse = registry.try_get<lw::Corpse>(person)) {
        CHECK(corpse->cause == lw::DeathCause::Infection);
        CHECK(corpse->turnsAt >= corpse->diedAt + lw::kTurnMinMinutes * lw::kTicksPerGameMinute);
        CHECK(corpse->turnsAt <= corpse->diedAt + lw::kTurnMaxMinutes * lw::kTicksPerGameMinute);
    }
    for (int minute = 0; minute < 60 && !registry.all_of<lw::Zombie>(person); ++minute) {
        field.steps(lw::kTicksPerGameMinute);
    }
    CHECK(field.world.find(id) == person);
    CHECK((registry.all_of<lw::Zombie, lw::Walker, lw::Senses, lw::Awareness, lw::Combat>(person)));
    CHECK(!registry.all_of<lw::Corpse>(person));
    CHECK(registry.all_of<lw::Weapon>(person));
    CHECK(field.count(lw::ChronicleKind::Turned) == 1);
    CHECK(field.count(lw::ChronicleKind::Died) == 1);
    // Обратившийся бьёт как зомби, а не ножом.
    const lw::AttackSpec* spec = lw::attackOf(registry, person);
    CHECK(spec == &lw::zombieAttack());
    field.steps(100);
}

// --- Вспышка и заражение (этап 7.5) -------------------------------------------

// Заражение от больного: затравка, путь, от кого и когда, запись в летописи;
// течение дольше, чем после укуса; лихорадка с середины; погибший встаёт, хотя
// его никто не кусал, а убийцы у него нет.
void testContactInfectionCourse() {
    Field field(2);
    CHECK(field.open && field.people.size() >= 2);
    if (!field.open || field.people.size() < 2) {
        return;
    }
    const entt::entity sick = field.people[0];
    const entt::entity person = field.people[1];
    const lw::EntityId id = field.id(person);
    field.freeze(person, 0);
    field.dismiss(sick);
    entt::registry& registry = field.world.registry();

    CHECK(lw::infect(field.world, person, lw::InfectionRoute::Household, field.id(sick)));
    CHECK(!lw::infect(field.world, person, lw::InfectionRoute::Nearby, field.id(sick))); // уже
    lw::Wounds& wounds = registry.get<lw::Wounds>(person);
    CHECK(wounds.infection == lw::kInfectionSeed && wounds.route == lw::InfectionRoute::Household &&
          wounds.infectedBy == field.id(sick) && wounds.infectedAt == field.world.tick());
    CHECK(wounds.bites == 0 && lw::infectionHours(wounds) == lw::kContactInfectionHours);
    CHECK(lw::kContactInfectionHours > lw::kInfectionHours);
    CHECK(lw::isInfected(wounds) && !lw::isFeverish(wounds) && !lw::isVisiblyIll(wounds));
    CHECK(lw::contagiousness(wounds) == 0.0f);
    CHECK(field.count(lw::ChronicleKind::Infected) == 1);
    const lw::ChronicleEvent& infected = field.world.chronicle().events().back();
    CHECK(infected.kind == lw::ChronicleKind::Infected && infected.subject == id &&
          infected.object == field.id(sick) &&
          infected.a == static_cast<std::int32_t>(lw::InfectionRoute::Household));
    CHECK(lw::infectionRouteOf(field.world, id) == lw::InfectionRoute::Household);

    // Конец скрытого периода: заразен слабо; лихорадка — в полную силу.
    wounds.infection = 0.35f;
    CHECK(lw::contagiousness(wounds) == lw::kLatentShedding && !lw::isVisiblyIll(wounds));
    wounds.infection = 0.49f;
    const float toFever = lw::hoursToFever(wounds);
    CHECK(std::abs(toFever - 0.01f * lw::kContactInfectionHours) < 1e-3f);
    const std::uint64_t start = field.world.tick();
    for (int minute = 0; minute < 120 && field.count(lw::ChronicleKind::Feverish) == 0; ++minute) {
        field.steps(lw::kTicksPerGameMinute);
    }
    CHECK(field.count(lw::ChronicleKind::Feverish) == 1);
    const std::uint64_t feverAt = field.world.tick() - start;
    CHECK(feverAt <= static_cast<std::uint64_t>(toFever * lw::kTicksPerGameHour) + 2 * lw::kTicksPerGameMinute);
    CHECK(lw::isFeverish(registry.get<lw::Wounds>(person)) &&
          lw::isVisiblyIll(registry.get<lw::Wounds>(person)) &&
          lw::contagiousness(registry.get<lw::Wounds>(person)) == 1.0f);

    // Под конец — смерть от заражения и подъём, хотя укусов не было.
    registry.get<lw::Wounds>(person).infection = 0.98f;
    for (int minute = 0; minute < 180 && !registry.all_of<lw::Corpse>(person); ++minute) {
        field.steps(lw::kTicksPerGameMinute);
    }
    CHECK(registry.all_of<lw::Corpse>(person));
    if (const auto* corpse = registry.try_get<lw::Corpse>(person)) {
        CHECK(corpse->cause == lw::DeathCause::Infection && corpse->killer == lw::kInvalidId);
        CHECK(corpse->turnsAt > corpse->diedAt);
    }
    CHECK(lw::infectionRouteOf(field.world, id) == lw::InfectionRoute::Household); // по летописи
    for (int minute = 0; minute < 60 && !registry.all_of<lw::Zombie>(person); ++minute) {
        field.steps(lw::kTicksPerGameMinute);
    }
    CHECK(registry.all_of<lw::Zombie>(person) && field.count(lw::ChronicleKind::Turned) == 1);
}

// Рядом с больным заражаются: под одной крышей — у двери общего дома, просто
// рядом — в той же клетке; дальше нескольких метров — нет. Путь и от кого — в
// ранах и летописи. Тело заражённого заразно тому, кто стоит рядом.
void testContagion() {
    constexpr int kGroup = 12;
    Field field(2 * kGroup + 2);
    CHECK(field.open && field.people.size() >= 2 * kGroup + 2);
    if (!field.open || field.people.size() < 2 * kGroup + 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity sick = field.people[0];
    const entt::entity far = field.people[1];
    const lw::EntityId sickId = field.id(sick);
    // Больной в лихорадке стоит в клетке ряда; его заражение держим на месте.
    field.freeze(sick, 10);
    lw::Wounds& fever = registry.get<lw::Wounds>(sick);
    fever.infection = 0.6f;
    fever.route = lw::InfectionRoute::Origin;
    // Половина — его домашние (тот же дом), половина — чужие, все в той же клетке.
    const lw::EntityId home = registry.get<lw::Person>(sick).home;
    std::vector<entt::entity> housemates;
    std::vector<entt::entity> strangers;
    for (int i = 0; i < 2 * kGroup; ++i) {
        const entt::entity person = field.people[static_cast<std::size_t>(i + 2)];
        field.freeze(person, 10);
        lw::Person& self = registry.get<lw::Person>(person);
        if (i < kGroup) {
            self.home = home;
            housemates.push_back(person);
        } else {
            self.home = lw::kInvalidId;
            strangers.push_back(person);
        }
        registry.get<lw::Needs>(person) = {};
    }
    // Дверь их дома — та самая клетка: «у себя дома» — значит здесь.
    if (const entt::entity house = field.world.find(home); house != entt::null) {
        registry.get<lw::Building>(house).door = field.at(10);
    }
    field.freeze(far, 13); // 12 м — уже не рядом
    for (int hour = 0; hour < 10; ++hour) {
        field.steps(lw::kTicksPerGameHour);
        registry.get<lw::Wounds>(sick).infection = 0.6f;
        registry.get<lw::Needs>(sick).health = 1.0f;
    }
    int household = 0;
    int nearby = 0;
    for (const lw::ChronicleEvent& event : field.world.chronicle().events()) {
        if (event.kind != lw::ChronicleKind::Infected) {
            continue;
        }
        CHECK(event.object == sickId);
        const entt::entity person = field.world.find(event.subject);
        const bool mate = std::find(housemates.begin(), housemates.end(), person) != housemates.end();
        const auto route = static_cast<lw::InfectionRoute>(event.a);
        CHECK(route == (mate ? lw::InfectionRoute::Household : lw::InfectionRoute::Nearby));
        household += route == lw::InfectionRoute::Household ? 1 : 0;
        nearby += route == lw::InfectionRoute::Nearby ? 1 : 0;
        const lw::Wounds& wounds = registry.get<lw::Wounds>(person);
        CHECK(wounds.route == route && wounds.infectedBy == sickId && wounds.bites == 0);
    }
    CHECK(household > 0 && nearby > 0);
    CHECK(!lw::isInfected(registry, far));

    // Тело заражённого (пока не встало) — тем, кто стоит рядом.
    Field morgue(kGroup + 1);
    CHECK(morgue.open && morgue.people.size() >= static_cast<std::size_t>(kGroup + 1));
    if (!morgue.open || morgue.people.size() < static_cast<std::size_t>(kGroup + 1)) {
        return;
    }
    entt::registry& place = morgue.world.registry();
    const entt::entity dead = morgue.people[0];
    morgue.freeze(dead, 5);
    place.get<lw::Wounds>(dead).infection = 0.4f;
    lw::die(morgue.world, dead, lw::DeathCause::Wounds, lw::kInvalidId);
    CHECK(place.get<lw::Corpse>(dead).turnsAt > morgue.world.tick());
    place.get<lw::Corpse>(dead).turnsAt = morgue.world.tick() + 20 * lw::kTicksPerGameHour;
    for (int i = 1; i <= kGroup; ++i) {
        morgue.freeze(morgue.people[static_cast<std::size_t>(i)], 5);
        place.get<lw::Person>(morgue.people[static_cast<std::size_t>(i)]).home = lw::kInvalidId;
    }
    morgue.steps(10 * lw::kTicksPerGameHour);
    int body = 0;
    for (const lw::ChronicleEvent& event : morgue.world.chronicle().events()) {
        if (event.kind == lw::ChronicleKind::Infected) {
            CHECK(event.object == morgue.id(dead) &&
                  event.a == static_cast<std::int32_t>(lw::InfectionRoute::Body));
            ++body;
        }
    }
    CHECK(body > 0);
}

// Здоровье кончилось от голода — смерть от голода; неукушенный не встаёт.
void testStarvationDeath() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    const entt::entity person = field.people[0];
    field.freeze(person, 0);
    entt::registry& registry = field.world.registry();
    lw::Needs& needs = registry.get<lw::Needs>(person);
    needs.hunger = 1.0f;
    needs.thirst = 0.5f;
    needs.health = 0.0001f;
    field.steps(2 * lw::kTicksPerGameMinute);
    CHECK(registry.all_of<lw::Corpse>(person));
    if (const auto* corpse = registry.try_get<lw::Corpse>(person)) {
        CHECK(corpse->cause == lw::DeathCause::Starvation);
        CHECK(corpse->turnsAt == 0);
    }
}

// Мясо в туше портится, тела разлагаются и исчезают.
void testBodiesDecay() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    field.dismiss(field.people[0]);
    lw::World& world = field.world;
    entt::registry& registry = world.registry();
    const entt::entity deer = lw::createAnimal(world, lw::AnimalKind::Deer, field.at(0), field.rng);
    const entt::entity zombie = field.zombie(10);
    lw::die(world, deer, lw::DeathCause::Wounds, lw::kInvalidId);
    lw::die(world, zombie, lw::DeathCause::Wounds, lw::kInvalidId);
    const lw::EntityId deerId = field.id(deer);
    const lw::EntityId zombieId = field.id(zombie);
    CHECK(registry.get<lw::Corpse>(deer).meat == lw::carcassMeat(lw::AnimalKind::Deer));
    // Состарить: туше — сутки, телу — почти до разложения.
    const std::uint64_t now = world.tick();
    registry.get<lw::Corpse>(deer).diedAt = now - lw::kMeatSpoilTicks;
    registry.get<lw::Corpse>(zombie).diedAt = now - lw::kBodyDecayTicks + 2 * lw::kTicksPerGameMinute;
    field.steps(lw::kTicksPerGameMinute + 1);
    CHECK(world.find(deerId) != entt::null && registry.get<lw::Corpse>(deer).meat == 0);
    CHECK(world.find(zombieId) != entt::null);
    // Беззнаковая разность: «умер столько-то тиков назад» верно и в начале мира.
    registry.get<lw::Corpse>(deer).diedAt = world.tick() - lw::kCarcassDecayTicks;
    field.steps(2 * lw::kTicksPerGameMinute);
    CHECK(world.find(deerId) == entt::null);
    CHECK(world.find(zombieId) == entt::null);
}

// Тело человека рядом пугает.
void testBodyScares() {
    Field field(2);
    CHECK(field.open && field.people.size() == 2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    const entt::entity alive = field.people[0];
    const entt::entity dead = field.people[1];
    field.freeze(alive, 0);
    field.freeze(dead, 1);
    entt::registry& registry = field.world.registry();
    registry.get<lw::Traits>(alive) = {};
    registry.get<lw::Needs>(alive) = {};
    lw::die(field.world, dead, lw::DeathCause::Wounds, lw::kInvalidId);
    field.steps(60 * lw::kTicksPerGameMinute);
    CHECK(registry.get<lw::Needs>(alive).fear > 0.1f);
}

// Перелетая сама, стая садится у тела.
void testCrowsFlyToBody() {
    Field field(1);
    CHECK(field.open && !field.people.empty());
    if (!field.open || field.people.empty()) {
        return;
    }
    field.dismiss(field.people[0]);
    lw::World& world = field.world;
    entt::registry& registry = world.registry();
    const entt::entity zombie = field.zombie(29);
    lw::die(world, zombie, lw::DeathCause::Wounds, lw::kInvalidId);
    const entt::entity flock = lw::createFlock(world, field.at(0), 10, field.rng);
    registry.get<lw::Flock>(flock).until = world.tick();
    world.syncSpatial();
    for (std::uint64_t i = 0; i < lw::kAnimalThinkTicks && !registry.get<lw::Flock>(flock).flying; ++i) {
        field.steps(1);
    }
    CHECK(registry.get<lw::Flock>(flock).flying);
    for (int second = 0; second < 30 && registry.get<lw::Flock>(flock).flying; ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(field.distance(flock, zombie) <= 6.0f);
}


// --- Инвентарь и лут (этап 5.6) -------------------------------------------------

// Предметы: вес, место, лучшее оружие с учётом патронов, мясо — первым.
void testInventory() {
    using K = lw::ItemKind;
    lw::Inventory inventory;
    inventory.set(K::Water, 10);
    CHECK(std::abs(lw::inventoryWeight(inventory) - 10.0f) < 1e-4f);
    CHECK(lw::roomFor(inventory, K::Food) == 10); // 5 кг свободно по 0,5 кг
    CHECK(inventory.take(K::Water, 4) == 4 && inventory.count(K::Water) == 6);
    CHECK(inventory.take(K::Water, 40) == 6 && inventory.empty());

    // Ружьё без патронов хуже топора, с патронами — лучше.
    lw::Weapon weapon{lw::WeaponKind::Axe, 0};
    inventory.set(K::Rifle, 1);
    CHECK(!lw::equipBest(weapon, inventory));
    CHECK(weapon.kind == lw::WeaponKind::Axe);
    inventory.set(K::RifleAmmo, 5);
    CHECK(lw::equipBest(weapon, inventory));
    CHECK(weapon.kind == lw::WeaponKind::Rifle && weapon.ammo == 5);
    CHECK(inventory.count(K::Rifle) == 0 && inventory.count(K::RifleAmmo) == 0);
    CHECK(inventory.count(K::Axe) == 1); // прежнее — в инвентарь

    inventory = {};
    inventory.set(K::Food, 1);
    inventory.set(K::Meat, 1);
    CHECK(lw::eatFrom(inventory) && inventory.count(K::Meat) == 0 && inventory.count(K::Food) == 1);
}

// Лут в зданиях: снаряжение есть, в магазинах нет ружей; один seed — один лут.
void testLootSpawn() {
    const lw::World a(7, regionConfig());
    const lw::World b(7, regionConfig());
    const entt::registry& registry = a.registry();
    long long bandages = 0;
    bool shopGuns = false;
    for (const auto [entity, building, inventory] :
         registry.view<const lw::Building, const lw::Inventory>().each()) {
        bandages += inventory.count(lw::ItemKind::Bandage);
        if (building.kind == lw::BuildingKind::Shop) {
            shopGuns = shopGuns || inventory.count(lw::ItemKind::Pistol) +
                                           inventory.count(lw::ItemKind::Shotgun) +
                                           inventory.count(lw::ItemKind::Rifle) > 0;
        }
    }
    CHECK(bandages > 0);
    CHECK(!shopGuns);
    CHECK(a.stateHash() == b.stateHash());
}

// Голодный обыскивает здание: кроме еды, берёт ружьё с патронами и бинты.
void testScavengeTakesGear() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    if (lone.home == entt::null) {
        return;
    }
    for (const auto [entity, building, inventory] :
         registry.view<const lw::Building, lw::Inventory>().each()) {
        inventory = {};
        inventory.set(lw::ItemKind::Food, 3);
        inventory.set(lw::ItemKind::Rifle, 1);
        inventory.set(lw::ItemKind::RifleAmmo, 6);
        inventory.set(lw::ItemKind::Bandage, 2);
    }
    registry.get<lw::Inventory>(lone.home) = {};
    registry.get<lw::Inventory>(lone.person) = {};
    registry.get<lw::Weapon>(lone.person) = {lw::WeaponKind::Knife, 0};
    registry.get<lw::Needs>(lone.person).hunger = 0.8f;
    bool found = false;
    for (int minute = 0; minute < 8 * 60 && !found; minute += 10) {
        lone.run(10);
        found = lone.world.chronicle().countOf(lw::ChronicleKind::FoundWeapon) > 0;
    }
    CHECK(found);
    const lw::Weapon& weapon = registry.get<lw::Weapon>(lone.person);
    CHECK(weapon.kind == lw::WeaponKind::Rifle && weapon.ammo == 6);
    CHECK(registry.get<lw::Inventory>(lone.person).count(lw::ItemKind::Bandage) >= 2);
}

// Течёт кровь, есть бинт — перевязывается, кровотечение останавливается.
void testTreatsWound() {
    Lone lone;
    entt::registry& registry = lone.world.registry();
    registry.get<lw::Inventory>(lone.person).set(lw::ItemKind::Bandage, 1);
    registry.get<lw::Wounds>(lone.person).bleeding = 0.3f;
    const lw::ActionScores scores = lw::evaluateActions(lone.world, lone.person);
    CHECK(scores[static_cast<std::size_t>(lw::Action::Treat)] > 0.9f);
    lw::Mind& mind = registry.get<lw::Mind>(lone.person);
    mind.action = lw::Action::Idle;
    mind.phase = lw::ActionPhase::Perform;
    mind.until = lone.world.tick();
    lone.run(10);
    CHECK(registry.get<lw::Wounds>(lone.person).bleeding == 0.0f);
    CHECK(registry.get<lw::Inventory>(lone.person).count(lw::ItemKind::Bandage) == 0);
    CHECK(lone.world.chronicle().countOf(lw::ChronicleKind::Treated) == 1);
}

// Тело с вещами рядом обыскивают раньше зданий: дробовик погибшего — в руки.
void testLootsBody() {
    Lone lone;
    lw::World& world = lone.world;
    entt::registry& registry = world.registry();
    if (lone.home == entt::null) {
        return;
    }
    for (const auto [entity, building, inventory] :
         registry.view<const lw::Building, lw::Inventory>().each()) {
        inventory.set(lw::ItemKind::Food, 0);
    }
    registry.get<lw::Inventory>(lone.person) = {};
    registry.get<lw::Weapon>(lone.person) = {};
    registry.get<lw::Needs>(lone.person).hunger = 0.8f;
    const lw::Cell door = registry.get<lw::Building>(lone.home).door;
    const auto near = world.nav().nearestPassable({door.x + 10, door.y}, 6);
    CHECK(near.has_value());
    if (!near) {
        return;
    }
    lw::Rng rng(5);
    const entt::entity body = lw::createZombie(world, *near, rng);
    lw::Inventory loot;
    loot.set(lw::ItemKind::Food, 2);
    registry.emplace<lw::Inventory>(body, loot);
    registry.emplace<lw::Weapon>(body, lw::Weapon{lw::WeaponKind::Shotgun, 4});
    lw::die(world, body, lw::DeathCause::Wounds, lw::kInvalidId);
    world.syncSpatial();
    bool looted = false;
    for (int minute = 0; minute < 4 * 60 && !looted; minute += 10) {
        lone.run(10);
        looted = world.chronicle().countOf(lw::ChronicleKind::LootedBody) > 0;
    }
    CHECK(looted);
    CHECK(registry.get<lw::Weapon>(lone.person).kind == lw::WeaponKind::Shotgun);
    CHECK(registry.get<lw::Weapon>(lone.person).ammo == 4);
    CHECK(registry.get<lw::Weapon>(body).kind == lw::WeaponKind::Fists);
}

// Мясо портится: к полуночи от запаса остаётся половина.
void testMeatSpoils() {
    lw::World world(7, regionConfig());
    entt::registry& registry = world.registry();
    const entt::entity building = *registry.view<lw::Building>().begin();
    registry.get<lw::Inventory>(building).set(lw::ItemKind::Meat, 8);
    const std::uint64_t end = world.tick() + lw::kTicksPerGameDay;
    while (world.tick() < end) {
        world.step();
    }
    CHECK(registry.get<lw::Inventory>(building).count(lw::ItemKind::Meat) == 4);
}


// --- Дефицит и распределение (этап 5.7) ------------------------------------------

// Выбитых зверей восполняют пришедшие издалека: по группе вида в сутки.
void testAnimalsReturn() {
    lw::WorldConfig config = regionConfig();
    config.animals = 20;
    lw::World world(11, config);
    entt::registry& registry = world.registry();
    std::vector<entt::entity> deer;
    for (const auto [entity, animal] : registry.view<const lw::Animal>().each()) {
        if (animal.kind == lw::AnimalKind::Deer) {
            deer.push_back(entity);
        }
    }
    CHECK(deer.size() == 10);
    for (const entt::entity entity : deer) {
        lw::die(world, entity, lw::DeathCause::Wounds, lw::kInvalidId);
    }
    const auto alive = [&] {
        std::size_t count = 0;
        for (const auto [entity, animal] : registry.view<const lw::Animal>().each()) {
            count += animal.kind == lw::AnimalKind::Deer ? 1 : 0;
        }
        return count;
    };
    CHECK(alive() == 0);
    const std::uint64_t end = world.tick() + 3 * lw::kTicksPerGameDay;
    while (world.tick() < end) {
        world.step();
    }
    // Три полуночи — три стада по 2–5, но не больше начального числа.
    CHECK(alive() >= 6 && alive() <= 10);
}

// В каждом городе есть аптека: магазин с лекарствами.
void testPharmacies() {
    const lw::World world(7, regionConfig());
    const entt::registry& registry = world.registry();
    for (const lw::Places::SettlementPlace& place : world.places().settlements()) {
        const lw::Settlement& settlement = registry.get<lw::Settlement>(world.find(place.id));
        if (settlement.kind != lw::SettlementKind::Town) {
            continue;
        }
        bool pharmacy = false;
        for (const lw::Places::BuildingPlace& building : place.buildings) {
            pharmacy = pharmacy ||
                       (building.kind == lw::BuildingKind::Shop &&
                        registry.get<lw::Inventory>(world.find(building.id)).count(lw::ItemKind::Medicine) >= 4);
        }
        const bool hasShop = std::any_of(place.buildings.begin(), place.buildings.end(),
                                         [](const lw::Places::BuildingPlace& b) {
                                             return b.kind == lw::BuildingKind::Shop;
                                         });
        CHECK(pharmacy || !hasShop);
    }
}


// Упокоенных зомби заменяют пришедшие издалека: в полночь — орда.
void testZombiesArrive() {
    lw::WorldConfig config = regionConfig();
    config.people = 10;
    config.zombies = 40;
    lw::World world(11, config);
    entt::registry& registry = world.registry();
    std::vector<entt::entity> zombies;
    for (const auto [entity, zombie] : registry.view<const lw::Zombie>().each()) {
        zombies.push_back(entity);
    }
    CHECK(zombies.size() == 40);
    for (std::size_t i = 0; i < 20; ++i) {
        lw::die(world, zombies[i], lw::DeathCause::Wounds, lw::kInvalidId);
    }
    const std::size_t formed = world.chronicle().countOf(lw::ChronicleKind::HordeFormed);
    const std::uint64_t end = world.tick() + lw::kTicksPerGameDay;
    while (world.tick() < end) {
        world.step();
    }
    const std::size_t alive = registry.view<const lw::Zombie>().size();
    CHECK(alive >= 20 + lw::kArrivalMin - 2 && alive <= 36); // не больше 90 %; кого-то могли упокоить
    CHECK(world.chronicle().countOf(lw::ChronicleKind::HordeFormed) > formed);
    const auto& events = world.chronicle().events();
    CHECK(std::any_of(events.begin(), events.end(), [](const lw::ChronicleEvent& e) {
        return e.kind == lw::ChronicleKind::HordeFormed && e.b == 2;
    }));
}

// Совместимость: общая часть пары одна с обеих сторон, доброта нравится,
// агрессия отталкивает мирных сильнее, чем агрессивных.
void testChemistry() {
    const lw::Traits average{};
    lw::Traits kind;
    kind.altruism = 0.9f;
    lw::Traits harsh;
    harsh.aggression = 0.9f;
    lw::Traits peaceful;
    peaceful.aggression = 0.1f;
    int friendly = 0;
    int hostile = 0;
    bool symmetric = true;
    bool inRange = true;
    bool kindLiked = true;
    bool harshDisliked = true;
    constexpr int kPairs = 2000;
    for (lw::EntityId a = 1; a <= kPairs; ++a) {
        const lw::EntityId b = a * 7919 + 13;
        const float ab = lw::chemistry(a, average, b, average);
        symmetric = symmetric && ab == lw::chemistry(b, average, a, average);
        inRange = inRange && ab >= -1.0f && ab <= 1.0f;
        kindLiked = kindLiked && lw::chemistry(a, average, b, kind) > ab;
        harshDisliked = harshDisliked && lw::chemistry(a, peaceful, b, harsh) <
                                             lw::chemistry(a, harsh, b, harsh);
        friendly += ab >= lw::kFriendAffinity ? 1 : 0;
        hostile += ab <= lw::kFoeAffinity ? 1 : 0;
    }
    CHECK(symmetric);
    CHECK(inRange);
    CHECK(kindLiked);
    CHECK(harshDisliked);
    // Примерно каждый четвёртый — в друзья, каждый двенадцатый — в недруги.
    CHECK(friendly > kPairs / 6 && friendly < kPairs / 3);
    CHECK(hostile > kPairs / 25 && hostile < kPairs / 6);
}

// Мест для связей мало: новое знакомство вытесняет только почти забытое.
void testAcquireBond() {
    lw::Relations relations;
    bool created = false;
    for (lw::EntityId id = 1; id <= lw::Relations::kMaxBonds; ++id) {
        lw::Bond* bond = lw::acquireBond(relations, id, 0.2f, 5, &created);
        CHECK(bond != nullptr && created && bond->other == id && bond->met == 5);
        if (bond != nullptr) {
            bond->familiarity = 0.5f;
        }
    }
    CHECK(relations.count == lw::Relations::kMaxBonds);
    // Уже знакомый — та же связь.
    CHECK(lw::acquireBond(relations, 3, 0.9f, 7, &created) == relations.find(3) && !created);
    CHECK(relations.find(3)->chemistry == 0.2f);
    // Все связи крепкие — незнакомца не запомнить.
    CHECK(lw::acquireBond(relations, 100, 0.2f, 9, &created) == nullptr && !created);
    // Почти забытая уступает место.
    relations.find(7)->familiarity = 0.05f;
    lw::Bond* fresh = lw::acquireBond(relations, 100, 0.2f, 9, &created);
    CHECK(fresh != nullptr && created && fresh->other == 100);
    CHECK(relations.find(7) == nullptr);
    CHECK(relations.count == lw::Relations::kMaxBonds);
}

// С начала мира соседи по дому знают друг друга, а люди — ближайших соседей.
void testRelationsAtStart() {
    lw::WorldConfig config = regionConfig();
    config.people = 150;
    lw::World world(7, config);
    const entt::registry& registry = world.registry();
    std::size_t housemates = 0;
    bool know = true;
    std::size_t withBonds = 0;
    for (const auto [entity, id, person, relations] :
         registry.view<const lw::Id, const lw::Person, const lw::Relations>().each()) {
        withBonds += relations.count > 0 ? 1 : 0;
        for (const auto [other, otherId, otherPerson] :
             registry.view<const lw::Id, const lw::Person>().each()) {
            if (other == entity || otherPerson.home != person.home) {
                continue;
            }
            ++housemates;
            const lw::Bond* bond = relations.find(otherId.value);
            know = know && (bond != nullptr || relations.count == lw::Relations::kMaxBonds);
            know = know && (bond == nullptr || bond->familiarity > 0.5f);
        }
    }
    CHECK(know);
    CHECK(housemates > 0);
    CHECK(withBonds > 100);
    // Знакомства — без рандома и одинаковые для одного seed.
    lw::World twin(7, config);
    CHECK(world.stateHash() == twin.stateHash());
}

// Время вместе: знакомство растёт, симпатия тянется к совместимости; дальше —
// друг, а с несовместимым — недруг. Оба события — в летописи.
void testRelationsGrow() {
    Field field(2);
    CHECK(field.open && field.people.size() == 2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity a = field.people[0];
    const entt::entity b = field.people[1];
    registry.get<lw::Relations>(a) = {};
    registry.get<lw::Relations>(b) = {};
    field.place(a, 10);
    field.place(b, 12);
    registry.get<lw::Person>(a).asleep = false;
    registry.get<lw::Person>(b).asleep = false;
    lw::Mind& mind = registry.get<lw::Mind>(a);
    mind.action = lw::Action::Socialize; // разговор
    mind.phase = lw::ActionPhase::Perform;
    registry.get<lw::Mind>(b).action = lw::Action::Idle;

    lw::updateRelations(field.world, 60.0f);
    lw::Bond* ab = registry.get<lw::Relations>(a).find(field.id(b));
    const lw::Bond* ba = registry.get<lw::Relations>(b).find(field.id(a));
    CHECK(ab != nullptr && ba != nullptr);
    if (ab == nullptr || ba == nullptr) {
        return;
    }
    const float first = ab->familiarity;
    CHECK(first > 0.25f && first < 0.4f);
    CHECK(ba->familiarity == first); // в разговоре оба
    for (int hour = 0; hour < 5; ++hour) {
        lw::updateRelations(field.world, 60.0f);
    }
    CHECK(ab->familiarity > 0.75f);
    CHECK(std::abs(ab->affinity - ab->chemistry) < std::abs(ab->chemistry) * 0.6f + 1e-6f);

    // Сошлись характерами — друзья.
    ab->chemistry = 1.0f;
    for (int hour = 0; hour < 8; ++hour) {
        lw::updateRelations(field.world, 60.0f);
    }
    CHECK(ab->friendly() && ab->trust > 0.1f);
    CHECK(field.world.chronicle().countOf(lw::ChronicleKind::Befriended) >= 1);

    // Не сошлись — дружба прошла, невзлюбил.
    ab->chemistry = -1.0f;
    for (int hour = 0; hour < 12; ++hour) {
        lw::updateRelations(field.world, 60.0f);
    }
    CHECK(!ab->friendly() && ab->hostile());
    CHECK(field.world.chronicle().countOf(lw::ChronicleKind::Disliked) >= 1);
    // Доверие от общения не падает: его подорвут только поступки.
    CHECK(ab->trust > 0.1f);

    // Врозь знакомство понемногу забывается: не виделись больше двух суток.
    field.freeze(a, 0);
    field.dismiss(b);
    registry.get<lw::Needs>(a) = {};
    const float before = ab->familiarity;
    field.steps(2 * lw::kTicksPerGameDay + 2 * lw::kTicksPerGameHour);
    CHECK(ab->familiarity < before && ab->familiarity > before - 0.01f);
}

// Рядом с недругом одиночество не проходит.
void testFoeIsNoCompany() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity a = field.people[0];
    const entt::entity b = field.people[1];
    field.freeze(a, 10);
    field.freeze(b, 12);
    registry.get<lw::Relations>(a) = {};
    registry.get<lw::Relations>(b) = {};
    lw::Bond* bond = lw::acquireBond(registry.get<lw::Relations>(a), field.id(b), -1.0f, 0);
    CHECK(bond != nullptr);
    if (bond == nullptr) {
        return;
    }
    bond->familiarity = 0.8f;
    bond->affinity = -0.8f;
    lw::updateBondFlags(*bond);
    CHECK(bond->hostile());
    registry.get<lw::Needs>(a).loneliness = 0.5f;
    registry.get<lw::Needs>(b).loneliness = 0.5f;
    lw::updateNeeds(field.world, 30.0f);
    CHECK(registry.get<lw::Needs>(a).loneliness > 0.5f);
    CHECK(registry.get<lw::Needs>(b).loneliness < 0.5f);
}

// Одинокий идёт к другу, даже если тот дальше, чем ищут незнакомцев, а к
// недругу не идёт, даже если тот рядом.
void testSocializeSeeksFriend() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity a = field.people[0];
    const entt::entity b = field.people[1];
    const lw::NavGrid& nav = field.world.nav();
    // Друг — в 800 м: незнакомцев так далеко не ищут.
    std::optional<lw::Cell> spot;
    for (int dy = -250; dy <= 250 && !spot; dy += 10) {
        for (int dx = -250; dx <= 250 && !spot; dx += 10) {
            const lw::Cell cell{field.row.x + dx, field.row.y + dy};
            const int d2 = dx * dx + dy * dy;
            if (d2 >= 160 * 160 && d2 <= 240 * 240 && nav.passable(cell) &&
                nav.connected(field.row, cell)) {
                spot = cell;
            }
        }
    }
    CHECK(spot.has_value());
    if (!spot) {
        return;
    }
    field.place(a, 10);
    registry.get<lw::Person>(a).asleep = false;
    registry.get<lw::Traits>(a) = {};
    registry.get<lw::Needs>(a) = {};
    registry.get<lw::Needs>(a).loneliness = 1.0f;
    registry.get<lw::Relations>(a) = {};
    lw::Bond* bond = lw::acquireBond(registry.get<lw::Relations>(a), field.id(b), 0.8f, 0);
    CHECK(bond != nullptr);
    if (bond == nullptr) {
        return;
    }
    bond->familiarity = 0.9f;
    bond->affinity = 0.8f;
    lw::updateBondFlags(*bond);
    field.freeze(b, 0);
    registry.get<lw::Position>(b) = {lw::Map::cellCenter(spot->x), lw::Map::cellCenter(spot->y)};
    field.world.syncSpatial();

    const auto decide = [&]() {
        lw::Mind& mind = registry.get<lw::Mind>(a);
        mind.action = lw::Action::Idle;
        mind.phase = lw::ActionPhase::Perform;
        mind.until = field.world.tick();
        mind.nextThink = field.world.tick();
        field.world.step();
        return registry.get<lw::Mind>(a);
    };
    const lw::Mind visit = decide();
    CHECK(visit.action == lw::Action::Socialize && visit.target == field.id(b));

    // Недруг рядом: к нему не идут.
    bond->affinity = -0.8f;
    lw::updateBondFlags(*bond);
    CHECK(bond->hostile());
    field.place(a, 10);
    registry.get<lw::Position>(b) = {lw::Map::cellCenter(field.row.x + 14),
                                     lw::Map::cellCenter(field.row.y)};
    field.world.syncSpatial();
    const lw::Mind avoid = decide();
    CHECK(avoid.target != field.id(b));
}

// --- Память, долг и обида (этапы 6.1–6.2) ----------------------------------------

// Запомнить: повтор того же случая не новый, новое вытесняет самое слабое,
// угасшее забывается.
void testRememberAndFade() {
    lw::Memories memories;
    CHECK(lw::remember(memories, lw::MemoryKind::Robbed, 5, 9, 0.6f, 100));
    // Тот же случай в пределах часа — повтор: память свежее, но случай не новый.
    CHECK(!lw::remember(memories, lw::MemoryKind::Robbed, 5, 9, 0.6f, 200));
    CHECK(memories.count == 1 && memories.items[0].tick == 200);
    // Через сутки — новый случай, воспоминание крепче.
    CHECK(lw::remember(memories, lw::MemoryKind::Robbed, 5, 9, 0.6f, 200 + lw::kTicksPerGameDay));
    CHECK(memories.count == 1 && memories.items[0].strength > 0.6f);
    for (lw::EntityId who = 10; memories.count < lw::Memories::kMaxMemories; ++who) {
        CHECK(lw::remember(memories, lw::MemoryKind::Defended, who, lw::kInvalidId, 0.5f, 300));
    }
    // Места кончились: слабее всего помнящегося — не запомнить, сильнее — вытесняет.
    CHECK(!lw::remember(memories, lw::MemoryKind::Defended, 99, lw::kInvalidId, 0.4f, 400));
    CHECK(lw::remember(memories, lw::MemoryKind::Saved, 99, lw::kInvalidId, 1.0f, 400));
    CHECK(memories.count == lw::Memories::kMaxMemories);
    CHECK(memories.find(lw::MemoryKind::Saved, 99) != nullptr);
    CHECK(memories.find(lw::MemoryKind::Robbed, 5) != nullptr);
    // Угасание: за 30 суток — примерно в e раз; 0,5 ещё помнится, 1 — тем более.
    for (int day = 0; day < 30; ++day) {
        lw::fadeMemories(memories, 24.0f * 60.0f);
    }
    const lw::Memory* saved = memories.find(lw::MemoryKind::Saved, 99);
    CHECK(saved != nullptr && saved->strength > 0.3f && saved->strength < 0.42f);
    CHECK(memories.count == lw::Memories::kMaxMemories);
    // Ещё через 60 суток забыто всё.
    for (int day = 0; day < 60; ++day) {
        lw::fadeMemories(memories, 24.0f * 60.0f);
    }
    CHECK(memories.count == 0);
}

// Долг поднимает симпатию и доверие, обида опускает; к ним же тянется
// симпатия потом; угасают сами, обида у злопамятного — дольше.
void testDebtAndGrudge() {
    lw::Bond bond;
    bond.other = 2;
    bond.familiarity = 0.5f;
    bond.chemistry = 0.2f;
    bond.affinity = 0.2f;
    bond.trust = 0.1f;
    lw::addDebt(bond, 0.4f);
    CHECK(bond.debt == 0.4f && bond.affinity > 0.35f && bond.trust > 0.25f);
    CHECK(lw::affinityGoal(bond) > bond.chemistry);
    lw::addGrudge(bond, 0.6f);
    CHECK(bond.grudge == 0.6f && bond.affinity < 0.0f && bond.trust < 0.0f);
    CHECK(lw::affinityGoal(bond) < bond.chemistry);
    CHECK(lw::trustCeiling(bond) == 0.0f);
    // Не больше 1.
    lw::addGrudge(bond, 0.9f);
    CHECK(bond.grudge == 1.0f);
    // Долг и обида держат связь в памяти.
    lw::Bond plain = bond;
    plain.debt = 0.0f;
    plain.grudge = 0.0f;
    CHECK(lw::bondStrength(bond) > lw::bondStrength(plain) + 1.0f);

    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity a = field.people[0];
    const entt::entity b = field.people[1];
    registry.get<lw::Relations>(a) = {};
    registry.get<lw::Relations>(b) = {};
    lw::Bond* ab = lw::acquireBond(registry.get<lw::Relations>(a), field.id(b), 0.0f, 0);
    lw::Bond* ba = lw::acquireBond(registry.get<lw::Relations>(b), field.id(a), 0.0f, 0);
    CHECK(ab != nullptr && ba != nullptr);
    if (ab == nullptr || ba == nullptr) {
        return;
    }
    registry.get<lw::Traits>(a).aggression = 1.0f;
    registry.get<lw::Traits>(a).altruism = 0.0f;
    registry.get<lw::Traits>(b).aggression = 0.0f;
    registry.get<lw::Traits>(b).altruism = 1.0f;
    ab->grudge = 0.5f;
    ba->grudge = 0.5f;
    ab->debt = 0.5f;
    for (int day = 0; day < 5; ++day) {
        lw::updateRelations(field.world, 24.0f * 60.0f);
    }
    CHECK(ab->grudge > 0.4f && ab->grudge < 0.5f);
    CHECK(ba->grudge < 0.25f);
    CHECK(ab->debt > 0.4f && ab->debt < 0.5f);
}

// Убил зомби, который гнался за другим, — тот обязан ему жизнью.
void testRescueRemembered() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity rescuer = field.people[0];
    const entt::entity victim = field.people[1];
    field.place(rescuer, 0);
    field.place(victim, 6);
    registry.get<lw::Relations>(victim) = {};
    registry.get<lw::Traits>(victim) = {};
    const entt::entity zombie = field.zombie(4);
    lw::Zombie& state = registry.get<lw::Zombie>(zombie);
    state.state = lw::ZombieState::Chase;
    state.target = field.id(victim);
    lw::witnessRescue(field.world, field.id(rescuer), zombie);
    const lw::Memory* memory =
        registry.get<lw::Memories>(victim).find(lw::MemoryKind::Saved, field.id(rescuer));
    CHECK(memory != nullptr && memory->about == field.id(zombie) && memory->strength == 1.0f);
    const lw::Bond* bond = registry.get<lw::Relations>(victim).find(field.id(rescuer));
    CHECK(bond != nullptr && bond->debt > 0.5f && bond->familiarity >= 0.2f && bond->trust > 0.2f);
    CHECK(field.count(lw::ChronicleKind::SavedBy) == 1);
    // Спаситель сам ничего не запомнил: спасали не его.
    CHECK(registry.get<lw::Memories>(rescuer).count == 0);
    // Повтор того же — не новый случай.
    lw::witnessRescue(field.world, field.id(rescuer), zombie);
    CHECK(field.count(lw::ChronicleKind::SavedBy) == 1);
}

// Близкий убежал, когда на человека напал зомби, — бросил; кто дрался с
// напавшим — заступился.
void testAbandonedAndDefended() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity victim = field.people[0];
    const entt::entity friendly = field.people[1];
    const entt::entity stranger = field.people[2];
    field.place(victim, 0);
    field.place(friendly, 5);
    field.place(stranger, 4);
    registry.get<lw::Relations>(victim) = {};
    lw::Bond* bond =
        lw::acquireBond(registry.get<lw::Relations>(victim), field.id(friendly), 0.6f, 0);
    CHECK(bond != nullptr);
    if (bond == nullptr) {
        return;
    }
    bond->familiarity = 0.8f;
    bond->affinity = 0.6f;
    bond->trust = 0.4f;
    const entt::entity zombie = field.zombie(1);
    registry.get<lw::Combat>(zombie).target = field.id(victim);
    registry.get<lw::Mind>(friendly).action = lw::Action::Flee;
    registry.get<lw::Mind>(stranger).action = lw::Action::Fight;
    registry.get<lw::Combat>(stranger).target = field.id(zombie);
    // Жертва видит обоих.
    lw::Awareness& awareness = registry.get<lw::Awareness>(victim);
    awareness = {};
    for (const entt::entity other : {friendly, stranger}) {
        const lw::Position& at = registry.get<lw::Position>(other);
        awareness.contacts[awareness.count++] = {field.id(other), at.x, at.y, field.world.tick(),
                                                 true, lw::SoundKind::None};
    }
    lw::witnessAttack(field.world, victim, field.id(zombie));
    const lw::Memories& memories = registry.get<lw::Memories>(victim);
    CHECK(memories.find(lw::MemoryKind::Abandoned, field.id(friendly)) != nullptr);
    CHECK(memories.find(lw::MemoryKind::Defended, field.id(stranger)) != nullptr);
    CHECK(bond->grudge > 0.3f && bond->trust < 0.0f && bond->affinity < 0.3f);
    const lw::Bond* thanks = registry.get<lw::Relations>(victim).find(field.id(stranger));
    CHECK(thanks != nullptr && thanks->debt > 0.1f);
    CHECK(field.count(lw::ChronicleKind::AbandonedBy) == 1);
    CHECK(field.count(lw::ChronicleKind::DefendedBy) == 1);
    // Чужой человек, который убегал, никого не бросал: он не близкий.
    registry.get<lw::Mind>(stranger).action = lw::Action::Flee;
    if (lw::Bond* met = registry.get<lw::Relations>(victim).find(field.id(stranger))) {
        met->debt = 0.0f;
    }
    registry.get<lw::Memories>(victim) = {};
    lw::witnessAttack(field.world, victim, field.id(zombie));
    CHECK(registry.get<lw::Memories>(victim).find(lw::MemoryKind::Abandoned,
                                                  field.id(stranger)) == nullptr);
}

// Хозяин дома застал вора — обида; спящий хозяин ничего не заметил.
void testTheftWitnessed() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity owner = field.people[0];
    const entt::entity thief = field.people[1];
    const lw::EntityId home = registry.get<lw::Person>(owner).home;
    CHECK(home != lw::kInvalidId);
    if (home == lw::kInvalidId) {
        return;
    }
    registry.get<lw::Person>(thief).home = lw::kInvalidId; // чужой
    registry.get<lw::Relations>(owner) = {};
    registry.get<lw::Awareness>(owner) = {};
    registry.get<lw::Person>(owner).asleep = true;
    lw::witnessTheft(field.world, thief, home, 3);
    CHECK(registry.get<lw::Memories>(owner).count == 0);
    registry.get<lw::Person>(owner).asleep = false;
    registry.get<lw::Person>(owner).indoors = true;
    lw::witnessTheft(field.world, thief, home, 3);
    const lw::Memory* memory =
        registry.get<lw::Memories>(owner).find(lw::MemoryKind::Robbed, field.id(thief));
    CHECK(memory != nullptr && memory->about == home);
    const lw::Bond* bond = registry.get<lw::Relations>(owner).find(field.id(thief));
    CHECK(bond != nullptr && bond->grudge > 0.2f && bond->trust < 0.0f && bond->affinity < 0.0f);
    CHECK(bond != nullptr && bond->familiarity >= 0.2f);
    CHECK(field.count(lw::ChronicleKind::CaughtThief) == 1);
    // Ничего не взял — нечего и запоминать.
    registry.get<lw::Memories>(owner) = {};
    lw::witnessTheft(field.world, thief, home, 0);
    CHECK(registry.get<lw::Memories>(owner).count == 0);
}

// Нашёл тело близкого — горе: одиночество и страх; второй раз — не новое.
// Близкий среди зомби — тоже горе.
void testMourning() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity mourner = field.people[0];
    const entt::entity lost = field.people[1];
    const entt::entity turned = field.people[2];
    field.freeze(mourner, 0);
    field.place(lost, 5);
    field.dismiss(turned);
    registry.get<lw::Relations>(mourner) = {};
    registry.get<lw::Needs>(mourner) = {};
    for (const entt::entity other : {lost, turned}) {
        lw::Bond* bond =
            lw::acquireBond(registry.get<lw::Relations>(mourner), field.id(other), 0.8f, 0);
        CHECK(bond != nullptr);
        if (bond != nullptr) {
            bond->familiarity = 0.9f;
            bond->affinity = 0.8f;
        }
    }
    lw::die(field.world, lost, lw::DeathCause::Starvation, lw::kInvalidId);
    field.world.syncSpatial();
    lw::updateMemories(field.world, 1.0f);
    CHECK(registry.get<lw::Memories>(mourner).find(lw::MemoryKind::Bereaved, field.id(lost)) !=
          nullptr);
    CHECK(registry.get<lw::Needs>(mourner).loneliness > 0.3f);
    CHECK(registry.get<lw::Needs>(mourner).fear > 0.2f);
    CHECK(field.count(lw::ChronicleKind::Mourned) == 1);
    lw::updateMemories(field.world, 1.0f);
    CHECK(field.count(lw::ChronicleKind::Mourned) == 1);

    // Друг встал зомби — его узнают среди замеченных.
    lw::die(field.world, turned, lw::DeathCause::Infection, lw::kInvalidId);
    registry.remove<lw::Corpse>(turned);
    lw::Rng rng(3);
    lw::raiseZombie(field.world, turned, field.at(12), rng);
    field.world.syncSpatial();
    const lw::Position& at = registry.get<lw::Position>(turned);
    lw::Awareness& awareness = registry.get<lw::Awareness>(mourner);
    awareness = {};
    awareness.contacts[awareness.count++] = {field.id(turned), at.x, at.y, field.world.tick(),
                                             true, lw::SoundKind::None};
    lw::updateMemories(field.world, 1.0f);
    CHECK(registry.get<lw::Memories>(mourner).find(lw::MemoryKind::Turned, field.id(turned)) !=
          nullptr);
    CHECK(field.count(lw::ChronicleKind::SawTurned) == 1);
}

// --- Влияние событий на отношения (этап 6.3) -----------------------------------

// Связь self → other с заданными знакомством, симпатией и доверием.
lw::Bond* setBond(entt::registry& registry, entt::entity self, lw::EntityId other,
                  float familiarity, float affinity, float trust, float chemistry = 0.0f) {
    lw::Bond* bond = lw::acquireBond(registry.get<lw::Relations>(self), other, chemistry, 0);
    if (bond != nullptr) {
        bond->familiarity = familiarity;
        bond->affinity = affinity;
        bond->trust = trust;
        lw::updateBondFlags(*bond);
    }
    return bond;
}

// Наблюдатель видит other: свежий контакт «видит».
void letSee(Field& field, entt::entity observer, entt::entity other) {
    entt::registry& registry = field.world.registry();
    const lw::Position& at = registry.get<lw::Position>(other);
    lw::Awareness& awareness = registry.get<lw::Awareness>(observer);
    awareness.contacts[awareness.count++] = {field.id(other), at.x, at.y, field.world.tick(), true,
                                             lw::SoundKind::None};
}

// Кто видел со стороны, как обворовывают дом друга, запоминает вора и
// обижается за друга; чужая беда трогает слабо.
void testWitnessedTheft() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity owner = field.people[0];
    const entt::entity thief = field.people[1];
    const entt::entity bystander = field.people[2];
    const lw::EntityId home = registry.get<lw::Person>(owner).home;
    CHECK(home != lw::kInvalidId);
    if (home == lw::kInvalidId) {
        return;
    }
    registry.get<lw::Person>(thief).home = lw::kInvalidId;
    registry.get<lw::Person>(bystander).home = lw::kInvalidId;
    registry.get<lw::Person>(owner).asleep = true; // хозяин спит и ничего не видит
    field.place(thief, 0);
    field.place(bystander, 8);
    for (const entt::entity person : {owner, thief, bystander}) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
        registry.get<lw::Awareness>(person) = {};
        registry.get<lw::Traits>(person) = {};
    }
    setBond(registry, bystander, field.id(owner), 0.8f, 0.6f, 0.4f);
    letSee(field, bystander, thief);
    lw::witnessTheft(field.world, thief, home, 3);
    CHECK(registry.get<lw::Memories>(owner).count == 0);
    const lw::Memory* seen = registry.get<lw::Memories>(bystander).find(
        lw::MemoryKind::Robbed, field.id(thief), field.id(owner));
    CHECK(seen != nullptr && seen->source == lw::kInvalidId && seen->about == home);
    CHECK(seen != nullptr && seen->strength > 0.4f && seen->strength < 0.6f);
    const lw::Bond* grudge = registry.get<lw::Relations>(bystander).find(field.id(thief));
    CHECK(grudge != nullptr && grudge->grudge > 0.1f && grudge->affinity < 0.0f);
    CHECK(field.count(lw::ChronicleKind::Witnessed) == 1);

    // Пострадал чужой: случай запомнился слабее, обида — почти никакая, в
    // летопись не попал.
    registry.get<lw::Relations>(bystander) = {};
    registry.get<lw::Memories>(bystander) = {};
    lw::witnessTheft(field.world, thief, home, 3);
    const lw::Memory* stranger = registry.get<lw::Memories>(bystander).find(
        lw::MemoryKind::Robbed, field.id(thief), field.id(owner));
    CHECK(stranger != nullptr && seen != nullptr && stranger->strength < 0.4f);
    const lw::Bond* cold = registry.get<lw::Relations>(bystander).find(field.id(thief));
    CHECK(cold == nullptr || cold->grudge < 0.05f);
    CHECK(field.count(lw::ChronicleKind::Witnessed) == 1);
}

// Слухи: рассказывают то, что слушателю важно и чего он не знает; слух
// слабее увиденного и по доверию к рассказчику; дальше одного пересказа не
// идёт; ограбленный узнаёт, кто его обокрал; недругу не верят.
void testRumors() {
    Field field(4);
    if (!field.open || field.people.size() != 4) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity teller = field.people[0];
    const entt::entity listener = field.people[1];
    const entt::entity thief = field.people[2];
    const entt::entity robbed = field.people[3];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
        registry.get<lw::Traits>(person) = {};
    }
    const lw::EntityId thiefId = field.id(thief);
    // Вор обокрал рассказчика, а рассказчик видел, как тот обокрал и robbed.
    lw::remember(registry.get<lw::Memories>(teller), lw::MemoryKind::Robbed, thiefId, 1, 0.6f,
                 field.world.tick());
    lw::remember(registry.get<lw::Memories>(teller),
                 lw::Memory{lw::MemoryKind::Robbed, thiefId, field.id(robbed), 2, lw::kInvalidId,
                            field.world.tick(), 0.4f});
    setBond(registry, listener, field.id(teller), 0.8f, 0.6f, 0.6f);
    setBond(registry, listener, thiefId, 0.3f, 0.0f, 0.0f);

    // Слушатель знает вора: о краже у рассказчика.
    const lw::Memory* rumor = lw::pickRumor(field.world, teller, listener);
    CHECK(rumor != nullptr && rumor->victim == lw::kInvalidId && rumor->who == thiefId);
    if (rumor == nullptr) {
        return;
    }
    lw::hearRumor(field.world, listener, field.id(teller), *rumor);
    const lw::Memory* heard = registry.get<lw::Memories>(listener).find(
        lw::MemoryKind::Robbed, thiefId, field.id(teller));
    CHECK(heard != nullptr && heard->source == field.id(teller));
    CHECK(heard != nullptr && heard->strength < lw::kRetellStrength &&
          heard->strength >= lw::kForgotten);
    const lw::Bond* toThief = registry.get<lw::Relations>(listener).find(thiefId);
    CHECK(toThief != nullptr && toThief->grudge > 0.0f && toThief->affinity < 0.0f);
    // Второй раз то же не рассказывают; дальше слабый слух не идёт.
    const lw::Memory* next = lw::pickRumor(field.world, teller, listener);
    CHECK(next == nullptr || next->victim != lw::kInvalidId);
    setBond(registry, robbed, field.id(listener), 0.8f, 0.6f, 0.6f);
    setBond(registry, robbed, thiefId, 0.3f, 0.0f, 0.0f);
    CHECK(lw::pickRumor(field.world, listener, robbed) == nullptr);

    // Ограбленный узнаёт от рассказчика, кто его обокрал, — первым делом.
    setBond(registry, robbed, field.id(teller), 0.5f, 0.3f, 0.3f);
    rumor = lw::pickRumor(field.world, teller, robbed);
    CHECK(rumor != nullptr && rumor->victim == field.id(robbed));
    if (rumor != nullptr) {
        lw::hearRumor(field.world, robbed, field.id(teller), *rumor);
    }
    const lw::Memory* own = registry.get<lw::Memories>(robbed).find(lw::MemoryKind::Robbed, thiefId);
    CHECK(own != nullptr && own->source == field.id(teller));
    CHECK(field.count(lw::ChronicleKind::HeardOf) == 1);
    const lw::Bond* robbedToThief = registry.get<lw::Relations>(robbed).find(thiefId);
    CHECK(robbedToThief != nullptr && robbedToThief->grudge > 0.08f);

    // Недругу не верят.
    registry.get<lw::Memories>(listener) = {};
    registry.get<lw::Relations>(listener) = {};
    setBond(registry, listener, field.id(teller), 0.8f, -0.6f, -0.3f);
    setBond(registry, listener, thiefId, 0.3f, 0.0f, 0.0f);
    rumor = lw::pickRumor(field.world, teller, listener);
    CHECK(rumor != nullptr);
    if (rumor != nullptr) {
        lw::hearRumor(field.world, listener, field.id(teller), *rumor);
    }
    CHECK(registry.get<lw::Memories>(listener).count == 0);

    // В разговоре слух переходит сам (SocialSystem).
    registry.get<lw::Relations>(listener) = {};
    setBond(registry, listener, field.id(teller), 0.8f, 0.6f, 0.6f, 0.6f);
    setBond(registry, listener, thiefId, 0.3f, 0.0f, 0.0f);
    field.freeze(teller, 10);
    field.freeze(listener, 12);
    field.dismiss(thief);
    field.dismiss(robbed);
    lw::Mind talk;
    talk.action = lw::Action::Socialize;
    talk.phase = lw::ActionPhase::Perform;
    registry.emplace<lw::Mind>(teller, talk);
    lw::updateRelations(field.world, 1.0f);
    CHECK(registry.get<lw::Memories>(listener).find(lw::MemoryKind::Robbed, thiefId,
                                                     field.id(teller)) != nullptr);
}

// Весть о гибели близкого: горе и запись «узнал о гибели», с рассказчиком.
void testDeathRumor() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity teller = field.people[0];
    const entt::entity listener = field.people[1];
    const entt::entity lost = field.people[2];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
    }
    registry.get<lw::Needs>(listener) = {};
    setBond(registry, listener, field.id(lost), 0.9f, 0.8f, 0.6f);
    setBond(registry, listener, field.id(teller), 0.6f, 0.4f, 0.4f);
    lw::remember(registry.get<lw::Memories>(teller), lw::MemoryKind::Bereaved, field.id(lost),
                 lw::kInvalidId, 0.8f, field.world.tick());
    lw::die(field.world, lost, lw::DeathCause::Starvation, lw::kInvalidId);
    const lw::Memory* rumor = lw::pickRumor(field.world, teller, listener);
    CHECK(rumor != nullptr && rumor->kind == lw::MemoryKind::Bereaved);
    if (rumor == nullptr) {
        return;
    }
    lw::hearRumor(field.world, listener, field.id(teller), *rumor);
    const lw::Memory* grief =
        registry.get<lw::Memories>(listener).find(lw::MemoryKind::Bereaved, field.id(lost));
    CHECK(grief != nullptr && grief->source == field.id(teller));
    CHECK(registry.get<lw::Needs>(listener).loneliness > 0.1f);
    const auto& events = field.world.chronicle().events();
    CHECK(std::any_of(events.begin(), events.end(), [&](const lw::ChronicleEvent& e) {
        return e.kind == lw::ChronicleKind::Mourned && e.a == 0 && e.third == field.id(teller) &&
               e.object == field.id(lost);
    }));
    // Второй раз о том же не рассказывают.
    CHECK(lw::pickRumor(field.world, teller, listener) == nullptr);
}

// Враг моего друга: в разговоре симпатия слушателя к третьему тянется к мнению
// рассказчика, если рассказчик ему мил.
void testOpinionsSpread() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity speaker = field.people[0];
    const entt::entity listener = field.people[1];
    const entt::entity foe = field.people[2];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
    }
    field.freeze(speaker, 10);
    field.freeze(listener, 12);
    field.dismiss(foe);
    lw::Mind talk;
    talk.action = lw::Action::Socialize;
    talk.phase = lw::ActionPhase::Perform;
    registry.emplace<lw::Mind>(speaker, talk);
    setBond(registry, speaker, field.id(listener), 0.9f, 0.8f, 0.6f, 0.8f);
    setBond(registry, listener, field.id(speaker), 0.9f, 0.8f, 0.6f, 0.8f);
    lw::Bond* hated = setBond(registry, speaker, field.id(foe), 0.8f, -0.8f, -0.5f, -0.8f);
    lw::Bond* known = setBond(registry, listener, field.id(foe), 0.5f, 0.2f, 0.1f, 0.2f);
    CHECK(hated != nullptr && hated->hostile() && known != nullptr);
    if (known == nullptr) {
        return;
    }
    lw::updateRelations(field.world, 60.0f);
    CHECK(known->affinity < 0.0f);
    for (int hour = 0; hour < 3; ++hour) {
        lw::updateRelations(field.world, 60.0f);
    }
    CHECK(known->hostile());
    CHECK(field.count(lw::ChronicleKind::Disliked) >= 1);
}

// Добро гасит обиду; недруг, сделавший добро, — уже не недруг: помирились.
void testReconcile() {
    lw::Bond bond;
    bond.familiarity = 0.6f;
    bond.grudge = 0.3f;
    lw::addDebt(bond, 0.5f);
    CHECK(bond.grudge == 0.0f && std::abs(bond.debt - 0.2f) < 1e-6f);

    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity victim = field.people[0];
    const entt::entity rescuer = field.people[1];
    registry.get<lw::Relations>(victim) = {};
    registry.get<lw::Memories>(victim) = {};
    registry.get<lw::Traits>(victim) = {};
    lw::Bond* foe = setBond(registry, victim, field.id(rescuer), 0.6f, -0.35f, -0.3f);
    CHECK(foe != nullptr && foe->hostile());
    if (foe == nullptr) {
        return;
    }
    foe->grudge = 0.4f;
    lw::experience(field.world, victim, lw::MemoryKind::Saved, field.id(rescuer), lw::kInvalidId);
    CHECK(foe->grudge == 0.0f && !foe->hostile());
    CHECK(field.count(lw::ChronicleKind::Reconciled) == 1);
}

// --- Группы (этап 6.4) ----------------------------------------------------------

// Ближайшее к дому home другое жилое здание не дальше range метров; kInvalidId — нет.
lw::EntityId nearbyHome(const lw::World& world, lw::EntityId home, float range) {
    const entt::registry& registry = world.registry();
    const lw::Cell door = registry.get<lw::Building>(world.find(home)).door;
    lw::EntityId best = lw::kInvalidId;
    float nearest = range;
    for (const auto [entity, id, building] :
         registry.view<const lw::Id, const lw::Building>().each()) {
        if (id.value == home || (building.kind != lw::BuildingKind::House &&
                                 building.kind != lw::BuildingKind::Apartment)) {
            continue;
        }
        const float d = std::hypot(static_cast<float>(building.door.x - door.x),
                                   static_cast<float>(building.door.y - door.y)) *
                        lw::Map::kCellSize;
        if (d < nearest || (d == nearest && id.value < best)) {
            best = id.value;
            nearest = d;
        }
    }
    return best;
}

// Взаимные друзья с доверием.
void befriend(entt::registry& registry, const Field& field, entt::entity a, entt::entity b,
              float trust = 0.5f) {
    setBond(registry, a, field.id(b), 0.9f, 0.7f, trust, 0.7f);
    setBond(registry, b, field.id(a), 0.9f, 0.7f, trust, 0.7f);
}

const lw::Group* groupOf(const lw::World& world, entt::entity person) {
    const entt::entity group = world.find(world.registry().get<lw::Person>(person).group);
    return group != entt::null ? &world.registry().get<lw::Group>(group) : nullptr;
}

// Человек с лидерством и его взаимный друг собирают группу; база — дом вожака,
// друг переезжает туда. Друг вожака вступает следом.
void testGroupForms() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity a = field.people[0];
    const entt::entity b = field.people[1];
    const entt::entity c = field.people[2];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Traits>(person) = {};
        registry.get<lw::Traits>(person).leadership = 0.3f;
    }
    registry.get<lw::Traits>(a).leadership = 0.8f;
    const lw::EntityId homeA = registry.get<lw::Person>(a).home;
    CHECK(homeA != lw::kInvalidId);
    if (homeA == lw::kInvalidId) {
        return;
    }
    const lw::EntityId homeB = nearbyHome(field.world, homeA, 1000.0f);
    CHECK(homeB != lw::kInvalidId);
    registry.get<lw::Person>(b).home = homeB;
    registry.get<lw::Person>(c).home = homeB;
    // Знакомы, но не друзья — группы нет.
    setBond(registry, a, field.id(b), 0.9f, 0.3f, 0.5f);
    setBond(registry, b, field.id(a), 0.9f, 0.3f, 0.5f);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, a) == nullptr);

    befriend(registry, field, a, b);
    lw::updateGroups(field.world);
    const lw::Group* group = groupOf(field.world, a);
    CHECK(group != nullptr && groupOf(field.world, b) == group);
    if (group == nullptr) {
        return;
    }
    CHECK(group->leader == field.id(a) && group->base == homeA && group->members.size() == 2);
    CHECK(registry.get<lw::Person>(b).home == homeA);
    CHECK(registry.get<lw::Person>(b).formerHome == homeB);
    CHECK(registry.get<lw::Person>(a).formerHome == lw::kInvalidId);
    CHECK(field.count(lw::ChronicleKind::GroupFormed) == 1);
    CHECK(field.count(lw::ChronicleKind::JoinedGroup) == 1);

    befriend(registry, field, a, c, 0.4f);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, c) == group && group->members.size() == 3);
    CHECK(registry.get<lw::Person>(c).home == homeA);
    // Один seed — одни и те же группы.
    CHECK(field.world.stateHash() == field.world.stateHash());
}

// Кому вожак стал неприятен — уходит домой; кого вожак невзлюбил — изгнан и
// держит обиду; остался один — группа распалась.
void testGroupLeaveAndExpel() {
    Field field(4);
    if (!field.open || field.people.size() != 4) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity leader = field.people[0];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
        registry.get<lw::Traits>(person) = {};
        registry.get<lw::Traits>(person).leadership = 0.3f;
    }
    registry.get<lw::Traits>(leader).leadership = 0.8f;
    const lw::EntityId base = registry.get<lw::Person>(leader).home;
    const lw::EntityId other = base != lw::kInvalidId ? nearbyHome(field.world, base, 1000.0f)
                                                      : lw::kInvalidId;
    CHECK(other != lw::kInvalidId);
    if (other == lw::kInvalidId) {
        return;
    }
    for (std::size_t i = 1; i < field.people.size(); ++i) {
        registry.get<lw::Person>(field.people[i]).home = other;
        befriend(registry, field, leader, field.people[i]);
    }
    lw::updateGroups(field.world);
    const lw::Group* group = groupOf(field.world, leader);
    CHECK(group != nullptr && group->members.size() == 4);
    if (group == nullptr || group->members.size() != 4) {
        return;
    }
    const lw::EntityId groupId = registry.get<lw::Person>(leader).group;
    const entt::entity quitter = field.people[1];
    const entt::entity outcast = field.people[2];
    const entt::entity last = field.people[3];

    // Вожак стал неприятен — ушёл и вернулся в свой дом.
    setBond(registry, quitter, field.id(leader), 0.9f, -0.5f, -0.2f);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, quitter) == nullptr);
    CHECK(registry.get<lw::Person>(quitter).home == other);
    CHECK(field.count(lw::ChronicleKind::LeftGroup) == 1);

    // Вожак невзлюбил — изгнан, помнит и держит обиду.
    setBond(registry, leader, field.id(outcast), 0.9f, -0.6f, -0.3f);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, outcast) == nullptr);
    CHECK(registry.get<lw::Person>(outcast).home == other);
    CHECK(field.count(lw::ChronicleKind::Expelled) == 1);
    const lw::Memory* expelled = registry.get<lw::Memories>(outcast).find(
        lw::MemoryKind::Expelled, field.id(leader));
    CHECK(expelled != nullptr && expelled->about == groupId);
    const lw::Bond* grudge = registry.get<lw::Relations>(outcast).find(field.id(leader));
    CHECK(grudge != nullptr && grudge->grudge > 0.2f);
    // Изгнанного обратно не берут.
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, outcast) == nullptr);

    // Последний товарищ погиб — группа распалась, вожак живёт где жил.
    lw::die(field.world, last, lw::DeathCause::Starvation, lw::kInvalidId);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, leader) == nullptr);
    CHECK(field.world.find(groupId) == entt::null);
    CHECK(registry.get<lw::Person>(leader).home == base);
    CHECK(field.count(lw::ChronicleKind::GroupDisbanded) == 1);
}

// Вожак погиб — группу ведёт самый авторитетный; кто заметно авторитетнее
// вожака — смещает его.
void testGroupLeadership() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity founder = field.people[0];
    const entt::entity heir = field.people[1];
    const entt::entity rival = field.people[2];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Traits>(person) = {};
    }
    registry.get<lw::Traits>(founder).leadership = 0.9f;
    registry.get<lw::Traits>(heir).leadership = 0.6f;
    registry.get<lw::Traits>(rival).leadership = 0.3f;
    const lw::EntityId base = registry.get<lw::Person>(founder).home;
    registry.get<lw::Person>(heir).home = base;
    registry.get<lw::Person>(rival).home = base;
    befriend(registry, field, founder, heir);
    befriend(registry, field, founder, rival);
    befriend(registry, field, heir, rival);
    lw::updateGroups(field.world);
    const lw::Group* group = groupOf(field.world, founder);
    CHECK(group != nullptr && group->members.size() == 3 && group->leader == field.id(founder));
    if (group == nullptr) {
        return;
    }
    const lw::EntityId groupId = registry.get<lw::Person>(founder).group;
    lw::die(field.world, founder, lw::DeathCause::Starvation, lw::kInvalidId);
    lw::updateGroups(field.world);
    const lw::Group* left = groupOf(field.world, heir);
    CHECK(left != nullptr && left->leader == field.id(heir) && left->members.size() == 2);
    const auto& events = field.world.chronicle().events();
    CHECK(std::any_of(events.begin(), events.end(), [&](const lw::ChronicleEvent& e) {
        return e.kind == lw::ChronicleKind::LedGroup && e.a == 0 && e.subject == field.id(heir) &&
               e.third == field.id(founder) && e.object == groupId;
    }));
    // Соперник стал намного авторитетнее — сместил.
    registry.get<lw::Traits>(rival).leadership = 1.0f;
    lw::updateGroups(field.world);
    CHECK(left != nullptr && left->leader == field.id(rival));
    CHECK(std::any_of(events.begin(), events.end(), [&](const lw::ChronicleEvent& e) {
        return e.kind == lw::ChronicleKind::LedGroup && e.a == 1 && e.subject == field.id(rival) &&
               e.third == field.id(heir);
    }));
}

// Зомби напал на товарища по группе — драться с ним хочется сильнее.
void testDefendGroupMate() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity guard = field.people[0];
    const entt::entity mate = field.people[1];
    field.freeze(guard, 0);
    field.freeze(mate, 6);
    registry.get<lw::Relations>(guard) = {};
    registry.get<lw::Traits>(guard) = {};
    registry.get<lw::Needs>(guard) = {};
    registry.get<lw::Weapon>(guard) = {lw::WeaponKind::Axe, 0};
    const entt::entity zombie = field.zombie(7);
    lw::Zombie& state = registry.get<lw::Zombie>(zombie);
    state.state = lw::ZombieState::Chase;
    state.target = field.id(mate);
    registry.get<lw::Awareness>(guard) = {};
    letSee(field, guard, zombie);
    const auto fight = static_cast<std::size_t>(lw::Action::Fight);
    const float alone = lw::evaluateActions(field.world, guard)[fight];
    CHECK(alone > 0.0f);

    const entt::entity entity = field.world.create();
    lw::Group& group = registry.emplace<lw::Group>(entity);
    group.leader = field.id(guard);
    group.members = {field.id(guard), field.id(mate)};
    std::sort(group.members.begin(), group.members.end());
    registry.get<lw::Person>(guard).group = field.id(entity);
    registry.get<lw::Person>(mate).group = field.id(entity);
    const float together = lw::evaluateActions(field.world, guard)[fight];
    CHECK(together > alone * 1.3f);
}

// --- Социальные действия (этап 6.5) --------------------------------------------

// Решить заново прямо сейчас (Utility AI в следующем тике).
const lw::Mind& decideNow(Field& field, entt::entity person) {
    lw::Mind& mind = field.world.registry().get<lw::Mind>(person);
    mind.action = lw::Action::Idle;
    mind.phase = lw::ActionPhase::Perform;
    mind.until = field.world.tick();
    mind.nextThink = field.world.tick();
    field.world.step();
    return field.world.registry().get<lw::Mind>(person);
}

// Человек без нужд и обычного характера, стоит в клетке ряда.
void calm(Field& field, entt::entity person, int offset) {
    entt::registry& registry = field.world.registry();
    field.place(person, offset);
    registry.get<lw::Person>(person).asleep = false;
    registry.get<lw::Needs>(person) = {};
    registry.get<lw::Traits>(person) = {};
    registry.get<lw::Relations>(person) = {};
    registry.get<lw::Memories>(person) = {};
    registry.get<lw::Inventory>(person) = {};
    registry.get<lw::Wounds>(person) = {};
}

// Друг голодает, а у него ни при себе, ни дома нет еды, — отнести ему: он
// получит еду и будет обязан.
void testShareFood() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity giver = field.people[0];
    const entt::entity hungry = field.people[1];
    calm(field, giver, 0);
    field.freeze(hungry, 10);
    registry.get<lw::Relations>(hungry) = {};
    registry.get<lw::Memories>(hungry) = {};
    registry.get<lw::Inventory>(hungry) = {};
    registry.get<lw::Needs>(hungry).hunger = 0.9f;
    if (const entt::entity home = field.world.find(registry.get<lw::Person>(hungry).home);
        home != entt::null) {
        registry.get<lw::Inventory>(home).set(lw::ItemKind::Food, 0);
        registry.get<lw::Inventory>(home).set(lw::ItemKind::Meat, 0);
    }
    registry.get<lw::Inventory>(giver).set(lw::ItemKind::Food, 3);
    setBond(registry, giver, field.id(hungry), 0.9f, 0.8f, 0.6f, 0.8f);
    const lw::Mind& mind = decideNow(field, giver);
    CHECK(mind.action == lw::Action::Share && mind.target == field.id(hungry));
    for (int second = 0; second < 120 && field.count(lw::ChronicleKind::Shared) == 0; ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(field.count(lw::ChronicleKind::Shared) == 1);
    CHECK(registry.get<lw::Inventory>(hungry).food() == 2);
    CHECK(registry.get<lw::Inventory>(giver).food() == 1);
    const lw::Memory* helped =
        registry.get<lw::Memories>(hungry).find(lw::MemoryKind::Helped, field.id(giver));
    CHECK(helped != nullptr);
    const lw::Bond* debt = registry.get<lw::Relations>(hungry).find(field.id(giver));
    CHECK(debt != nullptr && debt->debt > 0.2f);
}

// Плохо и нет лекарства, а у знакомого лишнее, — обменять на него свою еду.
void testTrade() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity sick = field.people[0];
    const entt::entity medic = field.people[1];
    calm(field, sick, 0);
    field.freeze(medic, 10);
    registry.get<lw::Inventory>(medic) = {};
    registry.get<lw::Inventory>(medic).set(lw::ItemKind::Medicine, 2);
    registry.get<lw::Needs>(sick).health = 0.4f;
    registry.get<lw::Inventory>(sick).set(lw::ItemKind::Food, 6);
    setBond(registry, sick, field.id(medic), 0.8f, 0.4f, 1.0f, 0.4f);
    setBond(registry, medic, field.id(sick), 0.8f, 0.4f, 0.5f, 0.4f);
    const float trustBefore = registry.get<lw::Relations>(medic).find(field.id(sick))->trust;
    const lw::Mind& mind = decideNow(field, sick);
    CHECK(mind.action == lw::Action::Trade && mind.target == field.id(medic));
    for (int second = 0; second < 200 && field.count(lw::ChronicleKind::Traded) == 0; ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(field.count(lw::ChronicleKind::Traded) == 1);
    // Лекарство — 3 порции еды.
    CHECK(registry.get<lw::Inventory>(medic).count(lw::ItemKind::Medicine) == 1);
    CHECK(registry.get<lw::Inventory>(medic).count(lw::ItemKind::Food) == 3);
    CHECK(registry.get<lw::Inventory>(sick).count(lw::ItemKind::Food) == 3);
    CHECK(registry.get<lw::Inventory>(sick).count(lw::ItemKind::Medicine) == 1 ||
          field.count(lw::ChronicleKind::Treated) == 1);
    CHECK(registry.get<lw::Relations>(medic).find(field.id(sick))->trust > trustBefore);
}

// Предательство: жадный и голодный обворовывает дом того, кто ему доверяет;
// хозяин, застав его, считает его предателем. Порядочный так не поступит.
void testBetrayal() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity thief = field.people[0];
    const entt::entity owner = field.people[1];
    calm(field, thief, 0);
    registry.get<lw::Person>(thief).home = lw::kInvalidId;
    registry.get<lw::Traits>(thief).greed = 1.0f;
    registry.get<lw::Traits>(thief).altruism = 0.0f;
    registry.get<lw::Needs>(thief).hunger = 0.9f;
    const lw::EntityId home = registry.get<lw::Person>(owner).home;
    CHECK(home != lw::kInvalidId);
    if (home == lw::kInvalidId) {
        return;
    }
    field.freeze(owner, 20);
    registry.get<lw::Relations>(owner) = {};
    registry.get<lw::Memories>(owner) = {};
    lw::Inventory& stock = registry.get<lw::Inventory>(field.world.find(home));
    stock.set(lw::ItemKind::Food, 10);
    setBond(registry, thief, field.id(owner), 0.3f, 0.1f, 0.1f);
    setBond(registry, owner, field.id(thief), 0.6f, 0.4f, 0.5f);
    const auto steal = static_cast<std::size_t>(lw::Action::Steal);
    CHECK(lw::evaluateActions(field.world, thief)[steal] > 0.1f);
    // Хозяину не доверяют — не предательство; порядочный не крадёт.
    registry.get<lw::Relations>(owner).find(field.id(thief))->trust = 0.0f;
    CHECK(lw::evaluateActions(field.world, thief)[steal] == 0.0f);
    registry.get<lw::Relations>(owner).find(field.id(thief))->trust = 0.5f;
    registry.get<lw::Traits>(thief).altruism = 0.9f;
    registry.get<lw::Traits>(thief).greed = 0.3f;
    CHECK(lw::evaluateActions(field.world, thief)[steal] == 0.0f);

    // Кража: хозяин дома и не спит — застал.
    registry.get<lw::Person>(owner).indoors = true;
    lw::Mind& mind = registry.get<lw::Mind>(thief);
    mind.action = lw::Action::Steal;
    mind.phase = lw::ActionPhase::Perform;
    mind.target = home;
    mind.spot = field.at(0);
    mind.until = field.world.tick();
    mind.nextThink = field.world.tick() + 1000;
    field.world.step();
    CHECK(field.count(lw::ChronicleKind::Stole) == 1);
    CHECK(registry.get<lw::Inventory>(thief).food() >= 6);
    CHECK(stock.food() <= 4);
    const lw::Memory* betrayed =
        registry.get<lw::Memories>(owner).find(lw::MemoryKind::Betrayed, field.id(thief));
    CHECK(betrayed != nullptr && betrayed->about == home);
    CHECK(field.count(lw::ChronicleKind::BetrayedBy) == 1);
    const lw::Bond* grudge = registry.get<lw::Relations>(owner).find(field.id(thief));
    CHECK(grudge != nullptr && grudge->grudge > 0.4f && grudge->trust < 0.0f);
}

// Сбежать из группы с запасами базы: вожак не мил, жадный. Товарищи считают
// беглеца предателем, а он возвращается в свой прежний дом.
void testDesertion() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity leader = field.people[0];
    const entt::entity deserter = field.people[1];
    const entt::entity loyal = field.people[2];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
        registry.get<lw::Traits>(person) = {};
        registry.get<lw::Traits>(person).leadership = 0.3f;
    }
    registry.get<lw::Traits>(leader).leadership = 0.9f;
    const lw::EntityId base = registry.get<lw::Person>(leader).home;
    const lw::EntityId own = base != lw::kInvalidId ? nearbyHome(field.world, base, 1000.0f)
                                                    : lw::kInvalidId;
    CHECK(own != lw::kInvalidId);
    if (own == lw::kInvalidId) {
        return;
    }
    registry.get<lw::Person>(deserter).home = own;
    registry.get<lw::Person>(loyal).home = base;
    befriend(registry, field, leader, deserter);
    befriend(registry, field, leader, loyal);
    lw::updateGroups(field.world);
    const lw::Group* group = groupOf(field.world, leader);
    CHECK(group != nullptr && group->members.size() == 3);
    CHECK(registry.get<lw::Person>(deserter).home == base);
    if (group == nullptr || group->members.size() != 3) {
        return;
    }
    const lw::EntityId groupId = registry.get<lw::Person>(leader).group;
    // Вожак стал не мил, а на базе — запасы.
    calm(field, deserter, 0);
    registry.get<lw::Traits>(deserter).greed = 1.0f;
    registry.get<lw::Traits>(deserter).altruism = 0.0f;
    setBond(registry, deserter, field.id(leader), 0.9f, 0.0f, 0.2f);
    registry.get<lw::Inventory>(field.world.find(base)).set(lw::ItemKind::Food, 12);
    const auto steal = static_cast<std::size_t>(lw::Action::Steal);
    CHECK(lw::evaluateActions(field.world, deserter)[steal] > 0.0f);
    lw::Mind& mind = registry.get<lw::Mind>(deserter);
    mind.action = lw::Action::Steal;
    mind.phase = lw::ActionPhase::Perform;
    mind.target = base;
    mind.spot = field.at(0);
    mind.until = field.world.tick();
    mind.nextThink = field.world.tick() + 1000;
    field.world.step();
    CHECK(field.count(lw::ChronicleKind::Deserted) == 1);
    CHECK(registry.get<lw::Person>(deserter).group == lw::kInvalidId);
    CHECK(registry.get<lw::Person>(deserter).home == own);
    CHECK(registry.get<lw::Inventory>(deserter).food() >= 6);
    for (const entt::entity mate : {leader, loyal}) {
        const lw::Memory* memory =
            registry.get<lw::Memories>(mate).find(lw::MemoryKind::Betrayed, field.id(deserter));
        CHECK(memory != nullptr && memory->about == groupId);
    }
    CHECK(group->members.size() == 2);
}

// Месть: сильная обида — найти обидчика и избить; тот запомнит нападение. Кто
// хочет убить — бьёт до смерти.
void testAvenge() {
    Field field(2);
    if (!field.open || field.people.size() != 2) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity avenger = field.people[0];
    const entt::entity offender = field.people[1];
    calm(field, avenger, 0);
    registry.get<lw::Traits>(avenger).aggression = 0.0f;
    registry.get<lw::Traits>(avenger).altruism = 0.0f;
    registry.get<lw::Traits>(avenger).bravery = 1.0f;
    registry.get<lw::Weapon>(avenger) = {lw::WeaponKind::Axe, 0};
    field.freeze(offender, 5);
    registry.get<lw::Relations>(offender) = {};
    registry.get<lw::Memories>(offender) = {};
    registry.get<lw::Weapon>(offender) = {lw::WeaponKind::Fists, 0};
    lw::Bond* grudge = setBond(registry, avenger, field.id(offender), 0.6f, -0.6f, -0.5f);
    CHECK(grudge != nullptr);
    if (grudge == nullptr) {
        return;
    }
    grudge->grudge = 0.9f;
    const lw::Mind& mind = decideNow(field, avenger);
    CHECK(mind.action == lw::Action::Avenge && mind.target == field.id(offender));
    for (int second = 0; second < 120 && field.count(lw::ChronicleKind::Avenged) == 0; ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(field.count(lw::ChronicleKind::Avenged) == 1);
    CHECK(registry.all_of<lw::Person>(offender)); // избил, но не убил
    CHECK(registry.get<lw::Needs>(offender).health <= 0.5f);
    CHECK(registry.get<lw::Memories>(offender).find(lw::MemoryKind::Assaulted,
                                                     field.id(avenger)) != nullptr);
    CHECK(field.count(lw::ChronicleKind::AssaultedBy) == 1);
    CHECK(grudge->grudge < 0.4f);
    CHECK(registry.get<lw::Combat>(avenger).target == lw::kInvalidId);

    // Хочет убить — бьёт до смерти.
    registry.get<lw::Needs>(offender).health = 1.0f;
    registry.get<lw::Traits>(avenger).aggression = 1.0f;
    grudge->grudge = 1.0f;
    const lw::Mind& again = decideNow(field, avenger);
    CHECK(again.action == lw::Action::Avenge);
    for (int second = 0; second < 120 && field.count(lw::ChronicleKind::Avenged) == 1; ++second) {
        field.steps(lw::kTicksPerSecond);
    }
    CHECK(field.count(lw::ChronicleKind::Avenged) == 2);
    const auto* corpse = registry.try_get<lw::Corpse>(offender);
    CHECK(corpse != nullptr && corpse->killer == field.id(avenger));
}

// Человек, который бьёт, — угроза: для жертвы (от него и дома не спрятаться) и
// для её товарища; убийство на глазах близкого — горе и обида на убийцу.
void testPersonThreat() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity attacker = field.people[0];
    const entt::entity victim = field.people[1];
    const entt::entity friendOf = field.people[2];
    field.freeze(attacker, 0);
    field.freeze(victim, 1);
    field.freeze(friendOf, 8);
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
        registry.get<lw::Awareness>(person) = {};
    }
    registry.get<lw::Combat>(attacker).target = field.id(victim);
    letSee(field, victim, attacker);
    letSee(field, friendOf, attacker);
    const lw::Threat threat = lw::threatOf(field.world, victim);
    CHECK(threat.assailant == field.id(attacker) && threat.level >= 0.8f);
    // Чужому — не угроза; другу жертвы — угроза.
    CHECK(lw::threatOf(field.world, friendOf).level == 0.0f);
    setBond(registry, friendOf, field.id(victim), 0.9f, 0.8f, 0.6f);
    CHECK(lw::threatOf(field.world, friendOf).level > 0.25f);

    // Убийство на глазах у друга: горе и сильная обида на убийцу.
    lw::witnessKilling(field.world, attacker, victim);
    const lw::Memory* killed = registry.get<lw::Memories>(friendOf).find(
        lw::MemoryKind::Killed, field.id(attacker), field.id(victim));
    CHECK(killed != nullptr);
    CHECK(registry.get<lw::Memories>(friendOf).find(lw::MemoryKind::Bereaved, field.id(victim)) !=
          nullptr);
    const lw::Bond* hatred = registry.get<lw::Relations>(friendOf).find(field.id(attacker));
    CHECK(hatred != nullptr && hatred->grudge > 0.4f);
    CHECK(field.count(lw::ChronicleKind::Witnessed) == 1);

    // Ударил того, кто бил другого, — заступился за того.
    registry.get<lw::Memories>(victim) = {};
    lw::witnessAssault(field.world, friendOf, attacker);
    CHECK(registry.get<lw::Memories>(victim).find(lw::MemoryKind::Defended, field.id(friendOf)) !=
          nullptr);
    CHECK(registry.get<lw::Memories>(attacker).find(lw::MemoryKind::Assaulted,
                                                     field.id(friendOf)) == nullptr);
}

// --- Ивенты (этап 7.1) -----------------------------------------------------------

// Ивент: место — открытая суша, грузы с припасами, гул и запись в летописи;
// один seed — один и тот же ивент, а ивент меняет будущее мира.
void testEventStarts() {
    lw::World a(7, regionConfig());
    lw::World b(7, regionConfig());
    const lw::World untouched(7, regionConfig());
    const lw::EntityId id = lw::startEvent(a, lw::EventKind::SupplyDrop);
    CHECK(id != lw::kInvalidId);
    CHECK(lw::startEvent(b, lw::EventKind::SupplyDrop) == id);
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.stateHash() != untouched.stateHash());
    const entt::entity entity = a.find(id);
    CHECK(entity != entt::null);
    if (entity == entt::null) {
        return;
    }
    const entt::registry& registry = a.registry();
    const lw::WorldEvent& event = registry.get<lw::WorldEvent>(entity);
    const lw::Map& map = a.map();
    CHECK(map.water.at(event.site.x, event.site.y) == lw::Water::None);
    CHECK(map.cover.at(event.site.x, event.site.y) == lw::Cover::None);
    CHECK(a.nav().passable(event.site));
    CHECK(event.signalUntil == event.began + 6 * lw::kTicksPerGameHour);
    CHECK(event.caches.size() >= 1 && event.caches.size() <= 2);
    for (const lw::EntityId cache : event.caches) {
        const entt::entity crate = a.find(cache);
        CHECK(crate != entt::null && registry.all_of<lw::Position>(crate));
        if (crate == entt::null) {
            continue;
        }
        CHECK(registry.get<lw::Cache>(crate).event == id);
        const lw::Inventory& stock = registry.get<lw::Inventory>(crate);
        CHECK(stock.food() >= 6 && stock.count(lw::ItemKind::Water) >= 5);
        CHECK(stock.count(lw::ItemKind::Bandage) >= 2 && stock.count(lw::ItemKind::Medicine) >= 1);
    }
    CHECK(a.chronicle().countOf(lw::ChronicleKind::EventBegan) == 1);
    CHECK(!a.sounds().empty() && a.sounds().back().kind == lw::SoundKind::Aircraft &&
          a.sounds().back().source == id);
    for (int i = 0; i < 2000; ++i) {
        a.step();
        b.step();
    }
    CHECK(a.stateHash() == b.stateHash());
}

// Гул слышно в округе: человек узнаёт о грузе, идёт к нему и забирает
// припасы; второй раз туда не пойдёт.
void testEventSalvaged() {
    Lone lone;
    lw::World& world = lone.world;
    entt::registry& registry = world.registry();
    if (lone.home == entt::null) {
        return;
    }
    registry.get<lw::Inventory>(lone.person) = {};
    const lw::Cell door = registry.get<lw::Building>(lone.home).door;
    const lw::EntityId id =
        lw::startEvent(world, lw::EventKind::SupplyDrop, lw::Cell{door.x + 25, door.y});
    CHECK(id != lw::kInvalidId);
    const lw::Leads& leads = registry.get<lw::Leads>(lone.person);
    const lw::Lead* lead = leads.find(id);
    CHECK(lead != nullptr && lead->source == lw::kInvalidId && !lead->visited);
    const auto& events = world.chronicle().events();
    CHECK(std::any_of(events.begin(), events.end(), [&](const lw::ChronicleEvent& event) {
        return event.kind == lw::ChronicleKind::LearnedOfEvent && event.object == id &&
               event.a == static_cast<std::int32_t>(lw::EventClue::Heard);
    }));
    bool salvaged = false;
    for (int minute = 0; minute < 6 * 60 && !salvaged; minute += 10) {
        lone.run(10);
        salvaged = world.chronicle().countOf(lw::ChronicleKind::Salvaged) > 0;
    }
    CHECK(salvaged);
    const auto took = std::find_if(events.begin(), events.end(), [](const lw::ChronicleEvent& event) {
        return event.kind == lw::ChronicleKind::Salvaged;
    });
    CHECK(took != events.end() && took->a == 3 && took->b == 2);
    const entt::entity crate = took != events.end() ? world.find(took->object) : entt::null;
    CHECK(crate != entt::null && registry.all_of<lw::Cache>(crate));
    lead = leads.find(id);
    CHECK(lead != nullptr && lead->visited && !lead->empty);
    CHECK(lw::evaluateActions(world, lone.person)[static_cast<std::size_t>(lw::Action::Salvage)] ==
          0.0f);
}

// Гула не слышно, но дым днём видно издалека: заметивший узнаёт о месте.
void testEventSmokeSeen() {
    Field field(1);
    if (!field.open || field.people.size() != 1) {
        return;
    }
    lw::World& world = field.world;
    entt::registry& registry = world.registry();
    const entt::entity person = field.people[0];
    field.freeze(person, 0);
    // Суша в 1,9 км (475 клеток) — в какую сторону получится.
    std::optional<lw::Cell> far;
    constexpr int kOffsets[][2] = {{475, 0}, {-475, 0}, {0, 475}, {0, -475},
                                   {336, 336}, {-336, 336}, {336, -336}, {-336, -336}};
    for (const auto& offset : kOffsets) {
        const lw::Cell cell{field.row.x + offset[0], field.row.y + offset[1]};
        if (!far && world.map().contains(cell) && world.nav().passable(cell) &&
            world.map().water.at(cell.x, cell.y) == lw::Water::None) {
            far = cell;
        }
    }
    CHECK(far.has_value());
    if (!far) {
        return;
    }
    const lw::EntityId id = lw::startEvent(world, lw::EventKind::SupplyDrop, *far);
    const entt::entity entity = world.find(id);
    CHECK(entity != entt::null);
    if (entity == entt::null) {
        return;
    }
    const lw::Cell site = registry.get<lw::WorldEvent>(entity).site;
    const lw::Position& at = registry.get<lw::Position>(person);
    const float d = std::hypot(lw::Map::cellCenter(site.x) - at.x, lw::Map::cellCenter(site.y) - at.y);
    CHECK(d > 1600.0f && d < 2300.0f);
    CHECK(registry.get<lw::Leads>(person).find(id) == nullptr); // не слышал
    field.steps(60 * lw::kTicksPerGameMinute);
    const lw::Lead* lead = registry.get<lw::Leads>(person).find(id);
    CHECK(lead != nullptr && lead->source == lw::kInvalidId);
    const auto& events = world.chronicle().events();
    CHECK(std::any_of(events.begin(), events.end(), [&](const lw::ChronicleEvent& event) {
        return event.kind == lw::ChronicleKind::LearnedOfEvent &&
               event.a == static_cast<std::int32_t>(lw::EventClue::Saw);
    }));
}

// Нежадный рассказывает о грузе тому, кто рядом; из вторых рук весть дальше не
// идёт. Побывавший там, где пусто, говорит об этом тому, кто туда собирается.
void testLeadSpreads() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    lw::World& world = field.world;
    entt::registry& registry = world.registry();
    const entt::entity teller = field.people[0];
    const entt::entity listener = field.people[1];
    const entt::entity stranger = field.people[2];
    for (const entt::entity person : field.people) {
        calm(field, person, 0);
        registry.get<lw::Leads>(person) = {};
    }
    field.freeze(teller, 0);
    field.freeze(listener, 2);
    field.dismiss(stranger);
    registry.get<lw::Traits>(teller).greed = 0.2f; // нежадный: скажет и незнакомцу
    constexpr lw::EntityId kEvent = 999999;        // ивента нет: весть ничего не проверяет
    CHECK(lw::learnOf(world, teller, kEvent, field.at(20), lw::EventClue::Saw));
    CHECK(!lw::learnOf(world, teller, kEvent, field.at(20), lw::EventClue::Saw)); // уже знал
    field.steps(10 * lw::kTicksPerGameMinute);
    const lw::Lead* own = registry.get<lw::Leads>(teller).find(kEvent);
    const lw::Lead* told = registry.get<lw::Leads>(listener).find(kEvent);
    CHECK(told != nullptr && told->source == field.id(teller));
    CHECK(told != nullptr && own != nullptr && told->since == own->since);
    CHECK(lw::leadToShare(world, listener, stranger) == nullptr);
    // Жадный незнакомцу не скажет.
    CHECK(lw::leadToShare(world, teller, stranger) != nullptr);
    registry.get<lw::Traits>(teller).greed = 0.9f;
    CHECK(lw::leadToShare(world, teller, stranger) == nullptr);

    lw::markVisited(registry.get<lw::Leads>(teller), kEvent, true);
    field.steps(10 * lw::kTicksPerGameMinute);
    told = registry.get<lw::Leads>(listener).find(kEvent);
    CHECK(told != nullptr && told->visited && told->empty);
}

// Знак погас, грузы пусты — ивент кончается, пустые грузы исчезают; груз с
// добром переживает ивент.
void testEventEnds() {
    lw::World world(7, regionConfig());
    entt::registry& registry = world.registry();
    const lw::EntityId id = lw::startEvent(world, lw::EventKind::SupplyDrop);
    const entt::entity entity = world.find(id);
    CHECK(entity != entt::null);
    if (entity == entt::null) {
        return;
    }
    const lw::WorldEvent event = registry.get<lw::WorldEvent>(entity);
    for (const lw::EntityId cache : event.caches) {
        registry.get<lw::Inventory>(world.find(cache)) = {};
    }
    while (world.tick() <= event.signalUntil + lw::kTicksPerGameMinute) {
        world.step();
    }
    CHECK(world.find(id) == entt::null);
    for (const lw::EntityId cache : event.caches) {
        CHECK(world.find(cache) == entt::null);
    }
    CHECK(world.chronicle().countOf(lw::ChronicleKind::EventEnded) == 1);

    const lw::EntityId second = lw::startEvent(world, lw::EventKind::SupplyDrop);
    const lw::WorldEvent kept = registry.get<lw::WorldEvent>(world.find(second));
    while (world.tick() <= kept.began + lw::kEventMaxTicks + lw::kTicksPerGameMinute) {
        world.step();
    }
    CHECK(world.find(second) == entt::null);
    for (const lw::EntityId cache : kept.caches) {
        const entt::entity crate = world.find(cache);
        CHECK(crate != entt::null && !registry.get<lw::Inventory>(crate).empty());
    }
    const lw::ChronicleEvent& ended = world.chronicle().events().back();
    CHECK(ended.kind == lw::ChronicleKind::EventEnded && ended.subject == second &&
          ended.a == static_cast<std::int32_t>(kept.caches.size()));
}

// --- Мирная жизнь (этап 7.2) -------------------------------------------------

// Мир 4 × 4 км с мирной жизнью: профессии, деньги, магазины, водопровод.
lw::WorldConfig peaceConfig(int people) {
    lw::WorldConfig config = regionConfig();
    config.people = people;
    config.peacetime = true;
    config.outbreakDays = 0.0; // вспышку (этап 7.4) начинают тесты сценария
    // Правила мирной жизни и фаз — тик в тик («решил сейчас»): на полном
    // уровне детализации (LOD — в своих тестах, этап 6а).
    config.lod = false;
    return config;
}

// Живой работник этой профессии с наименьшим id; entt::null — нет.
entt::entity workerOf(const lw::World& world, lw::Profession profession) {
    entt::entity best = entt::null;
    lw::EntityId bestId = lw::kInvalidId;
    for (const auto [entity, id, person, job] :
         world.registry().view<const lw::Id, const lw::Person, const lw::Job>().each()) {
        if (job.profession == profession && (best == entt::null || id.value < bestId)) {
            best = entity;
            bestId = id.value;
        }
    }
    return best;
}

// Прокрутить мир на столько игровых минут.
void runMinutes(lw::World& world, int gameMinutes) {
    const std::uint64_t end = world.tick() + static_cast<std::uint64_t>(gameMinutes) * lw::kTicksPerGameMinute;
    while (world.tick() < end) {
        world.step();
    }
}

// Прокрутить мир до ближайшего времени суток hour:minute.
void runUntil(lw::World& world, int hour, int minute) {
    const std::uint64_t end = lw::nextTimeOfDay(world.tick(), hour, minute);
    while (world.tick() < end) {
        world.step();
    }
}

// Пересмотреть решение в следующем тике (как decideNow, но без Field).
const lw::Mind& thinkNow(lw::World& world, entt::entity person) {
    lw::Mind& mind = world.registry().get<lw::Mind>(person);
    mind.action = lw::Action::Idle;
    mind.phase = lw::ActionPhase::Perform;
    mind.until = world.tick();
    mind.nextThink = world.tick();
    world.step();
    return world.registry().get<lw::Mind>(person);
}

// Безработный житель поселения с открытым зданием вида kind; entt::null — нет.
entt::entity idlerNear(const lw::World& world, lw::BuildingKind kind) {
    const entt::registry& registry = world.registry();
    std::vector<std::pair<lw::EntityId, entt::entity>> idlers;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        const auto* job = registry.try_get<lw::Job>(entity);
        if ((job == nullptr || job->profession == lw::Profession::None) && person.home != lw::kInvalidId) {
            idlers.emplace_back(id.value, entity);
        }
    }
    std::sort(idlers.begin(), idlers.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, entity] : idlers) {
        const lw::EntityId settlement =
            registry.get<lw::Building>(world.find(registry.get<lw::Person>(entity).home)).settlement;
        for (const lw::Places::BuildingPlace& building : world.places().settlement(settlement)->buildings) {
            if (building.kind == kind && lw::isOpen(world, building.id)) {
                return entity;
            }
        }
    }
    return entt::null;
}

// Мир начинается с мирной жизни: зомби нет, в городе больница, полиция и
// школа, в деревне — магазин; у людей профессии в своём поселении и деньги.
void testPeacefulStart() {
    const lw::WorldConfig defaults;
    CHECK(defaults.zombies == 0 && defaults.peacetime && !defaults.events &&
          defaults.outbreakDays > 0.0);
    const lw::World world(7, peaceConfig(150));
    const lw::World twin(7, peaceConfig(150));
    CHECK(world.stateHash() == twin.stateHash());
    const entt::registry& registry = world.registry();
    CHECK(registry.view<const lw::Zombie>().empty());

    bool town = false;
    for (const lw::Places::SettlementPlace& place : world.places().settlements()) {
        int kinds[7] = {};
        for (const lw::Places::BuildingPlace& building : place.buildings) {
            ++kinds[static_cast<std::size_t>(building.kind)];
        }
        const int hospitals = kinds[static_cast<std::size_t>(lw::BuildingKind::Hospital)];
        const int police = kinds[static_cast<std::size_t>(lw::BuildingKind::PoliceStation)];
        const int schools = kinds[static_cast<std::size_t>(lw::BuildingKind::School)];
        if (registry.get<lw::Settlement>(world.find(place.id)).kind == lw::SettlementKind::Town) {
            town = true;
            CHECK(hospitals == 1 && police == 1 && schools == 1);
        } else {
            CHECK(hospitals == 0 && police == 0 && schools == 0);
            const int shops = kinds[static_cast<std::size_t>(lw::BuildingKind::Shop)];
            CHECK(shops <= 1);
            CHECK(shops == 1 || kinds[static_cast<std::size_t>(lw::BuildingKind::House)] < 4);
        }
    }
    CHECK(town);

    int employed = 0;
    int pupils = 0;
    for (const auto [entity, person, job] : registry.view<const lw::Person, const lw::Job>().each()) {
        // Работают взрослые, учатся дети (этап 7.3).
        const int age = lw::ageOf(registry, entity);
        if (job.profession == lw::Profession::Pupil) {
            ++pupils;
            CHECK(age < lw::kAdultAge);
        } else {
            ++employed;
            CHECK(age >= lw::kAdultAge);
        }
        const entt::entity workplace = world.find(job.workplace);
        CHECK(workplace != entt::null);
        if (workplace == entt::null) {
            continue;
        }
        const lw::Building& building = registry.get<lw::Building>(workplace);
        CHECK(building.kind == lw::professionSpec(job.profession).workplace);
        CHECK(building.settlement == registry.get<lw::Building>(world.find(person.home)).settlement);
        CHECK(job.start < job.end && job.end <= 24 * 60 && job.days != 0);
        CHECK(registry.all_of<lw::Workplace>(workplace));
    }
    CHECK(employed > 20);
    CHECK(pupils > 0);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::Employed) == static_cast<std::size_t>(employed));
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        // У взрослых — сбережения, у детей — карманные деньги, бывает и ноль.
        const auto* wallet = registry.try_get<lw::Wallet>(entity);
        CHECK(wallet != nullptr && (wallet->money > 0 || lw::isMinor(registry, entity)));
    }

    // Без мирной жизни — ни профессий, ни денег.
    lw::WorldConfig plain = peaceConfig(150);
    plain.peacetime = false;
    const lw::World before(7, plain);
    CHECK(before.registry().view<const lw::Job>().empty());
    CHECK(before.registry().view<const lw::Wallet>().empty());
    CHECK(before.registry().view<const lw::Workplace>().empty());
}

// Неделя, смены и выходные.
void testShifts() {
    const std::uint64_t hour = lw::kTicksPerGameHour;
    const std::uint64_t day = lw::kTicksPerGameDay;
    CHECK(lw::calendarAt(0).dayOfWeek == 0); // первый день мира — понедельник
    CHECK(lw::calendarAt(6 * day).dayOfWeek == 6);
    CHECK(lw::calendarAt(7 * day).dayOfWeek == 0);
    lw::Job job;
    job.profession = lw::Profession::Shopkeeper;
    job.start = 8 * 60;
    job.end = 18 * 60;
    job.days = 0b0011111; // с понедельника по пятницу
    // Тик 0 — понедельник, 08:00: смена идёт до 18:00.
    CHECK(lw::onShift(job, lw::calendarAt(0)));
    CHECK(lw::shiftEndTick(job, 0) == 10 * hour);
    CHECK(!lw::onShift(job, lw::calendarAt(10 * hour)));
    CHECK(lw::shiftEndTick(job, 10 * hour) == 10 * hour);
    // Вторник, 07:00: смена ещё не началась, кончится в 18:00.
    CHECK(!lw::onShift(job, lw::calendarAt(23 * hour)));
    CHECK(lw::shiftEndTick(job, 23 * hour) == 34 * hour);
    // Суббота — выходной.
    CHECK(!lw::onShift(job, lw::calendarAt(5 * day + 2 * hour)));
    CHECK(lw::shiftEndTick(job, 5 * day + 2 * hour) == 5 * day + 2 * hour);
    CHECK(lw::priceOf(lw::ItemKind::Food) == lw::kFoodPrice && lw::priceOf(lw::ItemKind::Rifle) == 0);
}

// Продавец идёт на работу, и магазин открывается; за час на месте — плата;
// смена кончилась — уходит, магазин закрыт.
void testWorkday() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    const entt::entity seller = workerOf(world, lw::Profession::Shopkeeper);
    CHECK(seller != entt::null);
    if (seller == entt::null) {
        return;
    }
    lw::Job& job = registry.get<lw::Job>(seller);
    job.start = 8 * 60;
    job.end = 12 * 60; // смена до полудня
    job.days = 0b1111111;
    registry.get<lw::Needs>(seller) = {};
    registry.get<lw::Traits>(seller) = {};
    const lw::EntityId shop = job.workplace;
    CHECK(!lw::isOpen(world, shop));
    bool arrived = false;
    for (int minute = 0; minute < 3 * 60 && !arrived; ++minute) {
        runMinutes(world, 1);
        arrived = lw::isOnDuty(world, seller);
    }
    CHECK(arrived);
    CHECK(registry.get<lw::Mind>(seller).action == lw::Action::Work);
    runMinutes(world, 1);
    CHECK(lw::isOpen(world, shop));
    const int money = registry.get<lw::Wallet>(seller).money;
    runMinutes(world, 61);
    CHECK(registry.get<lw::Wallet>(seller).money >=
          money + lw::professionSpec(lw::Profession::Shopkeeper).wage);
    runUntil(world, 12, 30);
    CHECK(!lw::isOnDuty(world, seller));
    CHECK(!lw::isOpen(world, shop));
    CHECK(registry.get<lw::Mind>(seller).action != lw::Action::Work);
}

// Дома кончилась еда — купить в открытом магазине: деньги — в кассу, еда — с собой.
void testBuyFood() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    runUntil(world, 10, 0); // магазины открылись
    const entt::entity buyer = idlerNear(world, lw::BuildingKind::Shop);
    CHECK(buyer != entt::null);
    if (buyer == entt::null) {
        return;
    }
    registry.get<lw::Needs>(buyer) = {};
    registry.get<lw::Needs>(buyer).hunger = 0.6f;
    registry.get<lw::Traits>(buyer) = {};
    registry.get<lw::Inventory>(buyer) = {};
    registry.get<lw::Wallet>(buyer).money = 100;
    lw::Inventory& home = registry.get<lw::Inventory>(world.find(registry.get<lw::Person>(buyer).home));
    home.set(lw::ItemKind::Food, 0);
    home.set(lw::ItemKind::Meat, 0);
    const lw::Mind& mind = thinkNow(world, buyer);
    CHECK(mind.action == lw::Action::Buy);
    if (mind.action != lw::Action::Buy) {
        return;
    }
    const entt::entity shop = world.find(mind.target);
    const int stock = registry.get<lw::Inventory>(shop).food();
    const int till = registry.get<lw::Workplace>(shop).till;
    const std::size_t bought = world.chronicle().countOf(lw::ChronicleKind::Bought);
    for (int minute = 0; minute < 4 * 60 && registry.get<lw::Wallet>(buyer).money == 100; ++minute) {
        runMinutes(world, 1);
    }
    const int spent = 100 - registry.get<lw::Wallet>(buyer).money;
    CHECK(spent >= lw::kFoodPrice);
    CHECK(registry.get<lw::Workplace>(shop).till >= till + spent);
    CHECK(registry.get<lw::Inventory>(shop).food() < stock);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::Bought) > bought);
    const lw::ChronicleEvent* purchase = nullptr;
    for (const std::uint32_t index : world.chronicle().eventsOf(registry.get<lw::Id>(buyer).value)) {
        const lw::ChronicleEvent& event = world.chronicle().events()[index];
        if (event.kind == lw::ChronicleKind::Bought) {
            purchase = &event;
        }
    }
    CHECK(purchase != nullptr && purchase->a > 0 && purchase->third != lw::kInvalidId);
}

// Магазин открыт, только пока продавец на месте: без него купить негде, а
// при нём магазин не обыскивают.
void testShopNeedsStaff() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    runUntil(world, 10, 0);
    const entt::entity buyer = idlerNear(world, lw::BuildingKind::Shop);
    CHECK(buyer != entt::null);
    if (buyer == entt::null) {
        return;
    }
    registry.get<lw::Needs>(buyer) = {};
    registry.get<lw::Needs>(buyer).hunger = 0.6f;
    registry.get<lw::Inventory>(buyer) = {};
    lw::Inventory& home = registry.get<lw::Inventory>(world.find(registry.get<lw::Person>(buyer).home));
    home.set(lw::ItemKind::Food, 0);
    home.set(lw::ItemKind::Meat, 0);
    CHECK(lw::evaluateActions(world, buyer)[static_cast<std::size_t>(lw::Action::Buy)] > 0.0f);
    // Денег нет — не купить.
    registry.get<lw::Wallet>(buyer).money = 0;
    CHECK(lw::evaluateActions(world, buyer)[static_cast<std::size_t>(lw::Action::Buy)] == 0.0f);
    registry.get<lw::Wallet>(buyer).money = 100;
    // У всех продавцов выходной — магазины закрылись.
    for (const auto [entity, job] : registry.view<lw::Job>().each()) {
        if (job.profession == lw::Profession::Shopkeeper) {
            job.days = 0;
        }
    }
    runMinutes(world, 1);
    for (const auto [entity, building, workplace] :
         registry.view<const lw::Building, const lw::Workplace>().each()) {
        CHECK(building.kind != lw::BuildingKind::Shop || workplace.onDuty == 0);
    }
    CHECK(lw::evaluateActions(world, buyer)[static_cast<std::size_t>(lw::Action::Buy)] == 0.0f);
}

// В 06:00 — завоз в магазины, где есть продавцы: деревенский берёт сперва урожай
// своих ферм, касса сдаётся; без работников — не завозят. Безработным — пособие.
void testRestock() {
    lw::World world(7, peaceConfig(150));
    entt::registry& registry = world.registry();
    // Без решений: ночью никто ничего не возьмёт.
    std::vector<entt::entity> people;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        people.push_back(entity);
    }
    for (const entt::entity person : people) {
        registry.remove<lw::Mind>(person);
    }
    entt::entity townShop = entt::null;
    entt::entity villageShop = entt::null;
    entt::entity farm = entt::null;
    entt::entity closedShop = entt::null;
    for (const lw::Places::SettlementPlace& place : world.places().settlements()) {
        const bool town = registry.get<lw::Settlement>(world.find(place.id)).kind == lw::SettlementKind::Town;
        entt::entity shop = entt::null;
        entt::entity barn = entt::null;
        for (const lw::Places::BuildingPlace& building : place.buildings) {
            const entt::entity entity = world.find(building.id);
            const bool staffed = registry.all_of<lw::Workplace>(entity);
            if (building.kind == lw::BuildingKind::Shop && staffed && shop == entt::null) {
                shop = entity;
            }
            if (building.kind == lw::BuildingKind::Shop && !staffed && closedShop == entt::null) {
                closedShop = entity;
            }
            if (building.kind == lw::BuildingKind::Barn && barn == entt::null) {
                barn = entity;
            }
        }
        if (town && townShop == entt::null) {
            townShop = shop;
        } else if (!town && villageShop == entt::null && shop != entt::null && barn != entt::null) {
            villageShop = shop;
            farm = barn;
        }
    }
    CHECK(townShop != entt::null && closedShop != entt::null);
    if (townShop == entt::null || closedShop == entt::null) {
        return;
    }
    registry.get<lw::Inventory>(townShop).set(lw::ItemKind::Food, 0);
    registry.get<lw::Workplace>(townShop).till = 50;
    registry.get<lw::Inventory>(closedShop).set(lw::ItemKind::Food, 0);
    if (villageShop != entt::null) {
        registry.get<lw::Inventory>(villageShop).set(lw::ItemKind::Food, 0);
        registry.get<lw::Inventory>(farm).set(lw::ItemKind::Food, 10);
    }
    std::vector<int> money;
    for (const entt::entity person : people) {
        money.push_back(registry.get<lw::Wallet>(person).money);
    }
    runUntil(world, 6, 1);
    CHECK(registry.get<lw::Inventory>(townShop).food() == 40);
    CHECK(registry.get<lw::Workplace>(townShop).till == 0);
    CHECK(registry.get<lw::Inventory>(closedShop).food() == 0);
    if (villageShop != entt::null) {
        CHECK(registry.get<lw::Inventory>(villageShop).food() == 25);
        CHECK(registry.get<lw::Inventory>(farm).food() == 0); // урожай фермы — в магазин
    }
    for (std::size_t i = 0; i < people.size(); ++i) {
        // Пособие — взрослым без работы; детям — нет (этап 7.3).
        const auto* job = registry.try_get<lw::Job>(people[i]);
        const int income = registry.get<lw::Wallet>(people[i]).money - money[i];
        const bool allowance = !lw::isEmployed(job) && !lw::isMinor(registry, people[i]);
        CHECK(income == (allowance ? lw::kAllowance : 0));
    }
}

// Фермер в поле растит урожай: в сарае прибавляется еда — по сезону.
void testHarvest() {
    lw::World world(7, peaceConfig(150));
    entt::registry& registry = world.registry();
    const entt::entity farmer = workerOf(world, lw::Profession::Farmer);
    CHECK(farmer != entt::null);
    if (farmer == entt::null) {
        return;
    }
    lw::Job& job = registry.get<lw::Job>(farmer);
    job.start = 0;
    job.end = 24 * 60;
    job.days = 0b1111111;
    registry.get<lw::Needs>(farmer) = {};
    registry.get<lw::Traits>(farmer) = {};
    for (int minute = 0; minute < 4 * 60 && !lw::isOnDuty(world, farmer); ++minute) {
        runMinutes(world, 1);
    }
    CHECK(lw::isOnDuty(world, farmer));
    const lw::Map& map = world.map();
    const lw::Mind& mind = registry.get<lw::Mind>(farmer);
    const entt::entity barn = world.find(job.workplace);
    CHECK(map.ground.at(mind.spot.x, mind.spot.y) == lw::Ground::Field ||
          mind.spot == registry.get<lw::Building>(barn).door);
    const auto grown = [&] {
        return static_cast<float>(registry.get<lw::Inventory>(barn).food()) +
               registry.get<lw::Workplace>(barn).harvest;
    };
    const float before = grown();
    runMinutes(world, 3 * 60);
    const float rate = lw::harvestPerHour(lw::calendarAt(world.tick()).season);
    CHECK(grown() - before > 2.5f * rate);
}

// Ранен, а бинта нет, — в больницу: врач на смене останавливает кровь и лечит;
// вылеченный помнит, что врач ему помог.
void testSeeDoctor() {
    lw::World world(7, peaceConfig(150));
    entt::registry& registry = world.registry();
    runUntil(world, 10, 0); // врачи дневной смены на месте
    const entt::entity patient = idlerNear(world, lw::BuildingKind::Hospital);
    CHECK(patient != entt::null);
    if (patient == entt::null) {
        return;
    }
    registry.get<lw::Needs>(patient) = {};
    registry.get<lw::Needs>(patient).health = 0.4f;
    registry.get<lw::Traits>(patient) = {};
    registry.get<lw::Inventory>(patient) = {};
    registry.get<lw::Wounds>(patient).bleeding = 0.05f;
    const lw::Mind& mind = thinkNow(world, patient);
    CHECK(mind.action == lw::Action::SeeDoctor);
    if (mind.action != lw::Action::SeeDoctor) {
        return;
    }
    registry.get<lw::Inventory>(world.find(mind.target)).set(lw::ItemKind::Medicine, 5);
    for (int minute = 0; minute < 5 * 60 && world.chronicle().countOf(lw::ChronicleKind::SawDoctor) == 0;
         ++minute) {
        runMinutes(world, 1);
    }
    CHECK(world.chronicle().countOf(lw::ChronicleKind::SawDoctor) == 1);
    CHECK(registry.get<lw::Wounds>(patient).bleeding == 0.0f);
    CHECK(registry.get<lw::Needs>(patient).health > 0.5f);
    const lw::ChronicleEvent& visit = world.chronicle().events().back();
    const entt::entity doctor = world.find(visit.third);
    CHECK(doctor != entt::null && registry.get<lw::Job>(doctor).profession == lw::Profession::Doctor);
    CHECK(registry.get<lw::Memories>(patient).find(lw::MemoryKind::Helped, visit.third) != nullptr);
}

// При полицейском на службе лезть в чужое и красть страшно — кроме отчаявшихся.
void testPoliceDeters() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    const entt::entity officer = workerOf(world, lw::Profession::PoliceOfficer);
    entt::entity thief = entt::null;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        if (!registry.all_of<lw::Job>(entity) && person.home != lw::kInvalidId &&
            (thief == entt::null || registry.get<lw::Id>(entity).value < registry.get<lw::Id>(thief).value)) {
            thief = entity;
        }
    }
    CHECK(officer != entt::null && thief != entt::null);
    if (officer == entt::null || thief == entt::null) {
        return;
    }
    // Голоден, еды и денег нет — тянет обыскать чужое.
    registry.get<lw::Needs>(thief) = {};
    registry.get<lw::Needs>(thief).hunger = 0.7f;
    registry.get<lw::Traits>(thief) = {};
    registry.get<lw::Inventory>(thief) = {};
    registry.get<lw::Wallet>(thief).money = 0;
    lw::Inventory& home = registry.get<lw::Inventory>(world.find(registry.get<lw::Person>(thief).home));
    home.set(lw::ItemKind::Food, 0);
    home.set(lw::ItemKind::Meat, 0);
    // Полицейский — в другом конце мира.
    const auto scavenge = [&] {
        return lw::evaluateActions(world, thief)[static_cast<std::size_t>(lw::Action::Scavenge)];
    };
    registry.get<lw::Position>(officer) = {1.0f, 1.0f};
    world.syncSpatial();
    const float alone = scavenge();
    CHECK(alone > 0.0f);
    // На службе и рядом.
    lw::Job& job = registry.get<lw::Job>(officer);
    job.start = 0;
    job.end = 24 * 60;
    job.days = 0b1111111;
    registry.get<lw::Person>(officer).asleep = false;
    registry.get<lw::Mind>(officer).action = lw::Action::Work;
    const lw::Position at = registry.get<lw::Position>(thief);
    registry.get<lw::Position>(officer) = {at.x + 20.0f, at.y};
    world.syncSpatial();
    CHECK(lw::policeNearby(world, at.x, at.y, 50.0f));
    CHECK(scavenge() < 0.5f * alone);
    // Отчаявшемуся всё равно.
    registry.get<lw::Needs>(thief).hunger = 0.95f;
    const float desperate = scavenge();
    registry.get<lw::Position>(officer) = {1.0f, 1.0f};
    world.syncSpatial();
    CHECK(std::abs(scavenge() - desperate) < 1e-4f);
}

// Водопровод: в жилье вода не кончается; без мирной жизни — нет.
void testTapWater() {
    for (const bool peace : {true, false}) {
        lw::WorldConfig config = peaceConfig(0);
        config.peacetime = peace;
        lw::World world(7, config);
        entt::registry& registry = world.registry();
        entt::entity house = entt::null;
        for (const auto [entity, building, inventory] :
             registry.view<const lw::Building, lw::Inventory>().each()) {
            if (building.kind == lw::BuildingKind::House && house == entt::null) {
                house = entity;
                inventory.set(lw::ItemKind::Water, 0);
            }
        }
        CHECK(house != entt::null);
        if (house == entt::null) {
            continue;
        }
        world.weather().precipitation = 0.0f; // без дождя: бочки не наполняются
        world.weather().minutesLeft = 100000;
        runMinutes(world, 61);
        CHECK(registry.get<lw::Inventory>(house).count(lw::ItemKind::Water) ==
              (peace ? lw::tapWater(lw::BuildingKind::House) : 0));
    }
}

// --- Сценарий и фазы мира (этап 7.4) ---------------------------------------------

// Убрать всех зомби и орды: тесту нужны правила фаз, а не нападения.
void clearZombies(lw::World& world) {
    std::vector<entt::entity> doomed;
    for (const auto [entity, zombie] : world.registry().view<const lw::Zombie>().each()) {
        doomed.push_back(entity);
    }
    for (const auto [entity, horde] : world.registry().view<const lw::Horde>().each()) {
        doomed.push_back(entity);
    }
    for (const entt::entity entity : doomed) {
        world.destroy(entity);
    }
    world.syncSpatial();
}

// Оценка действия человека сейчас.
float scoreOf(const lw::World& world, entt::entity person, lw::Action action) {
    return lw::evaluateActions(world, person)[static_cast<std::size_t>(action)];
}

// Работник на смене весь день, каждый день, с такими чертами.
void alwaysOnShift(entt::registry& registry, entt::entity worker, const lw::Traits& traits) {
    lw::Job& job = registry.get<lw::Job>(worker);
    job.start = 0;
    job.end = 24 * 60;
    job.days = 0b1111111;
    registry.get<lw::Traits>(worker) = traits;
}

// Все черты — value.
lw::Traits traitsOf(float value) {
    lw::Traits traits;
    traits.bravery = traits.greed = traits.altruism = traits.aggression = traits.leadership =
        traits.sociability = traits.curiosity = traits.prudence = value;
    return traits;
}

// Живые люди с домом и без работы — по возрастанию id.
std::vector<entt::entity> idlers(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::vector<std::pair<lw::EntityId, entt::entity>> found;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        const auto* job = registry.try_get<lw::Job>(entity);
        if ((job == nullptr || job->profession == lw::Profession::None) &&
            person.home != lw::kInvalidId) {
            found.emplace_back(id.value, entity);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<entt::entity> result;
    for (const auto& [id, entity] : found) {
        result.push_back(entity);
    }
    return result;
}

lw::EntityId idOf(const lw::World& world, entt::entity entity) {
    return world.registry().get<lw::Id>(entity).value;
}

std::uint64_t phaseBegan(const lw::World& world, lw::WorldPhase phase) {
    return lw::scenarioOf(world)->began[static_cast<std::size_t>(phase)];
}

// Правила фаз: что держится и что нет, кто выходит на работу. Мир без мирной
// жизни сценария не имеет и живёт по правилам выживания.
void testPhaseRules() {
    using P = lw::WorldPhase;
    const lw::PhaseRules& peace = lw::phaseRules(P::Peace);
    const lw::PhaseRules& outbreak = lw::phaseRules(P::Outbreak);
    const lw::PhaseRules& panic = lw::phaseRules(P::Panic);
    const lw::PhaseRules& collapse = lw::phaseRules(P::Collapse);
    const lw::PhaseRules& survival = lw::phaseRules(P::Survival);
    CHECK(peace.economy && peace.tapWater && peace.lawful && peace.supply == 1.0f &&
          !peace.groups && peace.zombieShare == 0.0f && peace.eveningHome > 0.0f);
    // Во вспышке и панике зомби — только свои, бывшие жители (этап 7.5); приток
    // издалека — с коллапсом.
    CHECK(outbreak.lawful && outbreak.supply == 1.0f && outbreak.zombieShare == 0.0f &&
          outbreak.arrivals == 0);
    CHECK(panic.economy && panic.lawful && panic.supply < 1.0f && panic.stockpile > 1.0f &&
          panic.groups && panic.eveningHome > peace.eveningHome && panic.arrivals == 0);
    CHECK(!collapse.economy && !collapse.tapWater && !collapse.lawful && collapse.supply == 0.0f &&
          collapse.zombieShare > 0.0f && collapse.arrivals > 0);
    CHECK(survival.zombieShare > collapse.zombieShare && survival.arrivals >= collapse.arrivals);
    CHECK(!survival.economy && !survival.lawful && survival.professions == 0 &&
          survival.maxHours == 0);
    CHECK(lw::phaseByName("panic") == P::Panic);
    CHECK(!lw::phaseByName("riot"));
    CHECK(lw::nextPhase(P::Peace) == P::Outbreak && lw::nextPhase(P::Survival) == P::Survival);

    const lw::Traits steadfast = traitsOf(1.0f);
    const lw::Traits timid = traitsOf(0.0f);
    for (int i = 1; i < static_cast<int>(lw::Profession::Count); ++i) {
        const auto profession = static_cast<lw::Profession>(i);
        CHECK(lw::keepsWorking(P::Peace, profession, timid));
        CHECK(lw::keepsWorking(P::Outbreak, profession, timid));
        CHECK(!lw::keepsWorking(P::Survival, profession, steadfast));
    }
    CHECK(!lw::keepsWorking(P::Peace, lw::Profession::None, steadfast));
    CHECK(!lw::keepsWorking(P::Panic, lw::Profession::Teacher, steadfast)); // школа закрыта
    CHECK(lw::keepsWorking(P::Panic, lw::Profession::Shopkeeper, steadfast));
    CHECK(!lw::keepsWorking(P::Panic, lw::Profession::Shopkeeper, timid));
    CHECK(lw::keepsWorking(P::Collapse, lw::Profession::Doctor, steadfast));
    CHECK(lw::keepsWorking(P::Collapse, lw::Profession::PoliceOfficer, steadfast));
    CHECK(!lw::keepsWorking(P::Collapse, lw::Profession::Doctor, timid));
    CHECK(!lw::keepsWorking(P::Collapse, lw::Profession::Shopkeeper, steadfast));

    // Без мирной жизни сценария нет: катастрофа уже случилась.
    lw::WorldConfig plain = peaceConfig(20);
    plain.peacetime = false;
    plain.zombies = 40;
    const lw::World before(7, plain);
    CHECK(lw::scenarioOf(before) == nullptr && lw::scenarioIdOf(before) == lw::kInvalidId);
    CHECK(lw::worldPhase(before) == P::Survival);
    CHECK(lw::zombieTarget(before) == 40 && lw::arrivalsPerDay(before) == 1);
}

// Мир начинается с фазы «мир»; вспышка — по расписанию, с неё заражён
// нулевой пациент — взрослый житель. Один seed — одна и та же вспышка.
void testOutbreakScheduled() {
    lw::WorldConfig config = peaceConfig(60);
    config.outbreakDays = 0.25; // через 6 игровых часов
    lw::World world(7, config);
    lw::World twin(7, config);
    const entt::registry& registry = world.registry();
    const lw::Scenario* scenario = lw::scenarioOf(world);
    CHECK(scenario != nullptr);
    if (scenario == nullptr) {
        return;
    }
    const std::uint64_t sixHours = 6 * lw::kTicksPerGameHour;
    CHECK(scenario->phase == lw::WorldPhase::Peace && phaseBegan(world, lw::WorldPhase::Peace) == 0);
    CHECK(phaseBegan(world, lw::WorldPhase::Outbreak) == lw::Scenario::kNever);
    CHECK(scenario->outbreakAt == sixHours && lw::phaseDeadline(*scenario) == sixHours);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::PhaseBegan) == 1);
    CHECK(registry.view<const lw::Zombie>().empty() && lw::zombieTarget(world) == 0);

    runMinutes(world, 6 * 60 - 1);
    runMinutes(twin, 6 * 60 - 1);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Peace);
    runMinutes(world, 2);
    runMinutes(twin, 2);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Outbreak);
    CHECK(phaseBegan(world, lw::WorldPhase::Outbreak) == sixHours);
    const int people = static_cast<int>(registry.view<const lw::Person>().size());
    CHECK(lw::scenarioOf(world)->population == people);
    // Зомби издалека нет: вспышка начинается с нулевого пациента из жителей (этап 7.5).
    CHECK(registry.view<const lw::Zombie>().empty() && lw::zombieTarget(world) == 0);
    const lw::EntityId zero = lw::scenarioOf(world)->patientZero;
    CHECK(zero != lw::kInvalidId && zero == lw::scenarioOf(twin)->patientZero);
    const entt::entity patient = world.find(zero);
    CHECK(patient != entt::null && registry.all_of<lw::Person>(patient) && !lw::isMinor(registry, patient));
    if (patient != entt::null) {
        const lw::Wounds& wounds = registry.get<lw::Wounds>(patient);
        CHECK(wounds.route == lw::InfectionRoute::Origin && wounds.infectedAt == sixHours &&
              wounds.bites == 0 && wounds.infection > 0.0f && !lw::isVisiblyIll(wounds));
    }
    CHECK(lw::scenarioStats(world).infected == 1 && lw::scenarioStats(world).casualties() == 0);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::PatientZero) == 1);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::PhaseBegan) == 2);
    CHECK(world.stateHash() == twin.stateHash());
}

// Фазы сменяются сами: раньше срока — когда пострадавших не меньше порога, но
// не раньше minHours. Вперёд их можно перевести и вручную, назад — нет.
void testPhaseTransitions() {
    lw::World world(7, peaceConfig(40));
    entt::registry& registry = world.registry();
    CHECK(lw::phaseDeadline(*lw::scenarioOf(world)) == lw::Scenario::kNever); // не по расписанию
    runMinutes(world, 5);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Peace);
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    clearZombies(world);
    const std::uint64_t start = world.tick();
    const lw::Scenario* scenario = lw::scenarioOf(world);
    CHECK(scenario->phase == lw::WorldPhase::Outbreak && scenario->population == 40);
    CHECK(lw::phaseEarliest(*scenario) == start + 12 * lw::kTicksPerGameHour);
    CHECK(lw::phaseDeadline(*scenario) == start + 72 * lw::kTicksPerGameHour);
    const int trigger = lw::phaseTrigger(*scenario);
    CHECK(trigger == lw::phaseRules(lw::WorldPhase::Outbreak).triggerMin);

    // Трое заражены (укушены где-то ещё) — уже пострадавшие.
    std::vector<std::pair<lw::EntityId, entt::entity>> people;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        people.emplace_back(id.value, entity);
    }
    std::sort(people.begin(), people.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int i = 0; i < trigger; ++i) {
        lw::Wounds& wounds = registry.get_or_emplace<lw::Wounds>(people[static_cast<std::size_t>(i)].second);
        wounds.bites = 1;
        wounds.infection = 0.05f;
    }
    CHECK(lw::scenarioStats(world).casualties() == trigger);
    runMinutes(world, 11 * 60);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Outbreak); // ещё рано
    runMinutes(world, 2 * 60);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Panic);
    const std::uint64_t panic = phaseBegan(world, lw::WorldPhase::Panic);
    CHECK(panic >= start + 12 * lw::kTicksPerGameHour && panic < start + 13 * lw::kTicksPerGameHour);
    CHECK(lw::calendarAt(panic).minute == 0); // порог проверяется раз в час

    // Вручную — только вперёд.
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Panic);
    lw::enterPhase(world, lw::WorldPhase::Survival);
    CHECK(lw::worldPhase(world) == lw::WorldPhase::Survival);
    CHECK(phaseBegan(world, lw::WorldPhase::Collapse) == world.tick() &&
          phaseBegan(world, lw::WorldPhase::Survival) == world.tick());
    CHECK(world.chronicle().countOf(lw::ChronicleKind::PhaseBegan) == lw::kWorldPhases);
    CHECK(lw::phaseDeadline(*lw::scenarioOf(world)) == lw::Scenario::kNever);
    // Кто работал, бросил работу — каждый по разу.
    std::size_t employed = 0;
    for (const auto [entity, person, job] : registry.view<const lw::Person, const lw::Job>().each()) {
        employed += lw::isEmployed(&job) ? 1 : 0; // школа закрылась — дети работу не бросают
    }
    CHECK(employed > 0);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::LeftWork) == employed);
}

// Нулевой пациент (этап 7.5): с началом вспышки заражён один взрослый житель с
// домом; никто не замечает, пока у него нет жара, — потом он пострадавший.
// Зомби — только из жителей: заражённый погибший встаёт. Один seed — одна
// вспышка.
void testPatientZero() {
    lw::WorldConfig config = peaceConfig(60);
    lw::World world(11, config);
    lw::World twin(11, config);
    entt::registry& registry = world.registry();
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    lw::enterPhase(twin, lw::WorldPhase::Outbreak);
    const lw::Scenario* scenario = lw::scenarioOf(world);
    const lw::EntityId zero = scenario->patientZero;
    CHECK(zero != lw::kInvalidId && zero == lw::scenarioOf(twin)->patientZero);
    const entt::entity patient = world.find(zero);
    CHECK(patient != entt::null);
    if (patient == entt::null) {
        return;
    }
    CHECK(!lw::isMinor(registry, patient) && registry.get<lw::Person>(patient).home != lw::kInvalidId);
    const lw::Wounds& wounds = registry.get<lw::Wounds>(patient);
    CHECK(wounds.route == lw::InfectionRoute::Origin && wounds.infectedBy == lw::scenarioIdOf(world) &&
          wounds.infection == lw::kPatientZeroInfection);
    CHECK(lw::infectionRouteOf(world, zero) == lw::InfectionRoute::Origin);
    const lw::ChronicleEvent* origin = nullptr;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        origin = event.kind == lw::ChronicleKind::PatientZero ? &event : origin;
    }
    CHECK(origin != nullptr && origin->subject == zero && origin->object == lw::scenarioIdOf(world));
    CHECK(registry.view<const lw::Zombie>().empty());
    lw::ScenarioStats stats = lw::scenarioStats(world);
    CHECK(stats.infected == 1 && stats.sick == 0 && stats.casualties() == 0);

    // Жар — через (0,5 − 0,2) × 40 = 12 игровых часов: теперь это заметно.
    const auto hours = static_cast<int>(std::ceil(lw::hoursToFever(wounds))) + 1;
    runMinutes(world, hours * 60);
    runMinutes(twin, hours * 60);
    if (world.find(zero) == patient && registry.all_of<lw::Person>(patient)) {
        CHECK(lw::isFeverish(registry.get<lw::Wounds>(patient)));
        stats = lw::scenarioStats(world);
        CHECK(stats.sick >= 1 && stats.casualties() >= 1);
    }
    CHECK(world.chronicle().countOf(lw::ChronicleKind::Feverish) >= 1);
    CHECK(world.stateHash() == twin.stateHash());
    // Зомби издалека во вспышке не приходят.
    CHECK(lw::arrivalsPerDay(world) == 0 && lw::zombieTarget(world) == 0);
}

// Вирус приходит в край не один раз (этап 7.5): если во вспышке среди жителей
// не осталось заразы, через kReimportTicks заражается новый житель — завозной
// случай; нулевой пациент сценария остаётся прежним.
void testReimport() {
    lw::World world(5, peaceConfig(40));
    entt::registry& registry = world.registry();
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    const lw::EntityId zero = lw::scenarioOf(world)->patientZero;
    CHECK(zero != lw::kInvalidId);
    // Нулевой пациент выздоровел — заразы больше нет.
    registry.get<lw::Wounds>(world.find(zero)) = {};
    CHECK(lw::scenarioStats(world).infected == 0);
    runMinutes(world, 11 * 60);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::PatientZero) == 1);
    CHECK(lw::scenarioOf(world)->quietSince != lw::Scenario::kNever);
    runMinutes(world, 3 * 60);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::PatientZero) == 2);
    const lw::ChronicleEvent* imported = nullptr;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        imported = event.kind == lw::ChronicleKind::PatientZero ? &event : imported;
    }
    CHECK(imported != nullptr && imported->a == 1 && imported->subject != zero &&
          imported->object == lw::scenarioIdOf(world));
    if (imported != nullptr) {
        CHECK(!lw::isPatientZero(world, imported->subject) && lw::isPatientZero(world, zero));
        CHECK(lw::infectionRouteOf(world, imported->subject) == lw::InfectionRoute::Origin);
    }
    CHECK(lw::scenarioOf(world)->patientZero == zero);
    CHECK(lw::scenarioOf(world)->quietSince == lw::Scenario::kNever);
    CHECK(lw::scenarioStats(world).infected >= 1);
}

// Больной в лихорадке (этап 7.5): на работу почти не выходит, лежит дома и
// днём, тянет к врачу, гулять — нет. Родня из другого дома и друзья навещают
// его, даже если одиночества не чувствуют.
void testSickBehaviour() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    clearZombies(world);
    const entt::entity worker = workerOf(world, lw::Profession::Shopkeeper);
    CHECK(worker != entt::null);
    if (worker == entt::null) {
        return;
    }
    alwaysOnShift(registry, worker, traitsOf(0.5f));
    // Второй день, 11:00: ни сна, ни вечера, и с начала мира прошло больше
    // суток — родня успела не видеться.
    runMinutes(world, 9 * 60);
    runUntil(world, 11, 0);
    registry.get<lw::Needs>(worker) = {};
    const float work = scoreOf(world, worker, lw::Action::Work);
    const float wander = scoreOf(world, worker, lw::Action::Wander);
    CHECK(work > 0.5f);
    CHECK(scoreOf(world, worker, lw::Action::Sleep) == 0.0f);

    lw::Wounds& wounds = registry.get<lw::Wounds>(worker);
    wounds.infection = 0.6f;
    wounds.route = lw::InfectionRoute::Nearby;
    const float sickWork = scoreOf(world, worker, lw::Action::Work);
    const float bed = scoreOf(world, worker, lw::Action::Sleep);
    CHECK(sickWork > 0.0f && sickWork < 0.3f * work);
    CHECK(bed > sickWork);
    CHECK(scoreOf(world, worker, lw::Action::Wander) <= 0.5f * wander + 1e-4f);

    // Навестить больного: знакомый из другого дома — пусть он ему родня.
    entt::entity visitor = entt::null;
    entt::entity patient = entt::null;
    for (const auto [entity, id, person, relations] :
         registry.view<const lw::Id, const lw::Person, lw::Relations>().each()) {
        if (lw::isMinor(registry, entity) || entity == worker) {
            continue;
        }
        for (std::size_t i = 0; i < relations.count && patient == entt::null; ++i) {
            const entt::entity other = world.find(relations.bonds[i].other);
            if (other != entt::null && other != worker && registry.all_of<lw::Person>(other) &&
                registry.get<lw::Person>(other).home != person.home) {
                relations.bonds[i].origin = lw::BondOrigin::Kin;
                visitor = entity;
                patient = other;
            }
        }
        if (patient != entt::null) {
            break;
        }
    }
    CHECK(patient != entt::null);
    if (patient == entt::null) {
        return;
    }
    // Рядом — чтобы дорога не съела оценку; одиночества нет.
    registry.get<lw::Position>(visitor) = registry.get<lw::Position>(patient);
    registry.get<lw::Position>(visitor).x += 30.0f;
    world.syncSpatial();
    registry.get<lw::Needs>(visitor) = {};
    if (lw::Bond* bond = registry.get<lw::Relations>(visitor).find(idOf(world, patient))) {
        bond->seen = 0; // давно не виделись
    }
    registry.get<lw::Wounds>(patient) = {};
    const float calm = scoreOf(world, visitor, lw::Action::Socialize);
    lw::Wounds& ill = registry.get<lw::Wounds>(patient);
    ill.infection = 0.6f;
    ill.route = lw::InfectionRoute::Household;
    CHECK(lw::isFeverish(ill));
    const float worried = scoreOf(world, visitor, lw::Action::Socialize);
    CHECK(worried > calm + 0.1f);
}

// Коллапс: на работу выходят только самые стойкие врачи и полицейские, кто
// бросил — в летописи; ни завоза, ни пособия, ни воды из крана; кража уже не
// преступление — лезть в чужое не совестно.
void testCollapseRules() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    const entt::entity seller = workerOf(world, lw::Profession::Shopkeeper);
    const entt::entity doctor = workerOf(world, lw::Profession::Doctor);
    const std::vector<entt::entity> idle = idlers(world);
    CHECK(seller != entt::null && doctor != entt::null && !idle.empty());
    if (seller == entt::null || doctor == entt::null || idle.empty()) {
        return;
    }
    alwaysOnShift(registry, seller, traitsOf(1.0f));
    alwaysOnShift(registry, doctor, traitsOf(1.0f));
    // Голоден, а еды и денег нет — тянет обыскать чужое.
    const entt::entity hungry = idle.front();
    registry.get<lw::Needs>(hungry) = {};
    registry.get<lw::Needs>(hungry).hunger = 0.7f;
    registry.get<lw::Traits>(hungry) = {};
    registry.get<lw::Inventory>(hungry) = {};
    registry.get<lw::Wallet>(hungry).money = 0;
    lw::Inventory& pantry = registry.get<lw::Inventory>(world.find(registry.get<lw::Person>(hungry).home));
    pantry.set(lw::ItemKind::Food, 0);
    pantry.set(lw::ItemKind::Meat, 0);
    CHECK(scoreOf(world, seller, lw::Action::Work) > 0.0f);
    CHECK(scoreOf(world, doctor, lw::Action::Work) > 0.0f);
    const float lawful = scoreOf(world, hungry, lw::Action::Scavenge);
    CHECK(lawful > 0.0f);

    lw::enterPhase(world, lw::WorldPhase::Collapse);
    clearZombies(world);
    CHECK(scoreOf(world, seller, lw::Action::Work) == 0.0f);
    CHECK(scoreOf(world, doctor, lw::Action::Work) > 0.0f);
    CHECK(scoreOf(world, hungry, lw::Action::Scavenge) > 2.0f * lawful);
    // Кто бросил работу — в летописи, со своим местом работы.
    std::size_t quit = 0;
    bool sellerQuit = false;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        if (event.kind != lw::ChronicleKind::LeftWork) {
            continue;
        }
        ++quit;
        const entt::entity person = world.find(event.subject);
        const lw::Job& job = registry.get<lw::Job>(person);
        CHECK(event.object == job.workplace);
        CHECK(!lw::keepsWorking(lw::WorldPhase::Collapse, job.profession, registry.get<lw::Traits>(person)));
        CHECK(event.subject != idOf(world, doctor));
        sellerQuit = sellerQuit || event.subject == idOf(world, seller);
    }
    CHECK(quit > 0 && sellerQuit);

    // Ни завоза, ни пособия, ни воды из крана.
    const entt::entity shop = world.find(registry.get<lw::Job>(seller).workplace);
    registry.get<lw::Inventory>(shop).set(lw::ItemKind::Food, 0);
    entt::entity house = entt::null;
    for (const auto [entity, id, building] : registry.view<const lw::Id, const lw::Building>().each()) {
        if (building.kind != lw::BuildingKind::House ||
            (house != entt::null && id.value > idOf(world, house))) {
            continue;
        }
        bool empty = true;
        for (const auto [other, person] : registry.view<const lw::Person>().each()) {
            empty = empty && person.home != id.value;
        }
        if (empty) {
            house = entity;
        }
    }
    CHECK(house != entt::null);
    if (house != entt::null) {
        registry.get<lw::Inventory>(house).set(lw::ItemKind::Water, 0);
    }
    world.weather().precipitation = 0.0f; // без дождя: бочки не наполняются
    world.weather().minutesLeft = 100000;
    runUntil(world, lw::kRestockHour, 1);
    CHECK(registry.get<lw::Inventory>(shop).count(lw::ItemKind::Food) == 0);
    if (house != entt::null) {
        CHECK(registry.get<lw::Inventory>(house).count(lw::ItemKind::Water) == 0);
    }
    if (const auto* wallet = registry.try_get<lw::Wallet>(hungry)) {
        CHECK(wallet->money == 0);
    }
}

// Паника: школа закрыта, на работу выходят те, кто держится за неё, туда и
// везут товар — вполовину; вечером сильнее тянет домой; в группы сбиваются
// только теперь.
void testPanicRules() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    const entt::entity teacher = workerOf(world, lw::Profession::Teacher);
    const entt::entity seller = workerOf(world, lw::Profession::Shopkeeper);
    const std::vector<entt::entity> idle = idlers(world);
    CHECK(teacher != entt::null && seller != entt::null && idle.size() >= 3);
    if (teacher == entt::null || seller == entt::null || idle.size() < 3) {
        return;
    }
    alwaysOnShift(registry, teacher, traitsOf(1.0f));
    alwaysOnShift(registry, seller, traitsOf(0.0f));
    CHECK(scoreOf(world, teacher, lw::Action::Work) > 0.0f);
    CHECK(scoreOf(world, seller, lw::Action::Work) > 0.0f);

    // Двое друзей с одним домом: в мирное время группу не собирают.
    for (const auto [entity, relations] : registry.view<lw::Relations>().each()) {
        relations = {};
    }
    const entt::entity a = idle[0];
    const entt::entity b = idle[1];
    // Не родня и взрослые: с роднёй группу не основывают (этап 7.3).
    for (const entt::entity person : {a, b}) {
        registry.get<lw::Kin>(person) = {};
        registry.get<lw::Age>(person).years = 30;
    }
    registry.get<lw::Traits>(a).leadership = 0.8f;
    registry.get<lw::Traits>(b).leadership = 0.3f;
    registry.get<lw::Person>(b).home = registry.get<lw::Person>(a).home;
    setBond(registry, a, idOf(world, b), 0.9f, 0.7f, 0.5f, 0.7f);
    setBond(registry, b, idOf(world, a), 0.9f, 0.7f, 0.5f, 0.7f);
    lw::updateGroups(world);
    CHECK(registry.get<lw::Person>(a).group == lw::kInvalidId);

    lw::enterPhase(world, lw::WorldPhase::Panic);
    clearZombies(world);
    CHECK(scoreOf(world, teacher, lw::Action::Work) == 0.0f); // школа закрыта
    CHECK(scoreOf(world, seller, lw::Action::Work) == 0.0f);  // нестойкий бросил
    registry.get<lw::Traits>(seller) = traitsOf(1.0f);
    CHECK(scoreOf(world, seller, lw::Action::Work) > 0.0f);
    lw::updateGroups(world);
    CHECK(registry.get<lw::Person>(a).group != lw::kInvalidId &&
          registry.get<lw::Person>(a).group == registry.get<lw::Person>(b).group);

    // Вечером, не дома, тянет домой с силой фазы.
    lw::Job& job = registry.get<lw::Job>(seller);
    job.start = 8 * 60;
    job.end = 18 * 60;
    const entt::entity walker = idle[2];
    registry.get<lw::Traits>(walker).chronotype = 1.0f; // сова: в 19:00 ещё не ночь
    runUntil(world, 19, 0);
    if (registry.all_of<lw::Person>(walker)) {
        registry.get<lw::Needs>(walker).fear = 0.0f;
        for (lw::Danger& danger : registry.get<lw::Mind>(walker).dangers) {
            danger = {};
        }
        const lw::Cell door =
            registry.get<lw::Building>(world.find(registry.get<lw::Person>(walker).home)).door;
        registry.get<lw::Position>(walker) = {lw::Map::cellCenter(door.x) + 40.0f,
                                              lw::Map::cellCenter(door.y)};
        CHECK(std::abs(scoreOf(world, walker, lw::Action::GoHome) -
                       lw::phaseRules(lw::WorldPhase::Panic).eveningHome) < 1e-4f);
    }

    // Завоз — вполовину, в магазин, где ещё работают.
    const entt::entity shop = world.find(job.workplace);
    registry.get<lw::Inventory>(shop).set(lw::ItemKind::Food, 0);
    const bool town = registry.get<lw::Settlement>(world.find(registry.get<lw::Building>(shop).settlement))
                          .kind == lw::SettlementKind::Town;
    runUntil(world, lw::kRestockHour, 1);
    const int expected = static_cast<int>(
        std::lround(static_cast<float>(town ? 40 : 25) * lw::phaseRules(lw::WorldPhase::Panic).supply));
    CHECK(registry.get<lw::Inventory>(shop).count(lw::ItemKind::Food) == expected);
}

// Пока есть закон, кражу осуждает и тот, кому пострадавший чужой; без закона
// за чужого почти не обидно.
void testCrimeCondemned() {
    lw::World world(7, peaceConfig(30));
    entt::registry& registry = world.registry();
    const std::vector<entt::entity> idle = idlers(world);
    CHECK(idle.size() >= 4);
    if (idle.size() < 4) {
        return;
    }
    const entt::entity thief = idle[0];
    const entt::entity victim = idle[1];
    const entt::entity lawful = idle[2];
    const entt::entity lawless = idle[3];
    for (const entt::entity person : {thief, victim, lawful, lawless}) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Memories>(person) = {};
        registry.get<lw::Traits>(person) = {};
    }
    const lw::EntityId home = registry.get<lw::Person>(victim).home;
    lw::Account account;
    account.victim = idOf(world, victim);
    lw::experience(world, lawful, lw::MemoryKind::Robbed, idOf(world, thief), home, 3, account);
    lw::enterPhase(world, lw::WorldPhase::Collapse);
    lw::experience(world, lawless, lw::MemoryKind::Robbed, idOf(world, thief), home, 3, account);

    const lw::Memory* condemned = registry.get<lw::Memories>(lawful).find(
        lw::MemoryKind::Robbed, idOf(world, thief), idOf(world, victim));
    const lw::Memory* shrugged = registry.get<lw::Memories>(lawless).find(
        lw::MemoryKind::Robbed, idOf(world, thief), idOf(world, victim));
    CHECK(condemned != nullptr);
    CHECK(condemned != nullptr && (shrugged == nullptr || shrugged->strength < condemned->strength));
    const lw::Bond* grudge = registry.get<lw::Relations>(lawful).find(idOf(world, thief));
    const lw::Bond* indifferent = registry.get<lw::Relations>(lawless).find(idOf(world, thief));
    CHECK(grudge != nullptr && grudge->grudge > 0.0f);
    CHECK(grudge != nullptr && (indifferent == nullptr || indifferent->grudge < grudge->grudge));
    CHECK(world.chronicle().countOf(lw::ChronicleKind::Witnessed) == 1);
}

// Полицейский на службе заступается за любого: нападение на незнакомца для
// него — угроза, для прохожего — нет. Когда полиции не стало — и для него нет.
void testPoliceDefends() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    const entt::entity officer = workerOf(world, lw::Profession::PoliceOfficer);
    const std::vector<entt::entity> idle = idlers(world);
    CHECK(officer != entt::null && idle.size() >= 3);
    if (officer == entt::null || idle.size() < 3) {
        return;
    }
    const entt::entity attacker = idle[0];
    const entt::entity victim = idle[1];
    const entt::entity bystander = idle[2];
    const entt::entity defender = idle.size() > 3 ? idle[3] : entt::null;
    alwaysOnShift(registry, officer, traitsOf(1.0f));
    registry.get<lw::Mind>(officer).action = lw::Action::Work;
    registry.get<lw::Person>(officer).asleep = false;
    registry.get<lw::Person>(officer).indoors = false;
    for (const entt::entity person : {officer, bystander}) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Awareness>(person) = {};
    }
    const lw::Position at = registry.get<lw::Position>(officer);
    registry.get<lw::Position>(attacker) = {at.x + 15.0f, at.y};
    registry.get<lw::Position>(victim) = {at.x + 16.0f, at.y};
    registry.get<lw::Position>(bystander) = {at.x, at.y + 5.0f};
    registry.get<lw::Combat>(attacker).target = idOf(world, victim);
    for (const entt::entity observer : {officer, bystander}) {
        lw::Awareness& awareness = registry.get<lw::Awareness>(observer);
        awareness.contacts[awareness.count++] = {idOf(world, attacker), at.x + 15.0f, at.y,
                                                 world.tick(), true, lw::SoundKind::None};
    }
    CHECK(lw::policeOnDuty(world, officer));
    CHECK(lw::threatOf(world, officer).level > 0.25f);
    CHECK(scoreOf(world, officer, lw::Action::Fight) > 0.0f);
    CHECK(lw::threatOf(world, bystander).level == 0.0f);
    // Кто бьёт напавшего, заступается: для полицейского он не угроза, хоть и ближе.
    if (defender != entt::null) {
        registry.get<lw::Position>(defender) = {at.x + 8.0f, at.y};
        registry.get<lw::Combat>(defender).target = idOf(world, attacker);
        lw::Awareness& awareness = registry.get<lw::Awareness>(officer);
        awareness.contacts[awareness.count++] = {idOf(world, defender), at.x + 8.0f, at.y,
                                                 world.tick(), true, lw::SoundKind::None};
        CHECK(lw::threatOf(world, officer).nearest == idOf(world, attacker));
    }

    lw::enterPhase(world, lw::WorldPhase::Survival);
    CHECK(!lw::policeOnDuty(world, officer));
    CHECK(lw::threatOf(world, officer).level == 0.0f);
}

// Сюжетные линии (этап 12а.1): из записей летописи собираются вражда,
// предательство, заражение, группа и гонка за грузом — с завязкой, нарастанием
// и развязкой; события после развязки — эпилог той же линии.
void testStoryLines() {
    using K = lw::ChronicleKind;
    lw::World world(1, smallConfig(0));
    // Участники — просто номера: линии собираются из летописи, сущности не нужны.
    world.record(K::Stole, 20, 500, 3, 0, 10); // 20 обокрал дом 500, хозяин 10
    world.record(K::AssaultedBy, 10, 20);      // 20 напал на 10
    world.record(K::Avenged, 10, 20, 1);       // 10 отомстил
    world.record(K::Died, 20, 10);             // и 20 погиб от его руки
    world.record(K::Befriended, 30, 31);
    world.record(K::BetrayedBy, 30, 31, 2);
    world.record(K::Reconciled, 30, 31);
    world.record(K::Bitten, 40, 900);
    world.record(K::Feverish, 40);
    world.record(K::Died, 40, 900, static_cast<std::int32_t>(lw::DeathCause::Infection));
    world.record(K::GroupFormed, 50, 600);
    world.record(K::JoinedGroup, 51, 600, 1, 0, 50);
    world.record(K::EventBegan, 700, 5);
    world.record(K::LearnedOfEvent, 60, 700);
    world.record(K::Salvaged, 60, 700, 2, 1);

    lw::StoryTracker tracker;
    tracker.update(world);
    const std::vector<lw::Story>& stories = tracker.stories();
    CHECK(stories.size() == 5);
    if (stories.size() != 5) {
        return;
    }
    const lw::Story& feud = stories[0];
    CHECK(feud.kind == lw::StoryKind::Feud && feud.hero == 10 && feud.other == 20);
    CHECK(!feud.betrayal && !feud.active() && feud.ending == lw::StoryEnding::Revenge);
    CHECK(feud.beats.size() == 4); // кража, нападение, месть и смерть обидчика — эпилог
    CHECK(feud.beats[0].stage == lw::StoryStage::Setup);
    CHECK(feud.beats[1].stage == lw::StoryStage::Rising);

    const lw::Story& betrayal = stories[1];
    CHECK(betrayal.kind == lw::StoryKind::Feud && betrayal.betrayal);
    CHECK(betrayal.ending == lw::StoryEnding::Reconciled);

    const lw::Story& infection = stories[2];
    CHECK(infection.kind == lw::StoryKind::Infection && infection.hero == 40);
    CHECK(infection.active() && infection.stage == lw::StoryStage::Climax);

    const lw::Story& group = stories[3];
    CHECK(group.kind == lw::StoryKind::Group && group.hero == 50 && group.focus == 600);
    CHECK(group.cast.size() == 2 && group.stage == lw::StoryStage::Setup);

    const lw::Story& salvage = stories[4];
    CHECK(salvage.kind == lw::StoryKind::Salvage && salvage.focus == 700);
    CHECK(salvage.hero == 60 && salvage.stage == lw::StoryStage::Climax);

    // Дочитываются только новые записи.
    world.record(K::Turned, 40);
    world.record(K::GroupDisbanded, 600, 51);
    world.record(K::EventEnded, 700);
    tracker.update(world);
    CHECK(tracker.stories().size() == 5);
    CHECK(tracker.stories()[2].ending == lw::StoryEnding::Turned);
    CHECK(tracker.stories()[3].ending == lw::StoryEnding::Disbanded);
    CHECK(tracker.stories()[4].ending == lw::StoryEnding::Claimed);

    // Изгнание — вражда изгнанного с вожаком (группа уже распалась — без неё).
    world.record(K::Expelled, 50, 51, 0, 0, 600);
    tracker.update(world);
    CHECK(tracker.stories().size() == 6);
    CHECK(tracker.storiesOf(51) == (std::vector<std::uint32_t>{4, 6}));
    CHECK(lw::storyInterest(tracker.stories()[5], world.tick()) > 0.0f);
}

// Предвкушение (этап 12а.2): по состоянию мира видно, что вот-вот случится, —
// мститель идёт к обидчику, укушенный среди людей, двое идут к одному грузу;
// предвестие ведёт к своей линии и сбывается событием летописи.
void testForesight() {
    Field field(4);
    if (!field.open || field.people.size() != 4) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity avenger = field.people[0];
    const entt::entity offender = field.people[1];
    const entt::entity first = field.people[2];
    const entt::entity second = field.people[3];
    const auto setMind = [&](entt::entity person, lw::Action action, lw::EntityId target) {
        lw::Mind& mind = registry.emplace_or_replace<lw::Mind>(person);
        mind.action = action;
        mind.phase = lw::ActionPhase::Travel;
        mind.target = target;
        mind.spot = field.at(28);
    };
    field.place(avenger, 0);
    setMind(avenger, lw::Action::Avenge, field.id(offender));
    field.freeze(offender, 20);
    constexpr lw::EntityId kEvent = 777;
    field.place(first, 21); // рядом с обидчиком
    setMind(first, lw::Action::Salvage, kEvent);
    field.place(second, 10);
    setMind(second, lw::Action::Salvage, kEvent);
    registry.get<lw::Wounds>(offender).infection = 0.9f;

    const auto find = [](const std::vector<lw::Foresight>& all, lw::ForesightKind kind) {
        const auto it = std::find_if(all.begin(), all.end(),
                                     [kind](const lw::Foresight& f) { return f.kind == kind; });
        return it != all.end() ? *it : lw::Foresight{lw::ForesightKind::Count};
    };
    const std::vector<lw::Foresight> all = lw::foresee(field.world);
    for (std::size_t i = 1; i < all.size(); ++i) {
        CHECK(all[i - 1].interest >= all[i].interest);
    }
    const lw::Foresight revenge = find(all, lw::ForesightKind::Revenge);
    CHECK(revenge.hero == field.id(avenger) && revenge.other == field.id(offender));
    CHECK(std::abs(revenge.distance - 20.0f * lw::Map::kCellSize) < 1.0f);
    CHECK(revenge.eta > 0.0f && revenge.interest > 0.0f);

    const lw::Foresight turning = find(all, lw::ForesightKind::Turning);
    CHECK(turning.hero == field.id(offender) && turning.crowd >= 1);
    CHECK(turning.eta > 0.0f && turning.eta < lw::kInfectionHours * 300.0f);

    const lw::Foresight race = find(all, lw::ForesightKind::CacheRace);
    CHECK(race.focus == kEvent && race.crowd == 2);
    CHECK(race.hero == field.id(first) && race.other == field.id(second)); // первый ближе
    CHECK(race.eta < race.distance); // идёт быстрее метра в секунду

    // Ближе к кульминации — интереснее.
    field.place(avenger, 17);
    const lw::Foresight closer = find(lw::foresee(field.world), lw::ForesightKind::Revenge);
    CHECK(closer.same(revenge) && closer.eta < revenge.eta && closer.interest > revenge.interest);

    // Предвестие ведёт к своей линии: вражда обидчика с мстителем.
    using K = lw::ChronicleKind;
    field.world.record(K::AssaultedBy, field.id(avenger), field.id(offender));
    field.world.record(K::Bitten, field.id(offender), 900);
    lw::StoryTracker tracker;
    tracker.update(field.world);
    const lw::Story* feud = tracker.lineOf(revenge);
    CHECK(feud != nullptr && feud->kind == lw::StoryKind::Feud && feud->hero == field.id(avenger));
    const lw::Story* infection = tracker.lineOf(turning);
    CHECK(infection != nullptr && infection->kind == lw::StoryKind::Infection);
    CHECK(tracker.lineOf(race) == nullptr); // ивента в летописи нет

    // Сбылось: отомстил; кто-то из гонки забрал груз; встал зомби.
    const auto last = [&]() { return field.world.chronicle().events().back(); };
    field.world.record(K::Avenged, field.id(avenger), field.id(offender), 0);
    CHECK(lw::fulfilledBy(field.world, revenge, last()));
    CHECK(!lw::fulfilledBy(field.world, race, last()));
    field.world.record(K::Salvaged, field.id(second), kEvent, 2, 0);
    CHECK(lw::fulfilledBy(field.world, race, last()));
    field.world.record(K::Turned, field.id(offender));
    CHECK(lw::fulfilledBy(field.world, turning, last()));
    CHECK(!lw::fulfilledBy(field.world, revenge, last()));
}

} // namespace

// Главные герои (этап 12а.3): первые двое входят в состав по весу биографии,
// дальше — только через связь с уже знакомым героем; погибший и обратившийся
// выбывают, а опустевшее ядро снова пополняется. Итог не зависит от того, как
// часто звать update.
void testCast() {
    using K = lw::ChronicleKind;
    constexpr std::uint64_t kHour = lw::kTicksPerGameHour;
    lw::Chronicle chronicle;
    std::uint32_t stoleIndex = 0;
    const auto add = [&chronicle](std::uint64_t tick, K kind, lw::EntityId subject,
                                  lw::EntityId object = lw::kInvalidId, std::int32_t a = 0,
                                  lw::EntityId third = lw::kInvalidId) {
        chronicle.add({tick, kind, subject, object, a, 0, third});
        return static_cast<std::uint32_t>(chronicle.events().size() - 1);
    };
    for (const lw::EntityId id : {10, 20, 30, 40, 50}) {
        add(0, K::Settled, id, 500 + id);
    }
    add(10, K::GroupFormed, 10, 900);    // 10 — самый яркий
    add(20, K::Avenged, 10, 99, 1);      // 99 — не человек: веса ему нет
    add(30, K::Bitten, 30, 98);
    add(40, K::Feverish, 30);
    add(50, K::Stole, 30, 540, 0, 40);   // 30 обокрал дом 40
    add(kHour * 5 / 2, K::GroupFormed, 50, 901); // 50 ярок, но ни с кем из героев не связан
    add(kHour * 5 / 2 + 1, K::Deserted, 50, 901, 3, 51);
    stoleIndex = add(3 * kHour, K::Stole, 20, 510, 0, 10); // 20 обокрал дом героя 10
    add(3 * kHour + 1, K::Bitten, 20, 98);
    add(5 * kHour, K::Died, 10, 99);
    add(5 * kHour + 1, K::Died, 30, 98);
    add(5 * kHour + 2, K::Turned, 30);

    constexpr std::uint64_t kEnd = 10 * kHour;
    lw::CastTracker cast;
    cast.update(chronicle, kEnd);
    const std::vector<lw::Protagonist>& heroes = cast.heroes();
    CHECK(heroes.size() == 4);
    if (heroes.size() != 4) {
        return;
    }
    CHECK(heroes[0].id == 10 && heroes[0].since == kHour && heroes[0].introducedBy == lw::kInvalidId);
    CHECK(heroes[1].id == 30 && heroes[1].since == 2 * kHour);
    // Сложившийся сериал: 50 не связан с героями — ждёт; 20 входит через 10.
    CHECK(heroes[2].id == 20 && heroes[2].since == 4 * kHour);
    CHECK(heroes[2].introducedBy == 10 && heroes[2].via == stoleIndex);
    CHECK(heroes[0].status == lw::HeroStatus::Dead && heroes[0].ended == 5 * kHour);
    CHECK(heroes[1].status == lw::HeroStatus::Turned);
    // Живых героев меньше двух — 50 входит и без связи.
    CHECK(heroes[3].id == 50 && heroes[3].since == 6 * kHour &&
          heroes[3].introducedBy == lw::kInvalidId);
    CHECK(cast.living() == 2 && cast.isHero(20) && cast.isHero(50) && !cast.isHero(10));
    CHECK(cast.weight(99) == 0.0f && cast.link(20, 10) > 0.0f);

    // То же, если звать update после каждого кусочка летописи.
    lw::Chronicle partial;
    lw::CastTracker step;
    std::size_t next = 0;
    for (std::uint64_t now = 0; now <= kEnd; now += 700) {
        while (next < chronicle.events().size() && chronicle.events()[next].tick < now) {
            partial.add(chronicle.events()[next++]);
        }
        step.update(partial, now);
    }
    step.update(chronicle, kEnd);
    CHECK(step.heroes().size() == heroes.size());
    for (std::size_t i = 0; i < std::min(step.heroes().size(), heroes.size()); ++i) {
        CHECK(step.heroes()[i].id == heroes[i].id && step.heroes()[i].since == heroes[i].since &&
              step.heroes()[i].introducedBy == heroes[i].introducedBy);
    }
}

// --- Семьи и прошлое (этап 7.3) ------------------------------------------------

// План домов: ровно столько людей, родство взаимно, у каждого ребёнка —
// взрослый опекун из того же дома, родители старше детей, часть родни живёт
// отдельно; один seed — один план.
void testHouseholdPlan() {
    lw::Rng rng(11);
    const lw::HouseholdPlan plan = lw::planHouseholds(rng, 300);
    CHECK(plan.people.size() == 300);
    CHECK(plan.guardian.size() == plan.people.size());
    std::size_t minors = 0;
    std::size_t elders = 0;
    std::size_t apart = 0;
    bool symmetric = true;
    bool parentsOlder = true;
    bool adultsMarry = true;
    std::vector<int> relatives(plan.people.size(), 0);
    for (const lw::PlannedKin& link : plan.kin) {
        ++relatives[static_cast<std::size_t>(link.a)];
        const bool back = std::any_of(plan.kin.begin(), plan.kin.end(), [&](const lw::PlannedKin& k) {
            return k.a == link.b && k.b == link.a && k.kind == lw::inverseKin(link.kind);
        });
        symmetric = symmetric && back;
        const lw::PlannedPerson& a = plan.people[static_cast<std::size_t>(link.a)];
        const lw::PlannedPerson& b = plan.people[static_cast<std::size_t>(link.b)];
        if (link.kind == lw::KinKind::Parent) {
            parentsOlder = parentsOlder && b.age >= a.age + 16;
        }
        if (link.kind == lw::KinKind::Spouse) {
            adultsMarry = adultsMarry && a.age >= lw::kAdultAge && b.age >= lw::kAdultAge;
        }
        apart += a.household != b.household ? 1 : 0;
    }
    CHECK(symmetric);
    CHECK(parentsOlder);
    CHECK(adultsMarry);
    CHECK(apart > 0);
    bool guarded = true;
    for (std::size_t i = 0; i < plan.people.size(); ++i) {
        const lw::PlannedPerson& p = plan.people[i];
        CHECK(relatives[i] <= static_cast<int>(lw::Kin::kMaxRelatives));
        elders += p.age >= 65 ? 1 : 0;
        if (p.age >= lw::kAdultAge) {
            guarded = guarded && plan.guardian[i] == -1;
            continue;
        }
        ++minors;
        const int g = plan.guardian[i];
        guarded = guarded && g >= 0 && plan.people[static_cast<std::size_t>(g)].age >= lw::kAdultAge &&
                  plan.people[static_cast<std::size_t>(g)].household == p.household;
    }
    CHECK(guarded);
    CHECK(minors > 30 && minors < 120);
    CHECK(elders > 10);
    CHECK(plan.households.size() > 80 && plan.households.size() < 160);

    lw::Rng again(11);
    const lw::HouseholdPlan twin = lw::planHouseholds(again, 300);
    CHECK(twin.people.size() == plan.people.size() && twin.kin.size() == plan.kin.size());
    bool same = twin.lineages == plan.lineages;
    for (std::size_t i = 0; same && i < plan.people.size(); ++i) {
        same = twin.people[i].age == plan.people[i].age && twin.people[i].lineage == plan.people[i].lineage;
    }
    CHECK(same);
}

// Семьи в мире: у всех возраст и родня, родня знакома всю жизнь, ребёнок живёт
// с опекуном, супруги носят одну фамилию; школьники — городские дети, коллеги
// знакомы, у части людей есть прошлое.
void testFamiliesInWorld() {
    lw::World world(7, peaceConfig(300));
    const entt::registry& registry = world.registry();
    std::size_t kinBonds = 0;
    std::size_t colleagues = 0;
    std::size_t past = 0;
    std::size_t minors = 0;
    bool allAged = true;
    bool kinKnown = true;
    bool withGuardian = true;
    bool spousesNamed = true;
    for (const auto [entity, id, person, relations] :
         registry.view<const lw::Id, const lw::Person, const lw::Relations>().each()) {
        allAged = allAged && registry.all_of<lw::Age, lw::Kin>(entity);
        const lw::Kin& kin = registry.get<lw::Kin>(entity);
        for (std::size_t i = 0; i < kin.count; ++i) {
            const lw::Bond* bond = relations.find(kin.relatives[i].id);
            kinKnown = kinKnown && bond != nullptr && bond->origin == lw::BondOrigin::Kin &&
                       bond->familiarity >= 0.85f;
            const entt::entity other = world.find(kin.relatives[i].id);
            kinKnown = kinKnown && lw::kinOf(registry, other, id.value) == lw::inverseKin(kin.relatives[i].kind);
            if (kin.relatives[i].kind == lw::KinKind::Spouse) {
                spousesNamed = spousesNamed && registry.get<lw::Name>(entity).last == registry.get<lw::Name>(other).last;
            }
        }
        for (std::size_t i = 0; i < relations.count; ++i) {
            kinBonds += relations.bonds[i].origin == lw::BondOrigin::Kin ? 1 : 0;
            colleagues += relations.bonds[i].origin == lw::BondOrigin::Colleague ? 1 : 0;
            past += relations.bonds[i].origin == lw::BondOrigin::OldFriend ||
                            relations.bonds[i].origin == lw::BondOrigin::OldFeud ||
                            relations.bonds[i].origin == lw::BondOrigin::ExPartner
                        ? 1
                        : 0;
        }
        if (lw::isMinor(registry, entity)) {
            ++minors;
            const entt::entity guardian = world.find(kin.guardian);
            withGuardian = withGuardian && guardian != entt::null && !lw::isMinor(registry, guardian) &&
                           registry.get<lw::Person>(guardian).home == person.home;
            const auto* job = registry.try_get<lw::Job>(entity);
            withGuardian = withGuardian && (job == nullptr || job->profession == lw::Profession::Pupil);
        }
    }
    CHECK(allAged);
    CHECK(kinKnown);
    CHECK(withGuardian);
    CHECK(spousesNamed);
    CHECK(kinBonds > 100);
    CHECK(colleagues > 0);
    CHECK(past > 0);
    CHECK(minors > 0);
    // Один seed — одни и те же семьи.
    lw::World twin(7, peaceConfig(300));
    CHECK(world.stateHash() == twin.stateHash());
}

// Родню не забывают, даже если давно не виделись, и новое знакомство её не
// вытесняет; код родства в летописи читается обратно.
void testKinRemembered() {
    lw::World world(7, peaceConfig(120));
    entt::registry& registry = world.registry();
    entt::entity self = entt::null;
    lw::EntityId relative = lw::kInvalidId;
    for (const auto [entity, id, kin] : registry.view<const lw::Id, const lw::Kin>().each()) {
        if (kin.count > 0 && (self == entt::null || id.value < registry.get<lw::Id>(self).value)) {
            self = entity;
            relative = kin.relatives[0].id;
        }
    }
    CHECK(self != entt::null);
    if (self == entt::null) {
        return;
    }
    lw::Bond* bond = registry.get<lw::Relations>(self).find(relative);
    CHECK(bond != nullptr);
    if (bond == nullptr) {
        return;
    }
    // Давно не виделись: месяц без встреч.
    const float familiarity = bond->familiarity;
    for (int step = 0; step < 30 * 24; ++step) {
        for (int i = 0; i < 1000; ++i) {
            world.step(); // «давно» — по тикам мира
        }
        if (world.tick() > 3 * lw::kTicksPerGameDay) {
            break;
        }
    }
    bond = registry.get<lw::Relations>(self).find(relative);
    bond->seen = 0;
    lw::updateRelations(world, 30.0f * 24.0f * 60.0f);
    CHECK(registry.get<lw::Relations>(self).find(relative)->familiarity >= familiarity - 1e-6f);

    // Связи полны и все — родня: незнакомца не запомнить.
    lw::Relations full;
    for (std::size_t i = 0; i < lw::Relations::kMaxBonds; ++i) {
        lw::Bond* kin = lw::acquireBond(full, 1000 + i, 0.0f, 0);
        kin->origin = lw::BondOrigin::Kin;
    }
    CHECK(lw::acquireBond(full, 5000, 0.0f, 0) == nullptr);

    lw::KinKind kind = lw::KinKind::Count;
    bool female = true;
    CHECK(!lw::decodeKin(0, kind, female));
    CHECK(lw::decodeKin(1 + 2 * static_cast<int>(lw::KinKind::Child) + 1, kind, female) &&
          kind == lw::KinKind::Child && female);
}

// Дети: не охотятся, не воруют, не мстят и не торгуют; младший не дерётся с
// зомби, а бежит. Опекун за своего ребёнка дерётся охотнее, чем за чужого.
void testChildBehaviour() {
    Field field(3);
    if (!field.open || field.people.size() != 3) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity child = field.people[0];
    const entt::entity parent = field.people[1];
    const entt::entity stranger = field.people[2];
    registry.get<lw::Age>(child).years = 9;
    for (const entt::entity person : field.people) {
        registry.get<lw::Traits>(person) = {};
        registry.get<lw::Weapon>(person) = {lw::WeaponKind::Rifle, 10};
        registry.get<lw::Needs>(person).hunger = 0.9f;
        registry.get<lw::Inventory>(person) = {};
    }
    const lw::ActionScores calm = lw::evaluateActions(field.world, child);
    CHECK(calm[static_cast<std::size_t>(lw::Action::Hunt)] == 0.0f);
    CHECK(calm[static_cast<std::size_t>(lw::Action::Steal)] == 0.0f);
    CHECK(calm[static_cast<std::size_t>(lw::Action::Avenge)] == 0.0f);
    CHECK(calm[static_cast<std::size_t>(lw::Action::Trade)] == 0.0f);

    // Зомби гонится за ребёнком.
    field.place(child, 10);
    field.place(parent, 8);
    field.place(stranger, 12);
    const entt::entity zombie = field.zombie(13);
    lw::Zombie& chaser = registry.get<lw::Zombie>(zombie);
    chaser.state = lw::ZombieState::Chase;
    chaser.target = field.id(child);
    for (const entt::entity person : field.people) {
        letSee(field, person, zombie);
    }
    const lw::ActionScores scared = lw::evaluateActions(field.world, child);
    CHECK(scared[static_cast<std::size_t>(lw::Action::Fight)] == 0.0f);
    CHECK(scared[static_cast<std::size_t>(lw::Action::Flee)] > 0.0f);
    const float strangerFight = lw::evaluateActions(field.world, parent)[static_cast<std::size_t>(lw::Action::Fight)];
    lw::Kin& kin = registry.get<lw::Kin>(child);
    kin.add(field.id(parent), lw::KinKind::Parent);
    kin.guardian = field.id(parent);
    registry.get<lw::Kin>(parent).add(field.id(child), lw::KinKind::Child);
    const float parentFight = lw::evaluateActions(field.world, parent)[static_cast<std::size_t>(lw::Action::Fight)];
    CHECK(strangerFight > 0.0f);
    CHECK(parentFight > 1.5f * strangerFight);
}

// Сирота: погибли родители — летопись «осиротел», опекуном становится родня
// (здесь — дед из другого дома), ребёнок переезжает к нему. Без родни берёт
// добрый знакомый взрослый; ребёнок ему обязан.
void testOrphanTakenIn() {
    Field field(5);
    if (!field.open || field.people.size() != 5) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity child = field.people[0];
    const entt::entity mother = field.people[1];
    const entt::entity grandpa = field.people[2];
    const entt::entity neighbour = field.people[3];
    registry.get<lw::Age>(child).years = 8;
    registry.get<lw::Age>(grandpa).years = 70;
    const lw::EntityId home = registry.get<lw::Person>(mother).home;
    registry.get<lw::Person>(child).home = home;
    const lw::EntityId other = nearbyHome(field.world, home, 2000.0f);
    CHECK(other != lw::kInvalidId);
    registry.get<lw::Person>(grandpa).home = other;
    const auto relate = [&](entt::entity a, entt::entity b, lw::KinKind kind) {
        registry.get<lw::Kin>(a).add(field.id(b), kind);
        registry.get<lw::Kin>(b).add(field.id(a), lw::inverseKin(kind));
    };
    relate(child, mother, lw::KinKind::Parent);
    relate(child, grandpa, lw::KinKind::Grandparent);
    relate(mother, grandpa, lw::KinKind::Parent);
    registry.get<lw::Kin>(child).guardian = field.id(mother);

    lw::updateFamilies(field.world);
    CHECK(registry.get<lw::Kin>(child).guardian == field.id(mother)); // мать жива
    CHECK(field.count(lw::ChronicleKind::Orphaned) == 0);

    lw::die(field.world, mother, lw::DeathCause::Wounds, lw::kInvalidId);
    lw::updateFamilies(field.world);
    CHECK(field.count(lw::ChronicleKind::Orphaned) == 1);
    CHECK(field.count(lw::ChronicleKind::TookIn) == 1);
    CHECK(registry.get<lw::Kin>(child).guardian == field.id(grandpa));
    CHECK(registry.get<lw::Person>(child).home == other);
    // Повторно — ничего нового.
    lw::updateFamilies(field.world);
    CHECK(field.count(lw::ChronicleKind::Orphaned) == 1 && field.count(lw::ChronicleKind::TookIn) == 1);

    // Деда нет — берёт сосед, который ребёнка знает и добр к нему.
    lw::die(field.world, grandpa, lw::DeathCause::Wounds, lw::kInvalidId);
    registry.get<lw::Traits>(neighbour).altruism = 0.9f;
    setBond(registry, neighbour, field.id(child), 0.8f, 0.6f, 0.5f);
    CHECK(lw::chooseGuardian(field.world, child) == field.id(neighbour));
    lw::updateFamilies(field.world);
    CHECK(registry.get<lw::Kin>(child).guardian == field.id(neighbour));
    CHECK(registry.get<lw::Person>(child).home == registry.get<lw::Person>(neighbour).home);
    const lw::Bond* owed = registry.get<lw::Relations>(child).find(field.id(neighbour));
    CHECK(owed != nullptr && owed->debt > 0.0f);
    CHECK(field.count(lw::ChronicleKind::TookIn) == 2);
    CHECK(field.count(lw::ChronicleKind::Orphaned) == 1); // сиротой стал однажды
}

// Семья в группе: с роднёй группу не основывают; вступивший приводит супруга,
// ребёнок идёт за опекуном и уходит вместе с ним; ребёнок не вожак.
void testFamilyInGroup() {
    Field field(4);
    if (!field.open || field.people.size() != 4) {
        return;
    }
    entt::registry& registry = field.world.registry();
    const entt::entity leader = field.people[0];
    const entt::entity husband = field.people[1];
    const entt::entity wife = field.people[2];
    const entt::entity son = field.people[3];
    for (const entt::entity person : field.people) {
        registry.get<lw::Relations>(person) = {};
        registry.get<lw::Traits>(person) = {};
        registry.get<lw::Traits>(person).leadership = 0.3f;
    }
    registry.get<lw::Traits>(husband).leadership = 0.8f;
    registry.get<lw::Age>(son).years = 12;
    registry.get<lw::Traits>(son).leadership = 1.0f;
    const lw::EntityId familyHome = registry.get<lw::Person>(husband).home;
    registry.get<lw::Person>(wife).home = familyHome;
    registry.get<lw::Person>(son).home = familyHome;
    const lw::EntityId base = nearbyHome(field.world, familyHome, 1000.0f);
    CHECK(base != lw::kInvalidId);
    registry.get<lw::Person>(leader).home = base;
    const auto relate = [&](entt::entity a, entt::entity b, lw::KinKind kind) {
        registry.get<lw::Kin>(a).add(field.id(b), kind);
        registry.get<lw::Kin>(b).add(field.id(a), lw::inverseKin(kind));
    };
    relate(husband, wife, lw::KinKind::Spouse);
    relate(son, husband, lw::KinKind::Parent);
    relate(son, wife, lw::KinKind::Parent);
    registry.get<lw::Kin>(son).guardian = field.id(husband);

    // Супруги — взаимные друзья, но это семья, а не группа.
    befriend(registry, field, husband, wife);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, husband) == nullptr);

    // Друг не из родни — группа; жена и сын переезжают с мужем.
    befriend(registry, field, husband, leader);
    registry.get<lw::Traits>(leader).leadership = 0.9f;
    lw::updateGroups(field.world);
    const lw::Group* group = groupOf(field.world, husband);
    CHECK(group != nullptr);
    if (group == nullptr) {
        return;
    }
    CHECK(groupOf(field.world, wife) == group && groupOf(field.world, son) == group);
    CHECK(group->members.size() == 4);
    CHECK(group->leader == field.id(leader));
    CHECK(registry.get<lw::Person>(son).home == base);
    CHECK(lw::authorityOf(field.world, *group, field.id(son)) == 0.0f);

    // Муж невзлюбил вожака и ушёл — сын с ним, жена осталась.
    setBond(registry, husband, field.id(leader), 0.9f, -0.6f, -0.2f);
    lw::updateGroups(field.world);
    CHECK(groupOf(field.world, husband) == nullptr);
    CHECK(groupOf(field.world, son) == nullptr);
    CHECK(registry.get<lw::Person>(son).home == familyHome);
    CHECK(groupOf(field.world, wife) != nullptr);
}

// --- Реакция общества (этап 7.6) ---------------------------------------------

// Взрослый житель с домом, не первый попавшийся: skip — сколько пропустить.
entt::entity adultResident(const lw::World& world, int skip = 0) {
    std::vector<std::pair<lw::EntityId, entt::entity>> adults;
    for (const auto [entity, id, person] : world.registry().view<const lw::Id, const lw::Person>().each()) {
        if (!lw::isMinor(world.registry(), entity) && person.home != lw::kInvalidId) {
            adults.emplace_back(id.value, entity);
        }
    }
    std::sort(adults.begin(), adults.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return static_cast<std::size_t>(skip) < adults.size() ? adults[static_cast<std::size_t>(skip)].second
                                                          : entt::entity{entt::null};
}

// Тревога: растёт, не убывает; новая стадия — запись в летописи. Рассказ
// слабеет с пересказом: чужому верят меньше, чем родне и другу.
void testAlarmRules() {
    lw::World world(7, peaceConfig(40));
    entt::registry& registry = world.registry();
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    const entt::entity a = adultResident(world, 0);
    const entt::entity b = adultResident(world, 5);
    CHECK(a != entt::null && b != entt::null && registry.all_of<lw::Alarm>(a));
    if (a == entt::null || b == entt::null) {
        return;
    }
    CHECK(lw::alarmStage(0.1f) == lw::AlarmStage::Calm && lw::alarmStage(0.3f) == lw::AlarmStage::Uneasy &&
          lw::alarmStage(0.6f) == lw::AlarmStage::Alarmed && lw::alarmStage(0.9f) == lw::AlarmStage::Terrified);
    CHECK(lw::alarmWorry(0.2f) == 0.0f && lw::alarmWorry(1.0f) == 1.0f);
    const std::size_t before = world.chronicle().countOf(lw::ChronicleKind::Alarmed);
    CHECK(lw::raiseAlarm(world, a, lw::clueLevel(lw::AlarmClue::Zombie), lw::AlarmClue::Zombie, lw::kInvalidId));
    CHECK(!lw::raiseAlarm(world, a, 0.4f, lw::AlarmClue::Told, idOf(world, b))); // ниже — ничего
    CHECK(world.chronicle().countOf(lw::ChronicleKind::Alarmed) == before + 1);
    const lw::Alarm& alarm = registry.get<lw::Alarm>(a);
    CHECK(alarm.level >= lw::kTerrifiedLevel && alarm.clue == lw::AlarmClue::Zombie && alarm.since == world.tick());

    // Рассказ: чужому — kStrangerCredibility, другу — kCloseCredibility.
    lw::Relations& theirs = registry.get<lw::Relations>(b);
    if (lw::Bond* bond = theirs.find(idOf(world, a))) {
        *bond = {};
        bond->other = idOf(world, a);
    }
    lw::Bond* bond = theirs.find(idOf(world, a));
    if (bond == nullptr && theirs.count < lw::Relations::kMaxBonds) {
        theirs.bonds[theirs.count] = {};
        theirs.bonds[theirs.count].other = idOf(world, a);
        bond = &theirs.bonds[theirs.count++];
    }
    CHECK(bond != nullptr);
    if (bond == nullptr) {
        return;
    }
    bond->familiarity = 0.0f;
    bond->flags = 0;
    const float stranger = lw::alarmToShare(world, a, b);
    CHECK(std::abs(stranger - alarm.level * lw::kStrangerCredibility) < 1e-4f);
    bond->familiarity = 1.0f;
    bond->affinity = 0.8f;
    bond->flags = lw::Bond::kFriend;
    const float friendly = lw::alarmToShare(world, a, b);
    CHECK(std::abs(friendly - alarm.level * lw::kCloseCredibility) < 1e-4f && friendly > stranger);
    // Кто знает не меньше, тому рассказывать нечего.
    registry.get<lw::Alarm>(b).level = 0.95f;
    CHECK(lw::alarmToShare(world, a, b) == 0.0f);
    // В мирное время о беде не говорит никто: тревоги нет ни у кого.
    lw::World calm(7, peaceConfig(40));
    runMinutes(calm, 60);
    float most = 0.0f;
    for (const auto [entity, person, other] : calm.registry().view<const lw::Person, const lw::Alarm>().each()) {
        most = std::max(most, other.level);
    }
    CHECK(most == 0.0f);
}

// Своими глазами: зомби рядом — «мёртвые встают»; потом рассказывают другим.
void testAlarmFromEvidence() {
    lw::World world(7, peaceConfig(40));
    lw::World twin(7, peaceConfig(40));
    entt::registry& registry = world.registry();
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    lw::enterPhase(twin, lw::WorldPhase::Outbreak);
    // Скрытый нулевой пациент тревоги не поднимает: никто ничего не заметил.
    runMinutes(world, 30);
    runMinutes(twin, 30);
    float most = 0.0f;
    for (const auto [entity, person, alarm] : registry.view<const lw::Person, const lw::Alarm>().each()) {
        most = std::max(most, alarm.level);
    }
    CHECK(most < lw::kUneasyLevel);
    // Зомби у двери взрослого, который не спит и не дома.
    const entt::entity witness = adultResident(world, 3);
    CHECK(witness != entt::null);
    if (witness == entt::null) {
        return;
    }
    for (lw::World* w : {&world, &twin}) {
        entt::registry& r = w->registry();
        const entt::entity person = adultResident(*w, 3);
        r.get<lw::Person>(person).asleep = false;
        r.get<lw::Person>(person).indoors = false;
        const lw::Position at = r.get<lw::Position>(person);
        const auto near = w->nav().nearestPassable(lw::Map::cellAt(at.x + 8.0f, at.y), 4);
        lw::Rng rng(99);
        if (near) {
            lw::createZombie(*w, *near, rng);
        }
        w->syncSpatial();
    }
    runMinutes(world, 3);
    runMinutes(twin, 3);
    const lw::Alarm& alarm = registry.get<lw::Alarm>(witness);
    if (registry.all_of<lw::Person>(witness)) {
        CHECK(alarm.level >= lw::kTerrifiedLevel && alarm.clue == lw::AlarmClue::Zombie);
    }
    bool recorded = false;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        recorded = recorded || (event.kind == lw::ChronicleKind::Alarmed && event.subject == idOf(world, witness) &&
                                event.b == static_cast<std::int32_t>(lw::AlarmClue::Zombie));
    }
    CHECK(recorded);
    CHECK(world.stateHash() == twin.stateHash());
}

// Тревога меняет решения: меньше гуляют, к больному чужому не идут, еды держат
// больше; страх растёт.
void testAlarmBehaviour() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    clearZombies(world);
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    runUntil(world, 11, 0);
    const entt::entity person = adultResident(world, 2);
    CHECK(person != entt::null);
    if (person == entt::null) {
        return;
    }
    registry.get<lw::Needs>(person) = {};
    registry.get<lw::Needs>(person).loneliness = 0.8f;
    const float wander = scoreOf(world, person, lw::Action::Wander);
    const float socialize = scoreOf(world, person, lw::Action::Socialize);
    lw::raiseAlarm(world, person, 0.95f, lw::AlarmClue::Zombie, lw::kInvalidId);
    CHECK(scoreOf(world, person, lw::Action::Wander) < 0.5f * wander + 1e-4f);
    CHECK(scoreOf(world, person, lw::Action::Socialize) < socialize + 1e-4f);
    // Страх тянется вверх и без зомби рядом.
    const float fear = registry.get<lw::Needs>(person).fear;
    runMinutes(world, 60);
    if (registry.all_of<lw::Person>(person)) {
        CHECK(registry.get<lw::Needs>(person).fear > fear);
    }
}

// Карантин: в поселении больные — власть закрывает его, сообщает по радио;
// жители не выходят за кордон, случаев нет сутки — снимают.
void testQuarantine() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    clearZombies(world);
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    CHECK(lw::authorityPresent(world));
    // Двое заметно больных жителей одного поселения.
    const entt::entity first = adultResident(world, 0);
    CHECK(first != entt::null);
    if (first == entt::null) {
        return;
    }
    const lw::EntityId settlement =
        registry.get<lw::Building>(world.find(registry.get<lw::Person>(first).home)).settlement;
    std::vector<entt::entity> sick;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        const entt::entity home = world.find(person.home);
        if (home != entt::null && registry.get<lw::Building>(home).settlement == settlement && sick.size() < 2) {
            sick.push_back(entity);
        }
    }
    CHECK(sick.size() == 2);
    // Нулевого пациента — вылечить: зараза только та, что поставил тест.
    for (const auto [entity, person, wounds] : registry.view<const lw::Person, lw::Wounds>().each()) {
        wounds = {};
    }
    for (const entt::entity entity : sick) {
        lw::Wounds& wounds = registry.get<lw::Wounds>(entity);
        wounds.infection = 0.05f;
        wounds.bites = 1;
        wounds.route = lw::InfectionRoute::Bite;
    }
    CHECK(lw::quarantineCases(world, settlement) == 2);
    runUntil(world, (lw::calendarAt(world.tick()).hour + 1) % 24, 1);
    const entt::entity place = world.find(settlement);
    CHECK(registry.all_of<lw::Quarantine>(place));
    CHECK(world.chronicle().countOf(lw::ChronicleKind::QuarantineDeclared) == 1);
    CHECK(lw::scenarioOf(world)->broadcast >= static_cast<std::uint8_t>(lw::BroadcastKind::Epidemic));
    const lw::Settlement& info = registry.get<lw::Settlement>(place);
    CHECK(lw::quarantineAt(world, info.center) == settlement);
    CHECK(!lw::cordonExits(world, settlement).empty());
    // Больных нет — через сутки кордон снят.
    for (const entt::entity entity : sick) {
        if (registry.all_of<lw::Wounds>(entity)) {
            registry.get<lw::Wounds>(entity) = {};
        }
    }
    for (const auto [entity, person, wounds] : registry.view<const lw::Person, lw::Wounds>().each()) {
        wounds = {};
    }
    runMinutes(world, 26 * 60);
    if (lw::worldPhase(world) == lw::WorldPhase::Outbreak && lw::quarantineCases(world, settlement) == 0) {
        CHECK(!registry.all_of<lw::Quarantine>(place));
        CHECK(world.chronicle().countOf(lw::ChronicleKind::QuarantineLifted) >= 1);
    }
}

// Эвакуация: с паникой в городе пункт и солдаты; колонна увозит ждущих — семью
// с детьми вместе, родитель без детей не уезжает; эвакуированные не потеряны.
// С коллапсом армия уходит, кто-то из солдат может остаться.
void testEvacuation() {
    lw::World world(7, peaceConfig(60));
    lw::World twin(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    clearZombies(world);
    clearZombies(twin);
    lw::enterPhase(world, lw::WorldPhase::Panic);
    lw::enterPhase(twin, lw::WorldPhase::Panic);
    std::vector<lw::EntityId> points;
    for (const auto [entity, id, point] : registry.view<const lw::Id, const lw::EvacuationPoint>().each()) {
        points.push_back(id.value);
    }
    CHECK(!points.empty() && lw::evacuationOpen(world));
    int soldiers = 0;
    for (const auto [entity, person, job] : registry.view<const lw::Person, const lw::Job>().each()) {
        soldiers += job.profession == lw::Profession::Soldier ? 1 : 0;
    }
    CHECK(soldiers == static_cast<int>(points.size()) * lw::kSoldiersPerPoint);
    CHECK(lw::scenarioOf(world)->broadcast == static_cast<std::uint8_t>(lw::BroadcastKind::Evacuation));
    if (points.empty()) {
        return;
    }
    const lw::EntityId point = points.front();
    const lw::Cell door = registry.get<lw::Building>(world.find(point)).door;
    // Ребёнок и его опекун — у пункта; одинокий взрослый — тоже.
    entt::entity child = entt::null;
    for (const auto [entity, id, person, kin] : registry.view<const lw::Id, const lw::Person, const lw::Kin>().each()) {
        if (lw::isMinor(registry, entity) && world.find(kin.guardian) != entt::null && child == entt::null) {
            child = entity;
        }
    }
    CHECK(child != entt::null);
    if (child == entt::null) {
        return;
    }
    const entt::entity guardian = world.find(registry.get<lw::Kin>(child).guardian);
    entt::entity single = entt::null;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        if (lw::isMinor(registry, entity) || entity == guardian || lw::isSoldier(registry, entity)) {
            continue;
        }
        bool wards = false;
        for (const auto [other, kin] : registry.view<const lw::Kin>().each()) {
            wards = wards || kin.guardian == id.value;
        }
        if (!wards && single == entt::null) {
            single = entity;
        }
    }
    CHECK(single != entt::null);
    const auto wait = [&](entt::entity entity) {
        registry.get<lw::Position>(entity) = {lw::Map::cellCenter(door.x), lw::Map::cellCenter(door.y)};
        lw::Mind& mind = registry.get<lw::Mind>(entity);
        mind.action = lw::Action::Evacuate;
        mind.phase = lw::ActionPhase::Perform;
        mind.target = point;
        mind.spot = door;
        registry.get<lw::Wounds>(entity) = {};
    };
    // Все прочие дети опекуна — тоже у пункта, кроме этого ребёнка.
    wait(guardian);
    wait(single);
    for (const auto [entity, id, person, kin] : registry.view<const lw::Id, const lw::Person, const lw::Kin>().each()) {
        if (kin.guardian == idOf(world, guardian) && entity != child) {
            wait(entity);
        }
    }
    world.syncSpatial();
    const int population = lw::scenarioStats(world).population;
    const int lostBefore = lw::scenarioStats(world).lost;
    lw::departConvoy(world, point);
    CHECK(registry.all_of<lw::Evacuated>(single) && !registry.all_of<lw::Person>(single));
    CHECK(registry.all_of<lw::Person>(guardian)); // без ребёнка не уехал
    // Ребёнок пришёл — семья уезжает вместе.
    wait(child);
    world.syncSpatial();
    lw::departConvoy(world, point);
    CHECK(registry.all_of<lw::Evacuated>(guardian) && registry.all_of<lw::Evacuated>(child));
    CHECK(registry.all_of<lw::Name>(child) && registry.all_of<lw::Kin>(child)); // прошлое остаётся
    const lw::ScenarioStats stats = lw::scenarioStats(world);
    CHECK(stats.population == population && stats.lost == lostBefore && stats.evacuated >= 3);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::Evacuated) >= 3);
    // Мир живёт дальше без них.
    runMinutes(world, 30);

    // Коллапс: армия уходит, пункты закрыты, последняя передача.
    lw::enterPhase(world, lw::WorldPhase::Collapse);
    CHECK(!lw::evacuationOpen(world));
    CHECK(world.chronicle().countOf(lw::ChronicleKind::EvacuationEnded) == points.size());
    int left = 0;
    for (const lw::ChronicleEvent& event : world.chronicle().events()) {
        left += event.kind == lw::ChronicleKind::Evacuated && event.a == 1 ? 1 : 0;
    }
    CHECK(left + static_cast<int>(world.chronicle().countOf(lw::ChronicleKind::StayedBehind)) <= soldiers);
    CHECK(lw::scenarioStats(world).soldiers == static_cast<int>(world.chronicle().countOf(lw::ChronicleKind::StayedBehind)));
    CHECK(lw::scenarioOf(world)->broadcast == static_cast<std::uint8_t>(lw::BroadcastKind::Farewell));
    CHECK(registry.view<const lw::Quarantine>().empty());

    // Двойник без вмешательства: тот же seed и те же шаги — тот же мир.
    runMinutes(twin, 60);
    lw::World third(7, peaceConfig(60));
    clearZombies(third);
    lw::enterPhase(third, lw::WorldPhase::Panic);
    runMinutes(third, 60);
    CHECK(twin.stateHash() == third.stateHash());
}

// К своим: встревоженный опекун идёт за ребёнком, который далеко от дома;
// спокойный — нет. Ребёнок сам «к своим» не идёт.
void testReunite() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    clearZombies(world);
    lw::enterPhase(world, lw::WorldPhase::Outbreak);
    runUntil(world, 11, 0);
    entt::entity child = entt::null;
    for (const auto [entity, id, person, kin] : registry.view<const lw::Id, const lw::Person, const lw::Kin>().each()) {
        if (lw::isMinor(registry, entity) && world.find(kin.guardian) != entt::null && child == entt::null &&
            registry.all_of<lw::Person>(world.find(kin.guardian))) {
            child = entity;
        }
    }
    CHECK(child != entt::null);
    if (child == entt::null) {
        return;
    }
    const entt::entity guardian = world.find(registry.get<lw::Kin>(child).guardian);
    // Ребёнок — за полкилометра от дома и от опекуна.
    const lw::Position home = registry.get<lw::Position>(guardian);
    const auto far = world.nav().nearestPassable(lw::Map::cellAt(home.x + 500.0f, home.y + 200.0f), 30);
    CHECK(far.has_value());
    if (!far) {
        return;
    }
    registry.get<lw::Position>(child) = {lw::Map::cellCenter(far->x), lw::Map::cellCenter(far->y)};
    world.syncSpatial();
    CHECK(scoreOf(world, guardian, lw::Action::Reunite) == 0.0f);
    lw::raiseAlarm(world, guardian, 0.9f, lw::AlarmClue::Broadcast, lw::kInvalidId);
    lw::raiseAlarm(world, child, 0.9f, lw::AlarmClue::Broadcast, lw::kInvalidId);
    CHECK(scoreOf(world, guardian, lw::Action::Reunite) > 0.3f);
    CHECK(scoreOf(world, child, lw::Action::Reunite) == 0.0f);
}

// Кордон карантина (этап 7.6): житель деревни на карантине почти не идёт к
// пункту эвакуации в городе; смелый и вспыльчивый — охотнее послушного.
void testCordon() {
    lw::World world(7, peaceConfig(60));
    entt::registry& registry = world.registry();
    clearZombies(world);
    lw::enterPhase(world, lw::WorldPhase::Panic);
    runUntil(world, 11, 0);
    // Взрослый житель деревни (не солдат), до пункта — не дальше 7 км.
    entt::entity villager = entt::null;
    lw::EntityId village = lw::kInvalidId;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        const entt::entity home = world.find(person.home);
        if (lw::isMinor(registry, entity) || lw::isSoldier(registry, entity) || home == entt::null ||
            person.group != lw::kInvalidId) {
            continue;
        }
        const lw::EntityId settlement = registry.get<lw::Building>(home).settlement;
        if (registry.get<lw::Settlement>(world.find(settlement)).kind == lw::SettlementKind::Village &&
            (villager == entt::null || id.value < idOf(world, villager))) {
            villager = entity;
            village = settlement;
        }
    }
    CHECK(villager != entt::null);
    if (villager == entt::null) {
        return;
    }
    // Дома, без нужд, без детей поблизости, встревожен; трусоват — хочет уехать.
    const lw::Building& home = registry.get<lw::Building>(world.find(registry.get<lw::Person>(villager).home));
    registry.get<lw::Position>(villager) = {lw::Map::cellCenter(home.door.x), lw::Map::cellCenter(home.door.y)};
    world.syncSpatial();
    registry.get<lw::Needs>(villager) = {};
    lw::Traits timid = traitsOf(0.5f);
    timid.bravery = 0.1f;
    timid.aggression = 0.2f;
    timid.prudence = 0.9f;
    registry.get<lw::Traits>(villager) = timid;
    lw::raiseAlarm(world, villager, 0.95f, lw::AlarmClue::Zombie, lw::kInvalidId);
    const float open = scoreOf(world, villager, lw::Action::Evacuate);
    CHECK(open > 0.0f);
    if (open <= 0.0f) {
        return;
    }
    registry.emplace<lw::Quarantine>(world.find(village), lw::Quarantine{world.tick(), 2, lw::Quarantine::kNever});
    CHECK(lw::quarantineAt(world, registry.get<lw::Position>(villager).x, registry.get<lw::Position>(villager).y) ==
          village);
    const float obedient = scoreOf(world, villager, lw::Action::Evacuate);
    CHECK(obedient < 0.35f * open && obedient > 0.0f);
    // Смелый и вспыльчивый рвёт кордон охотнее (воля к отъезду у него меньше —
    // сравнить долю: оценку с кордоном и без).
    lw::Traits bold = timid;
    bold.aggression = 1.0f;
    bold.bravery = 0.1f;
    registry.get<lw::Traits>(villager) = bold;
    const float boldWith = scoreOf(world, villager, lw::Action::Evacuate);
    registry.remove<lw::Quarantine>(world.find(village));
    const float boldOpen = scoreOf(world, villager, lw::Action::Evacuate);
    CHECK(boldOpen > 0.0f && boldWith / boldOpen > obedient / open + 0.01f);
}

// --- Ивенты по фазам (этап 7.7) -------------------------------------------------

// Экипаж ивента: каждый в летописи; выживший — раненый человек с домом, без
// смен, знает о месте; погибший — тело, укушенный встанет.
void checkCrew(const lw::World& world, lw::EntityId event, int crewMin, int crewMax, lw::DeathCause cause) {
    const entt::registry& registry = world.registry();
    int crew = 0;
    for (const lw::ChronicleEvent& record : world.chronicle().events()) {
        if (record.kind != lw::ChronicleKind::EventCrew || record.object != event) {
            continue;
        }
        ++crew;
        const entt::entity soldier = world.find(record.subject);
        CHECK(soldier != entt::null);
        if (soldier == entt::null) {
            continue;
        }
        const lw::Job& job = registry.get<lw::Job>(soldier);
        CHECK(job.profession == lw::Profession::Soldier && job.days == 0);
        if (record.a == 1) {
            const lw::Person* person = registry.try_get<lw::Person>(soldier);
            CHECK(person != nullptr && person->home != lw::kInvalidId && job.workplace == person->home);
            CHECK(registry.get<lw::Wounds>(soldier).bleeding > 0.0f);
            CHECK(registry.get<lw::Leads>(soldier).find(event) != nullptr);
            CHECK(lw::isInfected(registry.get<lw::Wounds>(soldier)) == (record.b == 1));
        } else {
            const lw::Corpse* corpse = registry.try_get<lw::Corpse>(soldier);
            CHECK(corpse != nullptr && corpse->cause == cause);
            CHECK(corpse != nullptr && (corpse->turnsAt != 0) == (record.b == 1));
            CHECK(registry.all_of<lw::Weapon>(soldier)); // винтовка осталась при теле
        }
    }
    CHECK(crew >= crewMin && crew <= crewMax);
}

// Крушение вертолёта и застрявший конвой: место по виду (вертолёт — не на воде
// и не на дороге, колонна — на дороге, грузовики тоже), экипаж, зомби у
// колонны; один seed — один и тот же ивент.
void testMilitaryEvents() {
    lw::World a(7, regionConfig());
    lw::World b(7, regionConfig());
    const lw::EntityId crash = lw::startEvent(a, lw::EventKind::HelicopterCrash);
    CHECK(crash != lw::kInvalidId && lw::startEvent(b, lw::EventKind::HelicopterCrash) == crash);
    CHECK(a.stateHash() == b.stateHash());
    const entt::registry& registry = a.registry();
    const lw::Map& map = a.map();
    if (const entt::entity entity = a.find(crash); entity != entt::null) {
        const lw::WorldEvent& event = registry.get<lw::WorldEvent>(entity);
        CHECK(event.kind == lw::EventKind::HelicopterCrash && event.caches.size() == 1);
        CHECK(map.water.at(event.site.x, event.site.y) == lw::Water::None &&
              map.road.at(event.site.x, event.site.y) == lw::Road::None);
        checkCrew(a, crash, 2, 3, lw::DeathCause::Crash);
    }

    const std::size_t zombiesBefore = registry.view<const lw::Zombie>().size();
    const lw::EntityId convoy = lw::startEvent(a, lw::EventKind::MilitaryConvoy);
    CHECK(convoy != lw::kInvalidId && lw::startEvent(b, lw::EventKind::MilitaryConvoy) == convoy);
    CHECK(a.stateHash() == b.stateHash());
    if (const entt::entity entity = a.find(convoy); entity != entt::null) {
        const lw::WorldEvent& event = registry.get<lw::WorldEvent>(entity);
        CHECK(event.caches.size() >= 2 && event.caches.size() <= 3);
        for (const lw::EntityId cache : event.caches) {
            const lw::Position& at = registry.get<lw::Position>(a.find(cache));
            const lw::Cell cell = lw::Map::cellAt(at.x, at.y);
            CHECK(map.road.at(cell.x, cell.y) != lw::Road::None);
            CHECK(registry.get<lw::Inventory>(a.find(cache)).count(lw::ItemKind::Food) >= 15);
        }
        checkCrew(a, convoy, 3, 5, lw::DeathCause::Wounds);
        const std::size_t zombies = registry.view<const lw::Zombie>().size() - zombiesBefore;
        CHECK(zombies >= 5 && zombies <= 10);
    }
    // Мир живёт дальше: укушенные погибшие встают, всё детерминировано.
    runMinutes(a, 60);
    runMinutes(b, 60);
    CHECK(a.stateHash() == b.stateHash());
}

// Ивенты по фазам: в мирное время и во вспышке их нет, с паники — есть;
// гуманитарный груз тянется к поселению на карантине; расписание фазы
// выключается флажком.
void testPhaseEvents() {
    const auto total = [](lw::WorldPhase phase) {
        float sum = 0.0f;
        for (const float rate : lw::phaseRules(phase).eventsPerDay) {
            sum += rate;
        }
        return sum;
    };
    CHECK(total(lw::WorldPhase::Peace) == 0.0f && total(lw::WorldPhase::Outbreak) == 0.0f);
    CHECK(total(lw::WorldPhase::Panic) > 0.0f && total(lw::WorldPhase::Collapse) > 0.0f &&
          total(lw::WorldPhase::Survival) > 0.0f);
    CHECK(lw::phaseRules(lw::WorldPhase::Panic).eventsPerDay[static_cast<std::size_t>(lw::EventKind::SupplyDrop)] >
          lw::phaseRules(lw::WorldPhase::Panic).eventsPerDay[static_cast<std::size_t>(lw::EventKind::HelicopterCrash)]);

    // Коллапс на несколько суток: ивенты идут сами; без флажка — нет.
    lw::World world(7, peaceConfig(0));
    lw::WorldConfig off = peaceConfig(0);
    off.phaseEvents = false;
    lw::World quiet(7, off);
    lw::enterPhase(world, lw::WorldPhase::Collapse);
    lw::enterPhase(quiet, lw::WorldPhase::Collapse);
    runMinutes(world, 4 * 24 * 60);
    runMinutes(quiet, 4 * 24 * 60);
    CHECK(world.chronicle().countOf(lw::ChronicleKind::EventBegan) > 0);
    CHECK(quiet.chronicle().countOf(lw::ChronicleKind::EventBegan) == 0);

    // Поселение на карантине: гуманитарный груз чаще падает у него.
    lw::World aid(7, regionConfig());
    entt::registry& registry = aid.registry();
    const lw::Places::SettlementPlace& place = aid.places().settlements().front();
    const entt::entity settlement = aid.find(place.id);
    registry.emplace<lw::Quarantine>(settlement);
    const lw::Settlement& info = registry.get<lw::Settlement>(settlement);
    const float edge = static_cast<float>(info.radius) * lw::Map::kCellSize;
    lw::Rng rng(11);
    int near = 0;
    constexpr int kTries = 40;
    for (int i = 0; i < kTries; ++i) {
        const std::optional<lw::Cell> site = lw::pickEventSite(aid, lw::EventKind::SupplyDrop, rng);
        CHECK(site.has_value());
        if (site) {
            const float d = std::hypot(lw::Map::cellCenter(site->x) - place.x, lw::Map::cellCenter(site->y) - place.y);
            near += d <= edge + 800.0f + 2.0f * lw::Map::kCellSize ? 1 : 0;
        }
    }
    CHECK(near >= kTries / 3);
}

// --- LOD симуляции (этап 6а) ----------------------------------------------

// Мир для проверок LOD: карта 4 × 4 км, несколько людей, LOD включён.
lw::WorldConfig lodConfig(int people) {
    lw::WorldConfig config = regionConfig();
    config.people = people;
    config.lod = true;
    return config;
}

// Шагать, пока последним не прошёл тик пересчёта уровней.
void stepPastLodRecompute(lw::World& world) {
    do {
        world.step();
    } while ((world.tick() - 1) % lw::kLodPeriodTicks != 0);
}

// Люди стоят на месте и ничего не затевают долгое время.
void lodFreezePeople(lw::World& world) {
    entt::registry& registry = world.registry();
    for (const auto [entity, mind, walker] : registry.view<lw::Mind, lw::Walker>().each()) {
        mind.action = lw::Action::Idle;
        mind.phase = lw::ActionPhase::Perform;
        mind.until = world.tick() + 10 * lw::kTicksPerGameHour;
        mind.nextThink = mind.until;
        walker.waypoints.clear();
        walker.next = 0;
    }
}

// Проходимая клетка суши дальше minChebyshev метров (по какой-нибудь оси) от
// всех людей; nullopt — такой нет.
std::optional<lw::Cell> lodWildCell(const lw::World& world, float minChebyshev) {
    const lw::Map& map = world.map();
    const lw::NavGrid& nav = world.nav();
    std::vector<lw::Position> people;
    for (const auto [entity, person, position] :
         world.registry().view<const lw::Person, const lw::Position>().each()) {
        people.push_back(position);
    }
    for (int y = 16; y < map.height() - 16; y += 4) {
        for (int x = 16; x < map.width() - 16; x += 4) {
            if (!nav.passable({x, y}) || map.water.at(x, y) != lw::Water::None) {
                continue;
            }
            const float cx = lw::Map::cellCenter(x);
            const float cy = lw::Map::cellCenter(y);
            const bool far = std::all_of(people.begin(), people.end(), [&](const lw::Position& p) {
                return std::max(std::abs(p.x - cx), std::abs(p.y - cy)) > minChebyshev;
            });
            if (far) {
                return lw::Cell{x, y};
            }
        }
    }
    return std::nullopt;
}

// Проходимая клетка не дальше radius клеток от точки; nullopt — нет.
std::optional<lw::Cell> lodCellNear(const lw::World& world, float x, float y, int offset) {
    const lw::Cell center = lw::Map::cellAt(x, y);
    return world.nav().nearestPassable({center.x + offset, center.y}, 6);
}

lw::Detail lodLevel(const lw::World& world, entt::entity entity) {
    return lw::detailOf(world.registry(), entity);
}

// Расписания вложены: редкое — подмножество частого; окно восприятия — от
// обновления до обновления; повышение уровня — следующее обновление не позже
// интервала полного уровня.
void testLodSchedulesNested() {
    using lw::Detail;
    static_assert(lw::strideOf(Detail::Full) == 1 && lw::strideOf(Detail::Calm) == 4 &&
                  lw::strideOf(Detail::Dormant) == 16);
    bool nested = true;
    for (lw::EntityId id = 1; id < 40; id += 3) {
        for (const std::uint64_t base : {std::uint64_t{1}, lw::kPerceptionIntervalTicks, lw::kZombieThinkTicks}) {
            for (std::uint64_t tick = 0; tick < 2000; ++tick) {
                const bool dormant = lw::due(tick, id, base, Detail::Dormant);
                const bool calm = lw::due(tick, id, base, Detail::Calm);
                const bool full = lw::due(tick, id, base, Detail::Full);
                nested = nested && (!dormant || calm) && (!calm || full);
            }
        }
    }
    CHECK(nested);
    CHECK(lw::senseWindow(Detail::Full) == lw::kPerceptionIntervalTicks);
    CHECK(lw::senseWindow(Detail::Dormant) == 16 * lw::kPerceptionIntervalTicks);

    // Уровень скачет как попало: перерыв между обновлениями не длиннее
    // интервала самого редкого уровня за этот перерыв, и обновление всегда
    // приходится на тик ритма полного уровня.
    lw::Rng rng(3);
    const lw::EntityId id = 17;
    const std::uint64_t base = lw::kPerceptionIntervalTicks;
    Detail level = Detail::Dormant;
    std::uint64_t last = 0;
    bool seen = false;
    std::uint64_t coarsest = 1;
    bool gapsOk = true;
    bool onFullRhythm = true;
    for (std::uint64_t tick = 0; tick < 50000; ++tick) {
        if (rng.chance(0.01f)) {
            level = static_cast<Detail>(rng.below(3));
        }
        coarsest = std::max(coarsest, lw::strideOf(level));
        if (lw::due(tick, id, base, level)) {
            onFullRhythm = onFullRhythm && lw::due(tick, id, base, Detail::Full);
            if (seen) {
                gapsOk = gapsOk && tick - last <= base * coarsest;
            }
            last = tick;
            seen = true;
            coarsest = lw::strideOf(level);
        }
    }
    CHECK(gapsOk);
    CHECK(onFullRhythm);
}

// Карта активности: люди и зомби ближе 192 м всегда видят друг друга как
// встречу, дальше 362 м — никогда; люди ближе 448 м — «недалеко». Карта шире
// 64 районов — проверяется перенос битов между словами строки и хвост строки.
void testLodActivityMapGeometry() {
    lw::ActivityMap map;
    map.reset(8000.0f, 8000.0f); // 125 × 125 районов: два слова на строку
    CHECK(map.width() == 125 && map.height() == 125);
    lw::Rng rng(21);
    bool guaranteed = true;
    bool bounded = true;
    bool populated = true;
    for (int trial = 0; trial < 4000; ++trial) {
        map.clear();
        const float px = rng.range(100.0f, 7900.0f);
        const float py = rng.range(100.0f, 7900.0f);
        const float zx = std::clamp(px + rng.range(-520.0f, 520.0f), 0.0f, 7999.0f);
        const float zy = std::clamp(py + rng.range(-520.0f, 520.0f), 0.0f, 7999.0f);
        map.mark(lw::ActivityMap::People, px, py);
        map.mark(lw::ActivityMap::Undead, zx, zy);
        map.dilate(lw::ActivityMap::People, lw::ActivityMap::PeopleNear, lw::ActivityMap::kEncounterRegions);
        map.dilate(lw::ActivityMap::Undead, lw::ActivityMap::UndeadNear, lw::ActivityMap::kEncounterRegions);
        map.dilate(lw::ActivityMap::People, lw::ActivityMap::PeopleFar, lw::ActivityMap::kPopulatedRegions);
        const float d = std::hypot(zx - px, zy - py);
        const bool meet = map.around(px, py).undeadNear && map.around(zx, zy).peopleNear;
        if (d < 192.0f) {
            guaranteed = guaranteed && meet;
        }
        if (d > 363.0f) {
            bounded = bounded && !meet;
        }
        if (d < 448.0f) {
            populated = populated && map.around(zx, zy).peopleFar;
        }
    }
    CHECK(guaranteed);
    CHECK(bounded);
    CHECK(populated);
    // Край карты: точка за краем — крайний район, ничего не выходит за карту.
    map.clear();
    map.mark(lw::ActivityMap::People, 7999.0f, 10.0f);
    map.dilate(lw::ActivityMap::People, lw::ActivityMap::PeopleNear, lw::ActivityMap::kEncounterRegions);
    CHECK(map.testRegion(lw::ActivityMap::PeopleNear, 124, 0));
    CHECK(map.testRegion(lw::ActivityMap::PeopleNear, 121, 3));
    CHECK(!map.testRegion(lw::ActivityMap::PeopleNear, 120, 0));
    CHECK(!map.testRegion(lw::ActivityMap::PeopleNear, 124, 4));
    CHECK(!map.testRegion(lw::ActivityMap::PeopleNear, 125, 0));
}

// Без LOD компонентов нет, окно восприятия и жизнь звуков прежние, карта
// активности не считается.
void testLodOffIsPlain() {
    lw::WorldConfig config = lodConfig(8);
    config.lod = false;
    config.zombies = 20;
    lw::World world(5, config);
    for (int i = 0; i < 300; ++i) {
        world.step();
    }
    CHECK(world.registry().view<const lw::Lod>().size() == 0);
    bool windows = true;
    for (const auto [entity, awareness] : world.registry().view<const lw::Awareness>().each()) {
        windows = windows && awareness.window == lw::kPerceptionIntervalTicks;
    }
    CHECK(windows);
    CHECK(lw::soundLifetime(world) == lw::kSoundLifetimeTicks);
    CHECK(!world.activityMap().computed());
}

// Уровни — по состоянию мира: в мирное время все люди спокойные; зомби у
// человека — встреча, оба на полном; зомби вдали от людей дремлет; люди вдали
// от зомби остаются спокойными.
void testLodLevelsFromState() {
    lw::World world(11, lodConfig(8));
    entt::registry& registry = world.registry();
    world.step(); // пересчёт в тик 0
    lodFreezePeople(world);
    bool calm = true;
    std::size_t people = 0;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        const lw::Lod* lod = registry.try_get<lw::Lod>(entity);
        calm = calm && lod != nullptr && lod->level == lw::Detail::Calm && lod->cause == lw::LodCause::People;
        ++people;
    }
    CHECK(people == 8 && calm);
    CHECK(world.activityMap().computed() && world.activityMap().updated() == 0);

    // Зомби в 40 м от первого человека и зомби в глуши.
    std::vector<std::pair<lw::EntityId, entt::entity>> sorted;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        sorted.emplace_back(id.value, entity);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const entt::entity target = sorted.front().second;
    const lw::Position at = registry.get<lw::Position>(target);
    const auto near = lodCellNear(world, at.x, at.y, 10);
    const auto wild = lodWildCell(world, 600.0f);
    CHECK(near.has_value() && wild.has_value());
    if (!near || !wild) {
        return;
    }
    lw::Rng rng(4);
    const entt::entity hunter = lw::createZombie(world, *near, rng);
    const entt::entity loner = lw::createZombie(world, *wild, rng);
    world.syncSpatial();
    // До пересчёта новые — на полном уровне.
    CHECK(lodLevel(world, hunter) == lw::Detail::Full && lodLevel(world, loner) == lw::Detail::Full);
    stepPastLodRecompute(world);
    CHECK(lodLevel(world, target) == lw::Detail::Full);
    CHECK(lodLevel(world, hunter) == lw::Detail::Full);
    const lw::Lod* far = registry.try_get<lw::Lod>(loner);
    CHECK(far != nullptr && far->level == lw::Detail::Dormant && far->cause == lw::LodCause::Wilderness);
    CHECK(registry.get<lw::Awareness>(loner).window == lw::senseWindow(lw::Detail::Dormant));
    // Кто далеко от зомби и от того, к кому он подошёл (дальше 4 районов по
    // оси и от возможной погони), тот спокойный.
    const lw::Position& z = registry.get<lw::Position>(hunter);
    bool distantCalm = true;
    for (const auto [entity, person, position] : registry.view<const lw::Person, const lw::Position>().each()) {
        if (std::max(std::abs(position.x - z.x), std::abs(position.y - z.y)) > 330.0f) {
            distantCalm = distantCalm && lodLevel(world, entity) == lw::Detail::Calm;
        }
    }
    CHECK(distantCalm);
}

// «Будущий зомби» — больной в жару и тело, которое встанет, — для встречи как
// зомби: люди рядом на полном уровне заранее.
void testLodSickAndBodiesCount() {
    lw::World world(13, lodConfig(8));
    entt::registry& registry = world.registry();
    world.step();
    lodFreezePeople(world);
    std::vector<std::pair<lw::EntityId, entt::entity>> sorted;
    for (const auto [entity, id, person] : registry.view<const lw::Id, const lw::Person>().each()) {
        sorted.emplace_back(id.value, entity);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // Больной в жару — рядом со вторым человеком.
    const entt::entity sick = sorted[0].second;
    const entt::entity neighbour = sorted[1].second;
    lw::Position& place = registry.get<lw::Position>(neighbour);
    place = registry.get<lw::Position>(sick);
    place.x += 30.0f;
    lw::Wounds& wounds = registry.get_or_emplace<lw::Wounds>(sick);
    wounds.infection = lw::kFeverStart + 0.05f;
    wounds.route = lw::InfectionRoute::Bite;
    world.syncSpatial();
    stepPastLodRecompute(world);
    const lw::Lod* lod = registry.try_get<lw::Lod>(neighbour);
    CHECK(lod != nullptr && lod->level == lw::Detail::Full && lod->cause == lw::LodCause::Encounter);

    // Тело, которое встанет, — у третьего человека.
    const entt::entity mourner = sorted[2].second;
    const lw::Position by = registry.get<lw::Position>(mourner);
    const entt::entity body = world.create();
    registry.emplace<lw::Position>(body, by.x + 20.0f, by.y);
    lw::Corpse corpse;
    corpse.diedAt = world.tick();
    corpse.turnsAt = world.tick() + 100 * lw::kTicksPerGameHour;
    registry.emplace<lw::Corpse>(body, corpse);
    world.syncSpatial();
    stepPastLodRecompute(world);
    CHECK(lodLevel(world, mourner) == lw::Detail::Full);
}

// Событие поднимает до полного сразу и держит kFullHoldTicks; потом уровень
// возвращается по карте.
void testLodWakeAndHoldover() {
    lw::World world(17, lodConfig(4));
    entt::registry& registry = world.registry();
    world.step();
    lodFreezePeople(world);
    entt::entity person = entt::null;
    for (const auto [entity, p] : registry.view<const lw::Person>().each()) {
        person = entity;
        break;
    }
    CHECK(person != entt::null && lodLevel(world, person) == lw::Detail::Calm);
    if (person == entt::null) {
        return;
    }
    lw::wake(world, person);
    const lw::Lod& lod = registry.get<lw::Lod>(person);
    CHECK(lod.level == lw::Detail::Full && lod.cause == lw::LodCause::Woken);
    CHECK(lod.fullUntil == world.tick() + lw::kFullHoldTicks);
    CHECK(registry.get<lw::Awareness>(person).window == lw::kPerceptionIntervalTicks);
    const std::uint64_t until = lod.fullUntil;
    bool held = true;
    while (world.tick() <= until) {
        stepPastLodRecompute(world);
        const std::uint64_t recompute = world.tick() - 1;
        if (recompute < until) {
            held = held && registry.get<lw::Lod>(person).level == lw::Detail::Full &&
                   registry.get<lw::Lod>(person).cause == lw::LodCause::Holdover;
        }
    }
    CHECK(held);
    CHECK(registry.get<lw::Lod>(person).level == lw::Detail::Calm);
}

// Редко смотрящий слышит и то, что прозвучало между его обновлениями: звук
// живёт до самого редкого обновления.
void testLodDormantHearsSounds() {
    lw::World world(19, lodConfig(0));
    entt::registry& registry = world.registry();
    world.step();
    const auto wild = lodWildCell(world, 600.0f);
    CHECK(wild.has_value());
    if (!wild) {
        return;
    }
    lw::Rng rng(8);
    const entt::entity zombie = lw::createZombie(world, *wild, rng);
    world.syncSpatial();
    stepPastLodRecompute(world);
    CHECK(lodLevel(world, zombie) == lw::Detail::Dormant);
    CHECK(lw::soundLifetime(world) == 2 * lw::kSoundLifetimeTicks * lw::kMaxDetailStride);
    // Звук — в тик, когда зомби не смотрит, — и ждать его обновления.
    const lw::EntityId id = registry.get<lw::Id>(zombie).value;
    while (lw::due(world.tick(), id, lw::kPerceptionIntervalTicks, lw::Detail::Dormant)) {
        world.step();
    }
    const lw::Position& at = registry.get<lw::Position>(zombie);
    const auto spot = lodCellNear(world, at.x, at.y, 6); // туда можно дойти
    CHECK(spot.has_value());
    if (!spot) {
        return;
    }
    constexpr lw::EntityId kNobody = 999999;
    world.emitSound(lw::Map::cellCenter(spot->x), lw::Map::cellCenter(spot->y), 100.0f,
                    lw::SoundKind::Gunshot, kNobody);
    bool heard = false;
    for (std::uint64_t i = 0; i < lw::senseWindow(lw::Detail::Dormant) && !heard; ++i) {
        world.step();
        const lw::Contact* contact = registry.get<lw::Awareness>(zombie).find(kNobody);
        heard = contact != nullptr && contact->heard == lw::SoundKind::Gunshot;
    }
    CHECK(heard);
    // Пошёл на шум — сразу на полном уровне: сроки на месте шума и сбор в орду
    // мерятся от решений, их нельзя мерить шагом дремлющего.
    bool investigating = false;
    for (std::uint64_t i = 0; i < 2 * lw::senseWindow(lw::Detail::Dormant) && !investigating; ++i) {
        world.step();
        investigating = registry.get<lw::Zombie>(zombie).state == lw::ZombieState::Investigate;
    }
    CHECK(investigating);
    CHECK(lodLevel(world, zombie) == lw::Detail::Full);
}

// Звуки при смене уровня: слушатель берёт звуки с прошлого своего обновления,
// а не за окно нового уровня. Поднятый до полного слышит звук, прозвучавший
// между редкими обновлениями до подъёма; опущенный до дремлющего не слышит
// второй раз то, что уже слышал (иначе зомби снова шёл бы проверять место).
void testLodSoundsOnceAcrossLevels() {
    lw::World world(19, lodConfig(0));
    entt::registry& registry = world.registry();
    world.step();
    const auto wild = lodWildCell(world, 600.0f);
    CHECK(wild.has_value());
    if (!wild) {
        return;
    }
    lw::Rng rng(8);
    const entt::entity zombie = lw::createZombie(world, *wild, rng);
    // Источник звуков — другой зомби в 400 м: на «свой» выстрел слушатель не
    // идёт, поэтому уровень меняет только тест.
    const lw::Position start = registry.get<lw::Position>(zombie);
    std::optional<lw::Cell> elsewhere = lodCellNear(world, start.x, start.y, 100);
    if (!elsewhere) {
        elsewhere = lodCellNear(world, start.x, start.y, -100);
    }
    CHECK(elsewhere.has_value());
    if (!elsewhere) {
        return;
    }
    const entt::entity source = lw::createZombie(world, *elsewhere, rng);
    const lw::EntityId sourceId = registry.get<lw::Id>(source).value;
    world.syncSpatial();
    stepPastLodRecompute(world);
    CHECK(lodLevel(world, zombie) == lw::Detail::Dormant);

    // Звук — сразу после редкого обновления; три интервала полного уровня
    // слушатель не смотрит, потом событие поднимает его до полного.
    const lw::EntityId id = registry.get<lw::Id>(zombie).value;
    while (!lw::due(world.tick(), id, lw::kPerceptionIntervalTicks, lw::Detail::Dormant)) {
        world.step();
    }
    world.step();
    const lw::Position here = registry.get<lw::Position>(zombie);
    world.emitSound(here.x + 20.0f, here.y, 100.0f, lw::SoundKind::Gunshot, sourceId);
    for (std::uint64_t i = 0; i < 3 * lw::kPerceptionIntervalTicks; ++i) {
        world.step();
    }
    CHECK(registry.get<lw::Awareness>(zombie).find(sourceId) == nullptr);
    lw::wake(world, zombie);
    CHECK(lodLevel(world, zombie) == lw::Detail::Full);
    for (std::uint64_t i = 0; i < lw::kPerceptionIntervalTicks; ++i) {
        world.step();
    }
    const lw::Contact* heard = registry.get<lw::Awareness>(zombie).find(sourceId);
    CHECK(heard != nullptr && heard->heard == lw::SoundKind::Gunshot);
    if (heard == nullptr) {
        return;
    }
    const std::uint64_t heardAt = heard->tick;

    // Опустить до дремлющего: окно следующего, редкого обновления накрывает
    // уже услышанный звук, но второй раз он не слышен — контакт только помнится.
    lw::setDetail(registry, zombie, lw::Detail::Dormant, lw::LodCause::Wilderness, 0);
    while (!lw::due(world.tick(), id, lw::kPerceptionIntervalTicks, lw::Detail::Dormant)) {
        world.step();
    }
    world.step();
    CHECK(lodLevel(world, zombie) == lw::Detail::Dormant);
    // Звук ещё жив — значит, его не услышали, а не просто забыли.
    CHECK(std::any_of(world.sounds().begin(), world.sounds().end(),
                      [&](const lw::Sound& sound) { return sound.source == sourceId; }));
    const lw::Contact* again = registry.get<lw::Awareness>(zombie).find(sourceId);
    CHECK(again != nullptr && again->tick == heardAt);
}

// Мир с LOD при 1, 3 и 8 потоках один и тот же; в нём есть все три уровня.
void testLodThreadsSameHash() {
    lw::WorldConfig config = lodConfig(150);
    config.zombies = 400;
    config.animals = 80;
    config.flocks = 6;
    const int threads = lw::parallelThreads();
    std::vector<std::uint64_t> hashes;
    std::size_t levels[3] = {};
    for (const int count : {1, 3, 8}) {
        lw::setParallelThreads(count);
        lw::World world(23, config);
        for (int i = 0; i < 1500; ++i) {
            world.step();
        }
        hashes.push_back(world.stateHash());
        if (count == 1) {
            for (const auto [entity, lod] : world.registry().view<const lw::Lod>().each()) {
                ++levels[static_cast<std::size_t>(lod.level)];
            }
        }
    }
    lw::setParallelThreads(threads);
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
    CHECK(levels[1] > 0 && levels[2] > 0); // в мире есть и спокойные, и дремлющие
    // Тот же мир без LOD — другой (уровни меняют ритм), но тоже детерминирован.
    config.lod = false;
    lw::World plain(23, config);
    lw::World again(23, config);
    for (int i = 0; i < 1500; ++i) {
        plain.step();
        again.step();
    }
    CHECK(plain.stateHash() == again.stateHash());
    CHECK(plain.stateHash() != hashes[0]);
}

// У тела и у уехавшего уровня нет; вставший зомби — на полном до пересчёта.
void testLodRemovedOnDeath() {
    lw::World world(29, lodConfig(4));
    entt::registry& registry = world.registry();
    world.step();
    std::vector<entt::entity> people;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        people.push_back(entity);
    }
    CHECK(people.size() == 4);
    if (people.size() < 2) {
        return;
    }
    CHECK(registry.all_of<lw::Lod>(people[0]) && registry.all_of<lw::Lod>(people[1]));
    lw::die(world, people[0], lw::DeathCause::Wounds, lw::kInvalidId);
    CHECK(!registry.all_of<lw::Lod>(people[0]));
    lw::evacuate(world, people[1], lw::kInvalidId, false);
    CHECK(!registry.all_of<lw::Lod>(people[1]));
}

// Наблюдение мир не меняет: то, что читают интерфейс, режиссёр и инспектор
// (предвестия, линии, герои, угрозы, оценки решений, карта активности), не
// сдвигает ни одного бита.
void testObservationDoesNotChangeWorld() {
    lw::WorldConfig config = lodConfig(60);
    config.zombies = 120;
    config.animals = 20;
    lw::World plain(31, config);
    lw::World watched(31, config);
    lw::StoryTracker stories;
    lw::CastTracker cast;
    for (int i = 0; i < 2400; ++i) {
        plain.step();
        watched.step();
        if (i % 25 != 0) {
            continue;
        }
        const lw::World& view = watched;
        stories.update(view);
        cast.update(view);
        (void)lw::foresee(view);
        (void)lw::scenarioStats(view);
        for (const auto [entity, person] : view.registry().view<const lw::Person>().each()) {
            (void)lw::threatOf(view, entity);
            (void)lw::evaluateActions(view, entity);
            (void)lw::detailOf(view.registry(), entity);
        }
        (void)view.activityMap().around(1000.0f, 1000.0f);
    }
    CHECK(plain.stateHash() == watched.stateHash());
}

int main() {
    testRng();
    testStableIds();
    testMap();
    testMapGeneration();
    testDeterminism();
    testEntitiesStayInWorld();
    testSimDriver();
    testCalendar();
    testClimate();
    testWeather();
    testSpatialIndex();
    testSpatialIndexOrderIndependent();
    testWorldSpatialIndex();
    testSpatialIndexParallelSync();
    testParallelThreadsSameHash();
    testProfilerDoesNotChangeWorld();
    testSegmentWalk();
    testNavGrid();
    testPathfinderOnRandomMaps();
    testPathfinderDeterminism();
    testWorldNavigation();
    testWalkSpeed();
    testPeople();
    testLongMovementStep();
    testVelocityIsTickDisplacement();
    testNeeds();
    testTraits();
    testPlaces();
    testPersonalNight();
    testAiEatsAtHome();
    testAiFindsWater();
    testAiScavengesWhenHungry();
    testAiCarriesHome();
    testAiArrivesElsewhere();
    testAiStocksUp();
    testAiSleepsWhereStranded();
    testAiSurvives();
    testResources();
    testRainFillsBarrels();
    testAiForagesFromStock();
    testChronicle();
    testChronicleRecords();
    testSpatialIndexWide();
    testSenseConditions();
    testSightCost();
    testPerception();
    testPerceptionInWorld();
    testZombieSpawn();
    testZombiePerception();
    testZombieChase();
    testZombieInvestigates();
    testPersonFlees();
    testHordes();
    testShelter();
    testAnimalSpawn();
    testAnimalFlees();
    testZombieIgnoresAnimals();
    testBirdsScatter();
    testMeleeKillsZombie();
    testGunshotIsHeard();
    testZombieKillsPerson();
    testPersonFightsZombie();
    testHunting();
    testInfectionTurns();
    testContactInfectionCourse();
    testContagion();
    testStarvationDeath();
    testBodiesDecay();
    testBodyScares();
    testCrowsFlyToBody();
    testInventory();
    testLootSpawn();
    testScavengeTakesGear();
    testTreatsWound();
    testLootsBody();
    testMeatSpoils();
    testAnimalsReturn();
    testPharmacies();
    testZombiesArrive();
    testChemistry();
    testAcquireBond();
    testRelationsAtStart();
    testRelationsGrow();
    testFoeIsNoCompany();
    testSocializeSeeksFriend();
    testRememberAndFade();
    testDebtAndGrudge();
    testRescueRemembered();
    testAbandonedAndDefended();
    testTheftWitnessed();
    testMourning();
    testWitnessedTheft();
    testRumors();
    testDeathRumor();
    testOpinionsSpread();
    testReconcile();
    testGroupForms();
    testGroupLeaveAndExpel();
    testGroupLeadership();
    testDefendGroupMate();
    testShareFood();
    testTrade();
    testBetrayal();
    testDesertion();
    testAvenge();
    testPersonThreat();
    testEventStarts();
    testEventSalvaged();
    testEventSmokeSeen();
    testLeadSpreads();
    testEventEnds();
    testPeacefulStart();
    testShifts();
    testWorkday();
    testBuyFood();
    testShopNeedsStaff();
    testRestock();
    testHarvest();
    testSeeDoctor();
    testPoliceDeters();
    testTapWater();
    testPhaseRules();
    testOutbreakScheduled();
    testPhaseTransitions();
    testPatientZero();
    testReimport();
    testSickBehaviour();
    testCollapseRules();
    testPanicRules();
    testCrimeCondemned();
    testPoliceDefends();
    testStoryLines();
    testForesight();
    testCast();
    testHouseholdPlan();
    testFamiliesInWorld();
    testKinRemembered();
    testChildBehaviour();
    testOrphanTakenIn();
    testFamilyInGroup();
    testAlarmRules();
    testAlarmFromEvidence();
    testAlarmBehaviour();
    testQuarantine();
    testEvacuation();
    testReunite();
    testCordon();
    testMilitaryEvents();
    testPhaseEvents();
    testLodSchedulesNested();
    testLodActivityMapGeometry();
    testLodOffIsPlain();
    testLodLevelsFromState();
    testLodSickAndBodiesCount();
    testLodWakeAndHoldover();
    testLodDormantHearsSounds();
    testLodSoundsOnceAcrossLevels();
    testLodThreadsSameHash();
    testLodRemovedOnDeath();
    testObservationDoesNotChangeWorld();

    if (failures == 0) {
        std::printf("all tests passed\n");
    } else {
        std::printf("%d check(s) failed\n", failures);
    }
    return failures;
}
