// Ð¢ÐµÑÑ‚Ñ‹ Ð¾Ð±Ð»Ð¸ÐºÐ° Ð¼Ð¸Ñ€Ð° Ð²Ð½Ðµ Ð»ÐµÑÐ° (ÑÑ‚Ð°Ð¿ 11.9) â€” Ñ€Ð°ÑÐºÐ»Ð°Ð´ÐºÐ° Ð±ÐµÐ· Ð³Ñ€Ð°Ñ„Ð¸ÐºÐ¸: Ð»Ð¾ÑÐºÑƒÑ‚Ñ‹ Ð¿Ð¾Ð»ÐµÐ¹
// (render/FieldLayout), Ñ‚ÐµÑ‡ÐµÐ½Ð¸Ðµ Ñ€ÐµÐº, Ð´Ð²Ð¾Ñ€Ñ‹, Ð·Ð°Ð±Ð¾Ñ€Ñ‹, ÑÑ‚Ð¾Ð»Ð±Ñ‹ Ð¸ Ð¼Ð¾ÑÑ‚ÐºÐ¸
// (render/LandUse), Ð´ÐµÑ€ÐµÐ²ÑŒÑ Ð²Ð½Ðµ Ð»ÐµÑÐ° (render/TreeLayout). ÐžÐ±Ð»Ð¸Ðº Ð½Ðµ Ð²Ñ…Ð¾Ð´Ð¸Ñ‚ Ð² Ñ…ÐµÑˆ
// Ð¼Ð¸Ñ€Ð°, Ð½Ð¾ Ð´Ð¾Ð»Ð¶ÐµÐ½ Ð±Ñ‹Ñ‚ÑŒ Ð´ÐµÑ‚ÐµÑ€Ð¼Ð¸Ð½Ð¸Ñ€Ð¾Ð²Ð°Ð½Ð½Ñ‹Ð¼ Ð¸ ÑÐ¾Ð³Ð»Ð°ÑÐ¾Ð²Ð°Ð½Ð½Ñ‹Ð¼ Ñ ÐºÐ°Ñ€Ñ‚Ð¾Ð¹.
// Ð‘ÐµÐ· Ñ„Ñ€ÐµÐ¹Ð¼Ð²Ð¾Ñ€ÐºÐ°: CHECK Ð¿ÐµÑ‡Ð°Ñ‚Ð°ÐµÑ‚ Ð¼ÐµÑÑ‚Ð¾ ÑÐ±Ð¾Ñ, ÐºÐ¾Ð´ Ð²Ð¾Ð·Ð²Ñ€Ð°Ñ‚Ð° â€” Ñ‡Ð¸ÑÐ»Ð¾ ÑÐ±Ð¾ÐµÐ².

#include "render/FieldLayout.h"
#include "render/LandUse.h"
#include "render/TreeLayout.h"
#include "sim/World.h"

#include <cmath>
#include <cstdio>
#include <set>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

lw::WorldConfig regionConfig() {
    lw::WorldConfig config;
    config.mapWidth = 512;
    config.mapHeight = 512;
    config.people = 40;
    config.animals = 0;
    config.flocks = 0;
    return config;
}

// Ð›Ð¾ÑÐºÑƒÑ‚Ñ‹: Ð´ÐµÑ‚ÐµÑ€Ð¼Ð¸Ð½Ð¸Ñ€Ð¾Ð²Ð°Ð½Ñ‹, Ð¼ÐµÐ¶Ð° â€” Ñƒ Ð³Ñ€Ð°Ð½Ð¸Ñ†Ñ‹, Ð¿Ð¾ Ñ€Ð°Ð·Ð½Ñ‹Ðµ ÑÑ‚Ð¾Ñ€Ð¾Ð½Ñ‹ Ð³Ñ€Ð°Ð½Ð¸Ñ†Ñ‹ â€” Ñ€Ð°Ð·Ð½Ñ‹Ðµ Ð»Ð¾ÑÐºÑƒÑ‚Ñ‹.
void testFieldPatches() {
    std::set<int> crops;
    int balks = 0;
    for (int i = 0; i < 4000; ++i) {
        const float x = static_cast<float>((i * 7919) % 8192) + 0.37f;
        const float z = static_cast<float>((i * 104729) % 8192) + 0.61f;
        const lw::FieldPatch a = lw::fieldPatchAt(x, z);
        const lw::FieldPatch b = lw::fieldPatchAt(x, z);
        CHECK(a.id == b.id && a.crop == b.crop && a.angle == b.angle && a.edge == b.edge);
        CHECK(a.edge >= -1e-3f);
        CHECK(a.angle >= 0.0f && a.angle < 3.1416f);
        crops.insert(static_cast<int>(a.crop));
        if (a.edge < lw::kBalkWidth) {
            ++balks;
            // Ð§ÐµÑ€ÐµÐ· Ð³Ñ€Ð°Ð½Ð¸Ñ†Ñƒ (Ð²Ð´Ð²Ð¾Ðµ Ð´Ð°Ð»ÑŒÑˆÐµ Ð¼ÐµÐ¶Ð¸ Ð¾Ñ‚ Ñ‚Ð¾Ñ‡ÐºÐ¸) â€” ÑƒÐ¶Ðµ Ð´Ñ€ÑƒÐ³Ð¾Ð¹ Ð»Ð¾ÑÐºÑƒÑ‚ Ð³Ð´Ðµ-Ñ‚Ð¾ Ñ€ÑÐ´Ð¾Ð¼.
            bool other = false;
            for (int d = 0; d < 8 && !other; ++d) {
                const float angle = static_cast<float>(d) * 0.785f;
                other = lw::fieldPatchAt(x + 2.5f * std::cos(angle), z + 2.5f * std::sin(angle)).id != a.id;
            }
            CHECK(other);
        }
    }
    CHECK(crops.size() == 8); // Ð²ÑÐµ ÐºÑƒÐ»ÑŒÑ‚ÑƒÑ€Ñ‹ Ð²ÑÑ‚Ñ€ÐµÑ‡Ð°ÑŽÑ‚ÑÑ (Ð¾Ð³Ð¾Ñ€Ð¾Ð´ â€” Ð½Ðµ Ð»Ð¾ÑÐºÑƒÑ‚)
    CHECK(balks > 0);
}

// Ð¢ÐµÑ‡ÐµÐ½Ð¸Ðµ: ÐºÐ°Ð¶Ð´Ð°Ñ ÐºÐ»ÐµÑ‚ÐºÐ° Ñ€ÐµÐºÐ¸ Ñ‚ÐµÑ‡Ñ‘Ñ‚ Ðº ÑÐ¾ÑÐµÐ´Ð½ÐµÐ¹ Ð²Ð¾Ð´Ðµ, Ð¸ Ð²Ð½Ð¸Ð· Ð¿Ð¾ Ñ‚ÐµÑ‡ÐµÐ½Ð¸ÑŽ
// ÑƒÑ€Ð¾Ð²ÐµÐ½ÑŒ Ð²Ð¾Ð´Ñ‹ Ð½Ðµ Ñ€Ð°ÑÑ‚Ñ‘Ñ‚.
void testRiverFlow() {
    const lw::World world(3, regionConfig());
    const lw::Map& map = world.map();
    const lw::LandUse land(map);
    int rivers = 0;
    int flowing = 0;
    int rapid = 0;
    for (int y = 1; y + 1 < map.height(); ++y) {
        for (int x = 1; x + 1 < map.width(); ++x) {
            if (map.water.at(x, y) != lw::Water::River) {
                continue;
            }
            ++rivers;
            const lw::LandUse::Flow flow = land.flowAt({x, y});
            if (flow.speed <= 0.0f) {
                continue;
            }
            ++flowing;
            CHECK(std::abs(std::sqrt(flow.x * flow.x + flow.z * flow.z) - 1.0f) < 1e-3f);
            CHECK(flow.rapids >= 0.0f && flow.rapids <= 1.0f);
            rapid += flow.rapids > 0.3f ? 1 : 0;
            // Ð’Ð½Ð¸Ð· Ð¿Ð¾ Ñ‚ÐµÑ‡ÐµÐ½Ð¸ÑŽ (Ð½Ð° Ð´Ð²Ðµ ÐºÐ»ÐµÑ‚ÐºÐ¸) ÑƒÑ€Ð¾Ð²ÐµÐ½ÑŒ Ð²Ð¾Ð´Ñ‹ Ð½Ðµ Ð²Ñ‹ÑˆÐµ.
            const lw::Cell down = map.clampedCellAt(lw::Map::cellCenter(x) + flow.x * 8.0f,
                                                    lw::Map::cellCenter(y) + flow.z * 8.0f);
            if (map.water.at(down.x, down.y) == lw::Water::River) {
                const float here = map.elevationMeters({x, y}) + map.depthMeters({x, y});
                const float there = map.elevationMeters(down) + map.depthMeters(down);
                CHECK(there <= here + 0.25f);
            }
            CHECK(land.channelInset({x, y}) >= 0.3f - 1e-3f);
            CHECK(land.bankHeight({x, y}) > 0.0f);
        }
    }
    CHECK(rivers > 0);
    // Перекаты есть, но это меньшая часть реки.
    CHECK(rapid > 0 && rapid < rivers / 4);
    CHECK(flowing > rivers / 2);
}

// Ð”Ð²Ð¾Ñ€Ñ‹ Ð¸ Ð¿Ñ€ÐµÐ´Ð¼ÐµÑ‚Ñ‹: Ð¿Ð¾ Ð·Ð´Ð°Ð½Ð¸ÑÐ¼; Ð¾Ð´Ð¸Ð½Ð°ÐºÐ¾Ð²Ñ‹ Ð¿Ñ€Ð¸ Ð¿Ð¾Ð²Ñ‚Ð¾Ñ€Ðµ; Ð½Ðµ Ð½Ð° Ð´Ð¾Ñ€Ð¾Ð³Ð°Ñ… Ð¸ Ð½Ðµ Ð² Ð²Ð¾Ð´Ðµ.
void testYardsAndThings() {
    const lw::World world(5, regionConfig());
    const lw::Map& map = world.map();
    lw::LandUse a(map);
    lw::LandUse b(map);
    a.addSettlements(world);
    b.addSettlements(world);
    CHECK(a.hasSettlements());
    CHECK(a.lots().size() == b.lots().size());
    CHECK(a.things().size() == b.things().size());
    int yards = 0;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const lw::Cell cell{x, y};
            CHECK(a.flags(cell) == b.flags(cell));
            if (a.has(cell, lw::LandUse::kYard)) {
                ++yards;
                CHECK(map.road.at(x, y) == lw::Road::None);
                CHECK(map.water.at(x, y) == lw::Water::None);
                CHECK(a.lotOf(cell) > 0);
                CHECK(!a.has(cell, lw::LandUse::kBuilding));
            }
            if (a.has(cell, lw::LandUse::kGarden)) {
                CHECK(a.has(cell, lw::LandUse::kYard));
            }
        }
    }
    CHECK(yards > 0);
    int fences = 0;
    int poles = 0;
    for (std::size_t i = 0; i < a.things().size(); ++i) {
        const lw::LandUse::Thing& thing = a.things()[i];
        const lw::LandUse::Thing& same = b.things()[i];
        CHECK(thing.kind == same.kind && thing.x == same.x && thing.z == same.z);
        fences += thing.kind == lw::LandUse::ThingKind::Fence ? 1 : 0;
        if (thing.kind == lw::LandUse::ThingKind::Pole) {
            ++poles;
            const lw::Cell cell = map.clampedCellAt(thing.x, thing.z);
            CHECK(map.road.at(cell.x, cell.y) == lw::Road::None);
            // ÐŸÑ€Ð¾Ð²Ð¾Ð´ ÑÐ²ÑÐ·Ñ‹Ð²Ð°ÐµÑ‚ Ð¾Ð±Ð° ÑÑ‚Ð¾Ð»Ð±Ð°.
            for (const int link : thing.links) {
                if (link < 0) {
                    continue;
                }
                const lw::LandUse::Thing& other = a.things()[static_cast<std::size_t>(link)];
                CHECK(other.kind == lw::LandUse::ThingKind::Pole);
                CHECK(other.links[0] == static_cast<int>(i) || other.links[1] == static_cast<int>(i));
            }
        }
    }
    CHECK(fences > 0);
    CHECK(poles > 0);
    // ÐšÐ°Ð¶Ð´Ñ‹Ð¹ Ð¿Ñ€ÐµÐ´Ð¼ÐµÑ‚ â€” Ð² ÑÐ²Ð¾ÐµÐ¹ ÐºÐ¾Ñ€Ð·Ð¸Ð½Ðµ.
    std::size_t bucketed = 0;
    for (int by = 0; by * lw::LandUse::kBucketCells < map.height(); ++by) {
        for (int bx = 0; bx * lw::LandUse::kBucketCells < map.width(); ++bx) {
            bucketed += a.thingsIn(bx, by).size();
        }
    }
    CHECK(bucketed == a.things().size());
}

// Ð”ÐµÑ€ÐµÐ²ÑŒÑ Ð²Ð½Ðµ Ð»ÐµÑÐ°: Ð½Ðµ Ð½Ð° Ð´Ð¾Ñ€Ð¾Ð³Ð°Ñ…, Ð¿Ð¾Ð»ÑÑ…, Ð¿Ð¾Ð´ Ð·Ð´Ð°Ð½Ð¸ÑÐ¼Ð¸ Ð¸ Ð² Ð¾Ð³Ð¾Ñ€Ð¾Ð´Ð°Ñ…; Ð´ÐµÑ‚ÐµÑ€Ð¼Ð¸Ð½Ð¸Ñ€Ð¾Ð²Ð°Ð½Ñ‹.
void testOpenTrees() {
    const lw::World world(5, regionConfig());
    const lw::Map& map = world.map();
    lw::LandUse land(map);
    land.addSettlements(world);
    int trees = 0;
    int yardTrees = 0;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const lw::Cell cell{x, y};
            lw::TreeSpec first[lw::kMaxTreesPerCell];
            lw::TreeSpec second[lw::kMaxTreesPerCell];
            const int n = lw::openTreesInCell(land, cell, first);
            CHECK(n == lw::openTreesInCell(land, cell, second));
            if (n == 0) {
                continue;
            }
            CHECK(n <= lw::kMaxTreesPerCell);
            CHECK(map.cover.at(x, y) == lw::Cover::None);
            CHECK(map.road.at(x, y) == lw::Road::None);
            CHECK(map.ground.at(x, y) != lw::Ground::Field);
            CHECK(!land.has(cell, lw::LandUse::kBuilding) && !land.has(cell, lw::LandUse::kGarden));
            for (int i = 0; i < n; ++i) {
                CHECK(first[i].x == second[i].x && first[i].height == second[i].height);
                CHECK(lw::Map::cellAt(first[i].x, first[i].z) == cell);
            }
            trees += n;
            yardTrees += land.has(cell, lw::LandUse::kYard) ? n : 0;
        }
    }
    CHECK(trees > 0);
    CHECK(yardTrees > 0);
}

} // namespace

int main() {
    testFieldPatches();
    testRiverFlow();
    testYardsAndThings();
    testOpenTrees();
    if (failures == 0) {
        std::printf("land use tests passed\n");
    }
    return failures;
}
