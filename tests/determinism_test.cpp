// Тесты ядра симуляции: RNG, стабильные id, карта и её генерация, детерминизм,
// драйвер времени, календарь, климат и погода.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "sim/Calendar.h"
#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Rng.h"
#include "sim/SimDriver.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
lw::WorldConfig smallConfig(int wanderers) {
    lw::WorldConfig config;
    config.mapWidth = 64;
    config.mapHeight = 64;
    config.wanderers = wanderers;
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
    config.wanderers = 0;
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
    config.wanderers = 100;
    lw::World world(5, config);
    for (int i = 0; i < 5000; ++i) {
        world.step();
    }
    const lw::World& readOnly = world;
    for (const auto [entity, position] : readOnly.registry().view<const lw::Position>().each()) {
        CHECK(position.x >= 0.0f && position.x <= world.width());
        CHECK(position.y >= 0.0f && position.y <= world.height());
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

    if (failures == 0) {
        std::printf("all tests passed\n");
    } else {
        std::printf("%d check(s) failed\n", failures);
    }
    return failures;
}
