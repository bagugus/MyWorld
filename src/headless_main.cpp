// Прогон симуляции без окна: проверка детерминизма и замер скорости.
//   lw_headless --seed 42 --ticks 10000

#include "sim/Components.h"
#include "sim/World.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    for (const auto [entity, settlement] : world.registry().view<const lw::Settlement>().each()) {
        (settlement.kind == lw::SettlementKind::Town ? towns : villages) += 1;
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
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    std::uint64_t ticks = 1000;

    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--seed") == 0 && hasValue) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--ticks") == 0 && hasValue) {
            ticks = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::printf("usage: lw_headless [--seed N] [--ticks N]\n");
            return 2;
        }
    }

    const auto genStart = std::chrono::steady_clock::now();
    lw::World world(seed);
    const std::chrono::duration<double> genElapsed = std::chrono::steady_clock::now() - genStart;
    printMapSummary(world, genElapsed.count());

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
    if (elapsed.count() > 0.0) {
        std::printf("%.3f s, %.0f ticks/s\n", elapsed.count(),
                    static_cast<double>(ticks) / elapsed.count());
    }
    return 0;
}
