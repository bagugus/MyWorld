// Прогон симуляции без окна: проверка детерминизма и замер скорости.
//   lw_headless --seed 42 --ticks 10000
//   lw_headless --seed 42 --climate   сводка погоды и температуры за игровой год по сезонам
//   lw_headless --seed 42 --paths 500 замер поиска пути: пары дверей и пары случайных клеток суши
//                                     (--optimal — сравнить каждый путь с оптимальным)
//   lw_headless --seed 42 --people 3000 --ticks 144000   сутки мира с 3000 людей

#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Rng.h"
#include "sim/World.h"
#include "sim/nav/NavGrid.h"
#include "sim/nav/Pathfinder.h"
#include "sim/nav/NavGraph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace {

// Сводка по карте: по ней видно, что генерация дала осмысленный мир.
void printMapSummary(const lw::World& world, double seconds) {
    const lw::Map& map = world.map();
    std::size_t land = 0;
    std::size_t forest = 0;
    std::size_t field = 0;
    std::size_t lake = 0;
    std::size_t river = 0;
    std::size_t road = 0;
    int highest = 0;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const lw::Water water = map.water.at(x, y);
            land += water == lw::Water::None ? 1 : 0;
            lake += water == lw::Water::Lake ? 1 : 0;
            river += water == lw::Water::River ? 1 : 0;
            forest += map.cover.at(x, y) == lw::Cover::Forest ? 1 : 0;
            field += map.ground.at(x, y) == lw::Ground::Field ? 1 : 0;
            road += map.road.at(x, y) != lw::Road::None ? 1 : 0;
            highest = map.elevation.at(x, y) > highest ? map.elevation.at(x, y) : highest;
        }
    }
    const double cells = static_cast<double>(map.width()) * map.height();
    const double landCells = land > 0 ? static_cast<double>(land) : 1.0;
    const double cellKm = lw::Map::kCellSize / 1000.0;

    std::size_t towns = 0;
    std::size_t villages = 0;
    const lw::Settlement* largest = nullptr;
    for (const auto [entity, settlement] : world.registry().view<const lw::Settlement>().each()) {
        (settlement.kind == lw::SettlementKind::Town ? towns : villages) += 1;
        if (largest == nullptr || settlement.radius > largest->radius) {
            largest = &settlement;
        }
    }

    std::printf("map generated in %.2f s\n", seconds);
    std::printf("  land %.0f%% (forest %.0f%%, fields %.0f%% of land), highest point %.0f m\n",
                100.0 * static_cast<double>(land) / cells,
                100.0 * static_cast<double>(forest) / landCells,
                100.0 * static_cast<double>(field) / landCells,
                highest * lw::Map::kElevationStep);
    std::printf("  lake cells %zu, river cells %zu, roads %.1f km\n", lake, river,
                static_cast<double>(road) * cellKm);
    std::printf("  towns %zu, villages %zu, buildings %zu\n", towns, villages,
                world.registry().view<const lw::Building>().size());
    if (largest != nullptr) {
        std::printf("  largest: %s at %.0f, %.0f m\n", largest->name.c_str(),
                    lw::Map::cellCenter(largest->center.x), lw::Map::cellCenter(largest->center.y));
    }
}

// Люди: сколько в пути, сколько у своего дома.
void printPeople(const lw::World& world) {
    const entt::registry& registry = world.registry();
    std::size_t people = 0;
    std::size_t walking = 0;
    std::size_t atHome = 0;
    std::size_t asleep = 0;
    lw::Needs mean{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float worstHealth = 1.0f;
    for (const auto [entity, person, walker, position] :
         registry.view<const lw::Person, const lw::Walker, const lw::Position>().each()) {
        ++people;
        asleep += person.asleep ? 1 : 0;
        if (const auto* needs = registry.try_get<lw::Needs>(entity)) {
            mean.hunger += needs->hunger;
            mean.thirst += needs->thirst;
            mean.fatigue += needs->fatigue;
            mean.fear += needs->fear;
            mean.loneliness += needs->loneliness;
            mean.health += needs->health;
            worstHealth = std::min(worstHealth, needs->health);
        }
        walking += walker.moving() ? 1 : 0;
        if (const entt::entity home = world.find(person.home); home != entt::null) {
            atHome += lw::Map::cellAt(position.x, position.y) ==
                              registry.get<lw::Building>(home).door
                          ? 1
                          : 0;
        }
    }
    std::printf("people %zu: walking %zu, at home %zu, asleep %zu\n", people, walking, atHome,
                asleep);
    constexpr const char* kActions[] = {"idle",   "sleep",     "eat",    "drink",  "scavenge",
                                        "forage", "socialize", "wander", "go home"};
    static_assert(std::size(kActions) == static_cast<std::size_t>(lw::Action::Count));
    std::size_t actions[std::size(kActions)] = {};
    for (const auto [entity, mind] : registry.view<const lw::Mind>().each()) {
        ++actions[static_cast<std::size_t>(mind.action)];
    }
    std::printf("  doing:");
    for (std::size_t i = 0; i < std::size(kActions); ++i) {
        std::printf(" %s %zu%s", kActions[i], actions[i], i + 1 < std::size(kActions) ? "," : "\n");
    }
    long long food = 0;
    long long water = 0;
    for (const auto [entity, supplies] : registry.view<const lw::Supplies>().each()) {
        food += supplies.food;
        water += supplies.water;
    }
    long long carried = 0;
    for (const auto [entity, carry] : registry.view<const lw::Carry>().each()) {
        carried += carry.food;
    }
    std::printf("  supplies in buildings: food %lld, water %lld; carried food %lld\n", food,
                water, carried);
    const lw::Resources& resources = world.resources();
    std::printf("  forest: %lld portions in %d blocks (%d empty); rain filled barrels by %u\n",
                resources.forestTotal(), resources.forestBlocks(), resources.forestEmpty(),
                static_cast<unsigned>(resources.rainPortions()));
    if (people > 0) {
        const float n = static_cast<float>(people);
        std::printf("  mean needs: hunger %.2f, thirst %.2f, fatigue %.2f, fear %.2f, "
                    "loneliness %.2f, health %.2f (worst %.2f)\n",
                    mean.hunger / n, mean.thirst / n, mean.fatigue / n, mean.fear / n,
                    mean.loneliness / n, mean.health / n, worstHealth);
    }
}

// Летопись: сколько событий каждого вида.
void printChronicle(const lw::World& world) {
    const lw::Chronicle& chronicle = world.chronicle();
    std::printf("chronicle %zu events:", chronicle.events().size());
    const char* separator = " ";
    for (std::size_t i = 0; i < static_cast<std::size_t>(lw::ChronicleKind::Count); ++i) {
        const auto kind = static_cast<lw::ChronicleKind>(i);
        std::printf("%s%s %zu", separator, lw::chronicleKindName(kind), chronicle.countOf(kind));
        separator = ", ";
    }
    std::printf("\n");
}

void printWeather(const lw::World& world) {
    const lw::Environment environment = lw::environmentAt(world);
    const lw::CalendarTime& time = environment.time;
    const lw::Weather& weather = world.weather();
    std::printf("year %d, season %d, day %d of season, %02d:%02d; light %.2f, %.1f C, "
                "weather %d (clouds %.2f, precipitation %.2f, fog %.2f, wind %.1f m/s), snow %.2f\n",
                time.year, static_cast<int>(time.season), time.dayOfSeason + 1, time.hour,
                time.minute, environment.light, environment.temperature,
                static_cast<int>(weather.kind), weather.cloudCover, weather.precipitation,
                weather.fog, weather.windSpeed, weather.snowCover);
}

// Год погоды на маленькой карте: доли типов погоды, температура, снег — по сезонам.
// По сводке подбираются константы климата.
void printClimate(std::uint64_t seed) {
    lw::WorldConfig config;
    config.mapWidth = 64;
    config.mapHeight = 64;
    config.people = 0;
    lw::World world(seed, config);

    constexpr int kKinds = static_cast<int>(lw::WeatherKind::Count);
    struct SeasonStats {
        int hours = 0;
        int kinds[kKinds] = {};
        int snowfallHours = 0;
        int snowCoveredHours = 0;
        double temperatureSum = 0.0;
        float minTemperature = 1e9f;
        float maxTemperature = -1e9f;
    };
    SeasonStats seasons[4];

    for (int hour = 0; hour < lw::kGameDaysPerYear * 24; ++hour) {
        for (std::uint64_t i = 0; i < lw::kTicksPerGameHour; ++i) {
            world.step();
        }
        const lw::Environment environment = lw::environmentAt(world);
        const lw::Weather& weather = world.weather();
        SeasonStats& stats = seasons[static_cast<int>(environment.time.season)];
        ++stats.hours;
        ++stats.kinds[static_cast<int>(weather.kind)];
        stats.snowfallHours += weather.precipitation > 0.05f && lw::isSnowing(environment.temperature) ? 1 : 0;
        stats.snowCoveredHours += weather.snowCover > 0.2f ? 1 : 0;
        stats.temperatureSum += environment.temperature;
        stats.minTemperature = std::min(stats.minTemperature, environment.temperature);
        stats.maxTemperature = std::max(stats.maxTemperature, environment.temperature);
    }

    constexpr const char* kSeasonNames[] = {"spring", "summer", "autumn", "winter"};
    std::printf("climate, seed %llu: %% of hours by weather "
                "(clear cloudy overcast fog rain storm), temperature at sea level\n",
                static_cast<unsigned long long>(seed));
    for (int s = 0; s < 4; ++s) {
        const SeasonStats& stats = seasons[s];
        const double hours = stats.hours > 0 ? stats.hours : 1;
        std::printf("  %-6s", kSeasonNames[s]);
        for (const int count : stats.kinds) {
            std::printf(" %3.0f", 100.0 * count / hours);
        }
        std::printf(" | %+5.1f .. %+5.1f C, mean %+5.1f | snowfall %3d h, snow on ground %3d h\n",
                    stats.minTemperature, stats.maxTemperature, stats.temperatureSum / hours,
                    stats.snowfallHours, stats.snowCoveredHours);
    }
}


using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Серия запросов к поиску пути и сводка: методы, время, раскрытые клетки, крюк.
// optimal — если задан, каждый путь сравнивается с оптимальным (A* по всей карте).
void runPathGroup(const char* name, lw::Pathfinder& finder, lw::Pathfinder* optimal, int count,
                  const std::function<std::pair<lw::Cell, lw::Cell>()>& nextPair) {
    constexpr int kMethods = static_cast<int>(lw::PathMethod::Full) + 1;
    int methods[kMethods] = {};
    int failed = 0;
    std::vector<double> times;
    double expandedSum = 0.0;
    std::uint32_t expandedMax = 0;
    double detourSum = 0.0;
    double costRatioSum = 0.0;
    int measured = 0;
    double overOptimalSum = 0.0;
    double overOptimalMax = 0.0;
    double optimalMs = 0.0;
    double overByMethod[kMethods] = {};
    for (int i = 0; i < count; ++i) {
        const auto [start, goal] = nextPair();
        const auto begin = Clock::now();
        const lw::Path path = finder.find(start, goal);
        times.push_back(millisSince(begin));
        if (!path.found()) {
            ++failed;
            continue;
        }
        ++methods[static_cast<int>(path.method)];
        if (optimal != nullptr) {
            const auto optimalBegin = Clock::now();
            const lw::Path best = optimal->find(start, goal);
            optimalMs += millisSince(optimalBegin);
            const double over = best.cost > 0 ? static_cast<double>(path.cost) / best.cost - 1.0 : 0.0;
            overOptimalSum += over;
            overOptimalMax = std::max(overOptimalMax, over);
            overByMethod[static_cast<int>(path.method)] += over;
        }
        expandedSum += path.expanded;
        expandedMax = std::max(expandedMax, path.expanded);
        const double dx = goal.x - start.x;
        const double dy = goal.y - start.y;
        const double straight = std::sqrt(dx * dx + dy * dy) * lw::Map::kCellSize;
        if (straight > 0.0) {
            detourSum += path.length / straight;
            costRatioSum += path.cost / static_cast<double>(lw::NavGrid::kCostPerRoadMeter) / straight;
            ++measured;
        }
    }
    std::sort(times.begin(), times.end());
    double total = 0.0;
    for (const double t : times) {
        total += t;
    }
    const auto percentile = [&](double p) {
        return times[std::min(times.size() - 1, static_cast<std::size_t>(p * times.size()))];
    };
    const int found = count - failed;
    std::printf("%s: %d paths, found %d (direct %d, graph %d, full %d), failed %d\n", name, count,
                found, methods[static_cast<int>(lw::PathMethod::Direct)],
                methods[static_cast<int>(lw::PathMethod::Graph)],
                methods[static_cast<int>(lw::PathMethod::Full)], failed);
    std::printf("  time ms: mean %.2f, p50 %.2f, p95 %.2f, max %.2f; expanded cells: mean %.0f, max %u\n",
                total / count, percentile(0.5), percentile(0.95), times.back(),
                found > 0 ? expandedSum / found : 0.0, expandedMax);
    if (measured > 0) {
        std::printf("  length / straight line %.2f, cost in road meters / straight line %.2f\n",
                    detourSum / measured, costRatioSum / measured);
    }
    if (optimal != nullptr && found > 0) {
        const int graph = methods[static_cast<int>(lw::PathMethod::Graph)];
        std::printf("  over optimal cost: mean %.1f%%, max %.1f%% (by graph %.1f%%); "
                    "optimal search mean %.1f ms\n",
                    100.0 * overOptimalSum / found, 100.0 * overOptimalMax,
                    graph > 0 ? 100.0 * overByMethod[static_cast<int>(lw::PathMethod::Graph)] / graph : 0.0,
                    optimalMs / found);
    }
}

// Замер поиска пути на карте мира: между дверями зданий (походы из дома в дом и
// из поселения в поселение) и между случайными клетками главной области.
void benchmarkPaths(const lw::World& world, int count, bool compare) {
    // Мир сетку и граф уже построил; здесь — заново, только ради времени.
    auto begin = Clock::now();
    lw::NavGrid nav;
    nav.build(world.map(), world.registry());
    const double navMs = millisSince(begin);
    begin = Clock::now();
    lw::NavGraph graph;
    graph.build(nav);
    const double graphMs = millisSince(begin);
    int byKind[4] = {};
    for (const lw::NavGraph::Node& node : graph.nodes()) {
        ++byKind[static_cast<int>(node.kind)];
    }
    std::printf("nav grid %.0f ms, %zu regions; nav graph %.0f ms, %zu nodes "
                "(road %d, land %d, water %d), %zu edges\n",
                navMs, nav.regionCount(), graphMs, graph.nodeCount(),
                byKind[static_cast<int>(lw::NavClass::Road)], byKind[static_cast<int>(lw::NavClass::Land)],
                byKind[static_cast<int>(lw::NavClass::Water)], graph.edgeCount() / 2);

    std::vector<std::pair<lw::EntityId, lw::Cell>> doors;
    for (const auto [entity, id, building] : world.registry().view<const lw::Id, const lw::Building>().each()) {
        doors.emplace_back(id.value, building.door);
    }
    std::sort(doors.begin(), doors.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (doors.empty()) {
        std::printf("no buildings\n");
        return;
    }
    // Свой генератор: замер не трогает потоки мира.
    lw::Rng rng(world.seed() ^ 0x9A7Bu);
    const auto randomDoor = [&]() {
        return doors[rng.below(static_cast<std::uint32_t>(doors.size()))].second;
    };
    const lw::NavGrid::Region mainland = nav.region(doors.front().second);
    const auto randomLand = [&]() {
        for (;;) {
            const lw::Cell cell{static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.width()))),
                                static_cast<int>(rng.below(static_cast<std::uint32_t>(nav.height())))};
            if (nav.region(cell) == mainland) {
                return cell;
            }
        }
    };

    lw::Pathfinder finder(nav, graph);
    lw::PathfinderSettings exhaustive;
    exhaustive.directRange = 1 << 20;
    exhaustive.directBudget = 0xFFFFFFFFu;
    lw::Pathfinder optimal(nav, graph, exhaustive);
    lw::Pathfinder* reference = compare ? &optimal : nullptr;
    runPathGroup("doors", finder, reference, count, [&]() { return std::make_pair(randomDoor(), randomDoor()); });
    runPathGroup("land", finder, reference, count, [&]() { return std::make_pair(randomLand(), randomLand()); });
    // Ближние: в пределах прямого поиска, как походы по поселению и в лес за дровами.
    runPathGroup("near", finder, reference, count, [&]() {
        const lw::Cell start = randomLand();
        for (;;) {
            const lw::Cell goal{start.x + rng.rangeInt(-120, 120), start.y + rng.rangeInt(-120, 120)};
            if (nav.region(goal) == mainland) {
                return std::make_pair(start, goal);
            }
        }
    });
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    std::uint64_t ticks = 1000;
    bool climate = false;
    int paths = 0;
    bool compare = false;
    lw::WorldConfig config;

    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--seed") == 0 && hasValue) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--ticks") == 0 && hasValue) {
            ticks = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--climate") == 0) {
            climate = true;
        } else if (std::strcmp(argv[i], "--paths") == 0 && hasValue) {
            paths = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--optimal") == 0) {
            compare = true;
        } else if (std::strcmp(argv[i], "--people") == 0 && hasValue) {
            config.people = std::atoi(argv[++i]);
        } else {
            std::printf("usage: lw_headless [--seed N] [--ticks N] [--climate] [--people N] [--paths N [--optimal]]\n");
            return 2;
        }
    }

    if (climate) {
        printClimate(seed);
        return 0;
    }

    const auto genStart = std::chrono::steady_clock::now();
    lw::World world(seed, config);
    const std::chrono::duration<double> genElapsed = std::chrono::steady_clock::now() - genStart;
    printMapSummary(world, genElapsed.count());
    if (paths > 0) {
        benchmarkPaths(world, paths, compare);
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < ticks; ++i) {
        world.step();
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    std::printf("seed=%llu ticks=%llu map=%dx%d entities=%zu hash=%016llx\n",
                static_cast<unsigned long long>(world.seed()),
                static_cast<unsigned long long>(world.tick()),
                world.map().width(),
                world.map().height(),
                world.entityCount(),
                static_cast<unsigned long long>(world.stateHash()));
    printWeather(world);
    printPeople(world);
    printChronicle(world);
    if (elapsed.count() > 0.0) {
        std::printf("%.3f s, %.0f ticks/s\n", elapsed.count(),
                    static_cast<double>(ticks) / elapsed.count());
    }
    return 0;
}
