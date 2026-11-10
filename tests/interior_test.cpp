// Тесты комнат внутри зданий (этап 12а.14) — без графики: планировка
// (render/Interior) детерминирована, комнаты и мебель не вылезают за стены и
// не налезают друг на друга, все комнаты дома связаны дверями, кроватей не
// меньше жильцов, окна прорезаны в наружных стенах, а рассадка кладёт спящих
// в кровати, едящих — за стол, и домашних — к общему столу.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "render/Interior.h"

#include <algorithm>
#include <cmath>
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

using namespace lw;

// Окна, как у дома снаружи: по 1,1 м через равные промежутки на каждой стене.
std::vector<WallOpening> windowsFor(float x0, float z0, float x1, float z1, float floor, int storeys) {
    std::vector<WallOpening> result;
    const auto wall = [&](bool alongX, float plane, float outward, float a0, float a1) {
        const float length = a1 - a0;
        const int count = static_cast<int>((length - 1.0f) / 3.1f);
        for (int storey = 0; storey < storeys; ++storey) {
            for (int i = 0; i < count; ++i) {
                const float center = a0 + length * (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
                WallOpening o;
                o.alongX = alongX;
                o.plane = plane;
                o.outward = outward;
                o.a0 = center - 0.55f;
                o.a1 = center + 0.55f;
                o.y0 = floor + 3.0f * static_cast<float>(storey) + 0.9f;
                o.y1 = o.y0 + 1.3f;
                o.storey = storey;
                result.push_back(o);
            }
        }
    };
    wall(true, z0, -1.0f, x0, x1);
    wall(true, z1, 1.0f, x0, x1);
    wall(false, x0, -1.0f, z0, z1);
    wall(false, x1, 1.0f, z0, z1);
    return result;
}

InteriorInput houseInput(float width, float depth, int doorSide, std::uint64_t seed, int residents) {
    InteriorInput in;
    in.kind = BuildingKind::House;
    in.x0 = 100.0f;
    in.z0 = 200.0f;
    in.x1 = in.x0 + width;
    in.z1 = in.z0 + depth;
    in.floor = 10.0f;
    in.storey = 3.0f;
    in.storeys = 1;
    switch (doorSide) {
    case 0:
        in.doorX = in.x0;
        in.doorZ = in.z0 + depth * 0.5f;
        break;
    case 1:
        in.doorX = in.x1;
        in.doorZ = in.z0 + depth * 0.4f;
        break;
    case 2:
        in.doorX = in.x0 + width * 0.6f;
        in.doorZ = in.z0;
        break;
    default:
        in.doorX = in.x0 + width * 0.5f;
        in.doorZ = in.z1;
        break;
    }
    in.seed = seed;
    in.openings = windowsFor(in.x0, in.z0, in.x1, in.z1, in.floor, 1);
    in.residents = {residents};
    return in;
}

bool solid(FurnitureKind kind) {
    switch (kind) {
    case FurnitureKind::Rug:
    case FurnitureKind::Lamp:
    case FurnitureKind::Door:
    case FurnitureKind::Board:
    case FurnitureKind::Bars:
        return false;
    default:
        return true;
    }
}

// Комнаты, которые соединяет дверь в стене (по обе стороны проёма).
std::vector<std::pair<int, int>> doorLinks(const InteriorLayout& layout) {
    std::vector<std::pair<int, int>> links;
    for (const InteriorWall& wall : layout.walls) {
        for (const WallGap& gap : wall.gaps) {
            if (gap.window) {
                continue;
            }
            const float along = (gap.a0 + gap.a1) * 0.5f;
            int lo = -1;
            int hi = -1;
            for (std::size_t i = 0; i < layout.rooms.size(); ++i) {
                const InteriorRoom& r = layout.rooms[i];
                const float r0 = wall.alongX ? r.z0 : r.x0;
                const float r1 = wall.alongX ? r.z1 : r.x1;
                const float a0 = wall.alongX ? r.x0 : r.z0;
                const float a1 = wall.alongX ? r.x1 : r.z1;
                if (along < a0 || along > a1) {
                    continue;
                }
                if (std::abs(r1 - wall.lo) < 0.02f) {
                    lo = static_cast<int>(i);
                }
                if (std::abs(r0 - wall.hi) < 0.02f) {
                    hi = static_cast<int>(i);
                }
            }
            if (lo >= 0 && hi >= 0) {
                links.emplace_back(lo, hi);
            }
        }
    }
    return links;
}

// Все комнаты одной квартиры (и лестница) связаны дверями.
bool connected(const InteriorLayout& layout, int flat) {
    std::vector<int> rooms;
    for (std::size_t i = 0; i < layout.rooms.size(); ++i) {
        if (layout.rooms[i].flat == flat) {
            rooms.push_back(static_cast<int>(i));
        }
    }
    if (rooms.empty()) {
        return true;
    }
    const auto links = doorLinks(layout);
    std::vector<int> seen = {rooms.front()};
    for (std::size_t k = 0; k < seen.size(); ++k) {
        for (const auto& [a, b] : links) {
            const int other = a == seen[k] ? b : b == seen[k] ? a : -1;
            if (other >= 0 && layout.rooms[static_cast<std::size_t>(other)].flat == flat &&
                std::find(seen.begin(), seen.end(), other) == seen.end()) {
                seen.push_back(other);
            }
        }
    }
    return seen.size() == rooms.size();
}

void checkLayout(const InteriorLayout& layout, int residents, bool home) {
    // Комнаты — внутри стен и не налезают друг на друга.
    for (std::size_t i = 0; i < layout.rooms.size(); ++i) {
        const InteriorRoom& r = layout.rooms[i];
        CHECK(r.x1 - r.x0 > 1.5f && r.z1 - r.z0 > 1.5f);
        CHECK(r.x0 >= layout.x0 - 0.01f && r.x1 <= layout.x1 + 0.01f);
        CHECK(r.z0 >= layout.z0 - 0.01f && r.z1 <= layout.z1 + 0.01f);
        for (std::size_t j = i + 1; j < layout.rooms.size(); ++j) {
            const InteriorRoom& o = layout.rooms[j];
            const bool overlap = r.x0 < o.x1 - 0.01f && o.x0 < r.x1 - 0.01f && r.z0 < o.z1 - 0.01f && o.z0 < r.z1 - 0.01f;
            CHECK(!overlap);
        }
    }
    // Мебель — в своей комнате и не налезает на другую.
    for (std::size_t i = 0; i < layout.furniture.size(); ++i) {
        const Furniture& f = layout.furniture[i];
        CHECK(f.room >= 0 && f.room < static_cast<int>(layout.rooms.size()));
        if (f.room < 0) {
            continue;
        }
        const InteriorRoom& r = layout.rooms[static_cast<std::size_t>(f.room)];
        CHECK(f.minX() >= r.x0 - 0.02f && f.maxX() <= r.x1 + 0.02f);
        CHECK(f.minZ() >= r.z0 - 0.02f && f.maxZ() <= r.z1 + 0.02f);
        if (!solid(f.kind)) {
            continue;
        }
        for (std::size_t j = i + 1; j < layout.furniture.size(); ++j) {
            const Furniture& o = layout.furniture[j];
            if (!solid(o.kind)) {
                continue;
            }
            const bool overlap = f.minX() < o.maxX() - 0.02f && o.minX() < f.maxX() - 0.02f &&
                                 f.minZ() < o.maxZ() - 0.02f && o.minZ() < f.maxZ() - 0.02f;
            if (overlap) {
                std::printf("  overlap: kind %d and %d in rooms %d/%d\n", static_cast<int>(f.kind),
                            static_cast<int>(o.kind), f.room, o.room);
            }
            CHECK(!overlap);
        }
    }
    // Места — в комнатах.
    for (const InteriorSpot& s : layout.spots) {
        CHECK(s.room >= 0 && layout.roomAt(s.x, s.z) == s.room);
        CHECK(std::isfinite(s.heading));
    }
    if (!home) {
        return;
    }
    for (int flat = 0; flat < layout.flats; ++flat) {
        int beds = 0;
        int seats = 0;
        bool kitchen = false;
        for (const InteriorSpot& s : layout.spots) {
            if (s.flat == flat) {
                beds += s.kind == SpotKind::Bed ? 1 : 0;
                seats += s.kind == SpotKind::Seat ? 1 : 0;
            }
        }
        for (const InteriorRoom& r : layout.rooms) {
            kitchen = kitchen || (r.flat == flat && r.kind == RoomKind::Kitchen);
        }
        CHECK(beds >= std::min(residents, 4));
        if (kitchen && seats < 2) {
            for (const InteriorRoom& r : layout.rooms) {
                if (r.flat == flat && r.kind == RoomKind::Kitchen) {
                    std::printf("  kitchen without table: %.2f x %.2f (kind %d, level %d)\n", r.x1 - r.x0,
                                r.z1 - r.z0, static_cast<int>(layout.kind), layout.level);
                }
            }
        }
        CHECK(!kitchen || seats >= 2);
        CHECK(connected(layout, flat));
    }
    // Окна прорезаны в наружных стенах.
    int windows = 0;
    for (const InteriorWall& wall : layout.walls) {
        for (const WallGap& gap : wall.gaps) {
            windows += gap.window ? 1 : 0;
            CHECK(!gap.window || wall.outer);
        }
    }
    CHECK(windows > 0);
    CHECK(!layout.lamps.empty());
}

bool same(const InteriorLayout& a, const InteriorLayout& b) {
    if (a.rooms.size() != b.rooms.size() || a.walls.size() != b.walls.size() ||
        a.furniture.size() != b.furniture.size() || a.spots.size() != b.spots.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.furniture.size(); ++i) {
        if (a.furniture[i].kind != b.furniture[i].kind || a.furniture[i].x != b.furniture[i].x ||
            a.furniture[i].z != b.furniture[i].z) {
            return false;
        }
    }
    return true;
}

void testHouses() {
    int layouts = 0;
    for (const float width : {8.0f, 12.0f}) {
        for (const float depth : {8.0f, 12.0f}) {
            for (int side = 0; side < 4; ++side) {
                for (std::uint64_t seed = 1; seed <= 6; ++seed) {
                    const int residents = 1 + static_cast<int>(seed % 4);
                    const InteriorInput in = houseInput(width, depth, side, seed * 7919u, residents);
                    const InteriorLayout layout = planInterior(in);
                    checkLayout(layout, residents, true);
                    CHECK(same(layout, planInterior(in))); // детерминизм
                    CHECK(std::abs(layout.floor - in.floor) < 1e-4f);
                    CHECK(layout.ceiling > layout.floor + 2.4f);
                    ++layouts;
                }
            }
        }
    }
    CHECK(layouts == 96);
    // Разные дома — разные планировки.
    const InteriorLayout a = planInterior(houseInput(12.0f, 12.0f, 3, 11, 3));
    const InteriorLayout b = planInterior(houseInput(12.0f, 12.0f, 3, 12, 3));
    CHECK(!same(a, b));
}

void testApartment() {
    for (const float length : {24.0f, 28.0f, 32.0f}) {
        for (int level = 0; level < 4; ++level) {
            InteriorInput in;
            in.kind = BuildingKind::Apartment;
            in.x0 = 0.0f;
            in.z0 = 0.0f;
            in.x1 = length;
            in.z1 = 12.0f;
            in.floor = 5.0f;
            in.storey = 3.0f;
            in.storeys = 5;
            in.doorX = length * 0.45f;
            in.doorZ = 12.0f;
            in.seed = 99u + static_cast<std::uint64_t>(level);
            in.openings = windowsFor(in.x0, in.z0, in.x1, in.z1, in.floor, 5);
            in.level = level;
            const int flats = flatsPerLevel(in.kind, in.x0, in.z0, in.x1, in.z1);
            in.residents.assign(static_cast<std::size_t>(flats), 3);
            const InteriorLayout layout = planInterior(in);
            CHECK(layout.flats == flats);
            CHECK(flats >= 2);
            CHECK(std::abs(layout.floor - (in.floor + 3.0f * static_cast<float>(level))) < 1e-4f);
            bool stairs = false;
            for (const InteriorRoom& r : layout.rooms) {
                stairs = stairs || r.kind == RoomKind::Stairs;
            }
            CHECK(stairs);
            checkLayout(layout, 3, true);
        }
    }
    CHECK(livingLevels(BuildingKind::Apartment, 5) == 5);
    CHECK(livingLevels(BuildingKind::House, 2) == 1);
}

void testOtherKinds() {
    const struct {
        BuildingKind kind;
        float w;
        float d;
    } cases[] = {{BuildingKind::Shop, 14.0f, 12.0f},
                 {BuildingKind::PoliceStation, 12.0f, 12.0f},
                 {BuildingKind::Hospital, 28.0f, 12.0f},
                 {BuildingKind::School, 32.0f, 12.0f},
                 {BuildingKind::Barn, 16.0f, 12.0f}};
    for (const auto& c : cases) {
        InteriorInput in;
        in.kind = c.kind;
        in.x1 = c.w;
        in.z1 = c.d;
        in.storey = c.kind == BuildingKind::Barn ? 4.5f : 3.5f;
        in.doorX = c.w * 0.5f;
        in.doorZ = c.d;
        in.seed = 5;
        in.openings = windowsFor(0.0f, 0.0f, c.w, c.d, 0.0f, 1);
        const InteriorLayout layout = planInterior(in);
        CHECK(!layout.rooms.empty());
        CHECK(!layout.furniture.empty());
        checkLayout(layout, 0, false);
    }
}

void testPlacement() {
    const InteriorLayout layout = planInterior(houseInput(12.0f, 12.0f, 3, 4242, 4));
    // Двое спят, один ест, один отдыхает: едящий за столом, отдыхающий — с ним
    // (общий ужин), спящие — в кроватях.
    std::vector<Occupant> occupants = {
        {10, 0, Occupation::Sleep}, {11, 0, Occupation::Eat}, {12, 0, Occupation::Rest}, {13, 0, Occupation::Sleep}};
    const std::vector<Placement> placed = placeOccupants(layout, occupants);
    CHECK(placed.size() == 4);
    std::vector<int> spots;
    for (const Placement& p : placed) {
        CHECK(p.room >= 0);
        if (p.spot >= 0) {
            CHECK(std::find(spots.begin(), spots.end(), p.spot) == spots.end()); // у каждого своё место
            spots.push_back(p.spot);
        }
        if (p.what == Occupation::Sleep) {
            CHECK(p.kind == SpotKind::Bed);
            CHECK(p.height > 0.4f);
        }
        if (p.what == Occupation::Eat || p.what == Occupation::Rest) {
            CHECK(p.kind == SpotKind::Seat);
            CHECK(p.meal);
        }
    }
    // Тот же вход в другом порядке — те же места.
    std::vector<Occupant> shuffled = {occupants[3], occupants[1], occupants[0], occupants[2]};
    const std::vector<Placement> again = placeOccupants(layout, shuffled);
    for (std::size_t i = 0; i < placed.size(); ++i) {
        CHECK(placed[i].id == again[i].id && placed[i].spot == again[i].spot);
    }
    // Без еды отдыхающий — на диване.
    const std::vector<Placement> rest = placeOccupants(layout, {{20, 0, Occupation::Rest}});
    CHECK(rest.size() == 1 && rest[0].kind == SpotKind::Sofa && !rest[0].meal);
    // Мест не хватило — всё равно стоит в комнате.
    std::vector<Occupant> crowd;
    for (EntityId id = 100; id < 130; ++id) {
        crowd.push_back({id, 0, Occupation::Other});
    }
    for (const Placement& p : placeOccupants(layout, crowd)) {
        CHECK(p.room >= 0 && std::isfinite(p.x) && std::isfinite(p.z));
    }
}

} // namespace

int main() {
    testHouses();
    testApartment();
    testOtherKinds();
    testPlacement();
    if (failures == 0) {
        std::printf("interior: all tests passed\n");
    } else {
        std::printf("interior: %d failure(s)\n", failures);
    }
    return failures;
}
