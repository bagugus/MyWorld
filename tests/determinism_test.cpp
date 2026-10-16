// Тесты ядра симуляции: RNG, стабильные id, карта и её генерация, детерминизм,
// драйвер времени, календарь, климат и погода, пространственный индекс, навигация,
// люди и движение, нужды, черты, Utility AI, ресурсы, летопись, восприятие, зомби,
// животные и птицы, бой, смерть и тела, инвентарь и лут, дефицит, отношения.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "sim/Calendar.h"
#include "sim/Chronicle.h"
#include "sim/Climate.h"
#include "sim/Death.h"
#include "sim/Components.h"
#include "sim/Items.h"
#include "sim/Map.h"
#include "sim/Perception.h"
#include "sim/Places.h"
#include "sim/Relations.h"
#include "sim/Resources.h"
#include "sim/Rng.h"
#include "sim/SimDriver.h"
#include "sim/SpatialIndex.h"
#include "sim/Traits.h"
#include "sim/Weapons.h"
#include "sim/World.h"
#include "sim/nav/NavGraph.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/CombatSystem.h"
#include "sim/systems/MovementSystem.h"
#include "sim/systems/NeedsSystem.h"
#include "sim/systems/PerceptionSystem.h"
#include "sim/systems/ResourceSystem.h"
#include "sim/systems/SocialSystem.h"
#include "sim/systems/UtilityAiSystem.h"
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
    config.zombies = 0; // зомби, животные и птицы — в своих тестах
    config.animals = 0;
    config.flocks = 0;
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

    // К трём часам ночи почти все дома.
    const std::uint64_t night = lw::nextTimeOfDay(world.tick(), 3, 0);
    while (world.tick() < night) {
        world.step();
    }
    CHECK(atHome() >= 140);
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
    for (const auto [entity, needs, mind] : registry.view<const lw::Needs, const lw::Mind>().each()) {
        worst = std::min(worst, needs.health);
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

// Мир из двух человек на открытом месте: A смотрит, B — цель.
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
    CHECK(seen != nullptr && seen->seen && lw::isFresh(*seen, pair.world.tick()));
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
    CHECK(seen != nullptr && seen->x == where.x && !lw::isFresh(*seen, pair.world.tick()));
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

    explicit Field(int peopleCount) : world(7, [peopleCount] {
                                          lw::WorldConfig config = regionConfig();
                                          config.people = peopleCount;
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

} // namespace

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

    if (failures == 0) {
        std::printf("all tests passed\n");
    } else {
        std::printf("%d check(s) failed\n", failures);
    }
    return failures;
}
