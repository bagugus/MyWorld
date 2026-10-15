// Тесты ядра симуляции: RNG, стабильные id, карта и её генерация, детерминизм,
// драйвер времени, календарь, климат и погода, пространственный индекс, навигация.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "sim/Calendar.h"
#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Rng.h"
#include "sim/SimDriver.h"
#include "sim/SpatialIndex.h"
#include "sim/World.h"
#include "sim/nav/NavGraph.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"

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

    if (failures == 0) {
        std::printf("all tests passed\n");
    } else {
        std::printf("%d check(s) failed\n", failures);
    }
    return failures;
}
