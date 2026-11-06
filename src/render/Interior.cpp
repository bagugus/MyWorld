#include "render/Interior.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace lw {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kOuterWall = 0.25f; // наружная стена изнутри участка
constexpr float kInnerWall = 0.12f; // перегородка
constexpr float kDoorWidth = 0.9f;
constexpr float kDoorHeight = 2.05f;
constexpr float kDoorClear = 0.85f; // перед дверью мебели нет
constexpr float kGap = 0.03f;       // мебель — чуть от стены
constexpr float kSeat = 0.46f;      // высота сиденья стула
constexpr float kSofaSeat = 0.44f;
constexpr float kMattress = 0.55f;
// Квартира в многоэтажке — около стольких метров вдоль дома; лестница — шириной.
constexpr float kFlatWidth = 8.0f;
constexpr float kStairsWidth = 3.0f;

// Стороны прямоугольника: 0 — запад (−X), 1 — восток (+X), 2 — север (−Z), 3 — юг (+Z).
constexpr int kWest = 0;
constexpr int kEast = 1;
constexpr int kNorth = 2;
constexpr int kSouth = 3;

int opposite(int side) {
    return side ^ 1;
}

bool alongXSide(int side) {
    return side == kNorth || side == kSouth;
}

// Куда смотрит перёд предмета у стены side (внутрь комнаты).
float facingFrom(int side) {
    switch (side) {
    case kWest:
        return kPi * 0.5f;
    case kEast:
        return -kPi * 0.5f;
    case kNorth:
        return 0.0f;
    default:
        return kPi;
    }
}

struct Rect {
    float x0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float z1 = 0.0f;
    float w() const { return x1 - x0; }
    float d() const { return z1 - z0; }
    float cx() const { return (x0 + x1) * 0.5f; }
    float cz() const { return (z0 + z1) * 0.5f; }
    bool overlaps(const Rect& o, float margin = 0.0f) const {
        return x0 < o.x1 + margin && o.x0 < x1 + margin && z0 < o.z1 + margin && o.z0 < z1 + margin;
    }
    bool inside(const Rect& outer) const {
        return x0 >= outer.x0 - 1e-3f && z0 >= outer.z0 - 1e-3f && x1 <= outer.x1 + 1e-3f && z1 <= outer.z1 + 1e-3f;
    }
    // Вдоль стороны side: начало и конец; поперёк — координата стороны.
    float from(int side) const { return alongXSide(side) ? x0 : z0; }
    float to(int side) const { return alongXSide(side) ? x1 : z1; }
    float at(int side) const {
        switch (side) {
        case kWest:
            return x0;
        case kEast:
            return x1;
        case kNorth:
            return z0;
        default:
            return z1;
        }
    }
};

struct Span {
    float a0 = 0.0f;
    float a1 = 0.0f;
    float y0 = 0.0f;
    float y1 = 0.0f;
    bool door = false;
};

Rgb rgb(int r, int g, int b) {
    return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)};
}

constexpr std::array<Rgb, 8> kWallpapers = {{{214, 204, 180}, {196, 206, 190}, {206, 190, 176}, {188, 198, 212},
                                             {222, 214, 196}, {204, 184, 160}, {180, 192, 176}, {216, 196, 190}}};
constexpr Rgb kTileWall = {206, 214, 214};
constexpr Rgb kPlaster = {200, 198, 192};
constexpr Rgb kHospitalWall = {214, 224, 222};
constexpr Rgb kBarnWall = {132, 104, 76};
constexpr std::array<Rgb, 4> kWoodFloors = {{{140, 100, 66}, {120, 86, 58}, {156, 118, 80}, {104, 76, 54}}};
constexpr Rgb kTileFloor = {170, 168, 160};
constexpr Rgb kLinoleum = {150, 140, 112};
constexpr Rgb kConcreteFloor = {128, 126, 120};
constexpr Rgb kBarnFloor = {110, 92, 68};

class Planner {
public:
    Planner(const InteriorInput& in, InteriorLayout& out) : in_(in), out_(out), state_(in.seed ^ 0x51F15EEDULL) {}

    void run();

private:
    struct RoomData {
        std::vector<Rect> taken;
        std::vector<Rect> soft; // проходы: стол туда не ставить, стулья — можно
        std::array<std::vector<Span>, 4> windows;
        std::array<bool, 4> outer{};
        std::array<int, 4> doors{}; // сколько дверей в стороне
    };

    float unit() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1ULL << 24);
    }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    std::uint32_t bits() { return static_cast<std::uint32_t>(unit() * 4294967040.0f); }

    Rect roomRect(int room) const {
        const InteriorRoom& r = out_.rooms[static_cast<std::size_t>(room)];
        return {r.x0, r.z0, r.x1, r.z1};
    }
    int addRoom(RoomKind kind, const Rect& rect, int flat);
    // Разрезать прямоугольник перегородкой: вдоль X на координате at (стена
    // вдоль Z) или вдоль Z. Сдвигает разрез от окон на наружных стенах.
    float safeSplit(bool cutX, float at, float lo, float hi) const;
    // Перегородка между соседними комнатами по их общему отрезку; door — с
    // дверью (bias — где вдоль, 0..1). false — не соседи или без двери.
    bool partition(int a, int b, bool door, float bias = 0.5f);
    void outerWalls();
    void addFurniture(FurnitureKind kind, int room, const Rect& rect, float facing, float height, Rgb color,
                      float base = 0.0f);
    std::optional<Rect> against(int room, int side, float length, float depth, bool tall, float prefer) const;
    // Свободно ли место; soft — и проходы (в них могут заходить только стулья).
    bool free(int room, const Rect& rect, bool soft = true) const;
    void take(int room, const Rect& rect) { data_[static_cast<std::size_t>(room)].taken.push_back(rect); }
    void spot(SpotKind kind, float x, float z, float heading, float height, int room);
    // Стороны комнаты по убыванию длины, без дверей впереди.
    std::array<int, 4> sidesByLength(int room) const;
    void lamp(int room);

    // Квартира или дом в прямоугольнике: вход со стороны entry; передние комнаты.
    std::vector<int> planHome(const Rect& rect, int entry, int flat);
    void planApartmentLevel();
    // Холл у входа во всю глубину и крылья комнат по обе стороны (больница, школа).
    void planHallWings(RoomKind hall, RoomKind wing, float wingWidth, float hallWidth);
    // Передний зал и узкая задняя полоса (магазин, полиция).
    void planFrontBack(RoomKind front, RoomKind back, float backDepth);
    void furnishAll();
    int residents(int flat) const;

    void furnishKitchen(int room, int residents);
    void furnishSales(int room);
    void furnishStoreroom(int room);
    void furnishCells(int room);
    void furnishBarn(int room);
    void furnishLiving(int room, int entry);
    // Кровати в спальне; сколько спальных мест поставили.
    int furnishBedroom(int room, int beds);
    // Кровать у какой-нибудь стены: двуспальная или нет, изголовьем к стене или боком.
    bool bed(int room, bool wide, bool alongWall);
    void furnishBath(int room);
    void furnishStairs(int room);
    void furnishWard(int room);
    void furnishClassroom(int room);
    void furnishOffice(int room);
    void furnishHall(int room);
    void entryDoor(int room, int side, float along);
    void table(int room, int seats, float avoidSide);

    const InteriorInput& in_;
    InteriorLayout& out_;
    std::uint64_t state_;
    std::vector<RoomData> data_;
    std::array<std::vector<Span>, 4> buildingWindows_; // по сторонам здания, этот этаж
    int doorSide_ = kSouth;
    float doorAlong_ = 0.0f;
    Rect inner_;
    std::vector<int> flatEntry_;    // сторона входа каждой квартиры
    std::vector<std::pair<int, int>> panels_; // закрытые двери квартир: комната, сторона
};

int Planner::addRoom(RoomKind kind, const Rect& rect, int flat) {
    InteriorRoom room;
    room.kind = kind;
    room.x0 = rect.x0;
    room.z0 = rect.z0;
    room.x1 = rect.x1;
    room.z1 = rect.z1;
    room.flat = flat;
    switch (kind) {
    case RoomKind::Kitchen:
    case RoomKind::Bath:
        room.wall = kind == RoomKind::Bath ? kTileWall : kWallpapers[bits() % kWallpapers.size()];
        room.floor = kTileFloor;
        break;
    case RoomKind::Stairs:
    case RoomKind::Storeroom:
    case RoomKind::Cells:
        room.wall = kPlaster;
        room.floor = kConcreteFloor;
        break;
    case RoomKind::Ward:
        room.wall = kHospitalWall;
        room.floor = kLinoleum;
        break;
    case RoomKind::Sales:
    case RoomKind::Office:
    case RoomKind::Classroom:
        room.wall = kind == RoomKind::Classroom ? rgb(196, 210, 190) : kPlaster;
        room.floor = kLinoleum;
        break;
    case RoomKind::Barn:
        room.wall = kBarnWall;
        room.floor = kBarnFloor;
        break;
    default:
        room.wall = kWallpapers[bits() % kWallpapers.size()];
        room.floor = kWoodFloors[bits() % kWoodFloors.size()];
        break;
    }
    out_.rooms.push_back(room);
    data_.emplace_back();
    return static_cast<int>(out_.rooms.size()) - 1;
}

float Planner::safeSplit(bool cutX, float at, float lo, float hi) const {
    // Перегородка, идущая поперёк наружной стены, не должна упираться в окно.
    const std::array<int, 2> walls = cutX ? std::array<int, 2>{kNorth, kSouth} : std::array<int, 2>{kWest, kEast};
    const auto clear = [&](float c) {
        if (c < lo || c > hi) {
            return false;
        }
        for (const int side : walls) {
            for (const Span& span : buildingWindows_[static_cast<std::size_t>(side)]) {
                if (c + kInnerWall * 0.5f + 0.12f > span.a0 && c - kInnerWall * 0.5f - 0.12f < span.a1) {
                    return false;
                }
            }
        }
        return true;
    };
    for (int k = 0; k <= 24; ++k) {
        const float offset = static_cast<float>((k + 1) / 2) * 0.1f * (k % 2 == 0 ? 1.0f : -1.0f);
        if (clear(at + offset)) {
            return at + offset;
        }
    }
    return std::clamp(at, lo, hi);
}

bool Planner::partition(int a, int b, bool door, float bias) {
    Rect ra = roomRect(a);
    Rect rb = roomRect(b);
    // Комнаты разделены по X (стена вдоль Z) или по Z (стена вдоль X); a — со стороны меньших координат.
    const bool byX = ra.x1 <= rb.x0 + 0.2f || rb.x1 <= ra.x0 + 0.2f;
    const bool alongX = !byX;
    if ((byX && ra.x0 > rb.x0) || (!byX && ra.z0 > rb.z0)) {
        std::swap(a, b);
        std::swap(ra, rb);
    }
    InteriorWall wall;
    wall.alongX = alongX;
    if (alongX) {
        wall.lo = ra.z1;
        wall.hi = rb.z0;
        wall.a0 = std::max(ra.x0, rb.x0);
        wall.a1 = std::min(ra.x1, rb.x1);
    } else {
        wall.lo = ra.x1;
        wall.hi = rb.x0;
        wall.a0 = std::max(ra.z0, rb.z0);
        wall.a1 = std::min(ra.z1, rb.z1);
    }
    if (wall.a1 - wall.a0 < 0.05f || wall.hi - wall.lo > 0.5f || wall.hi < wall.lo - 0.01f) {
        return false; // не соседи
    }
    std::vector<float> doors;
    if (door && wall.a1 - wall.a0 >= kDoorWidth + 0.4f) {
        const float lo = wall.a0 + 0.15f + kDoorWidth * 0.5f;
        const float hi = wall.a1 - 0.15f - kDoorWidth * 0.5f;
        doors.push_back(lo + (hi - lo) * std::clamp(bias + range(-0.1f, 0.1f), 0.0f, 1.0f));
    }
    wall.colorLo = out_.rooms[static_cast<std::size_t>(a)].wall;
    wall.colorHi = out_.rooms[static_cast<std::size_t>(b)].wall;
    for (const float center : doors) {
        WallGap gap;
        gap.a0 = center - kDoorWidth * 0.5f;
        gap.a1 = center + kDoorWidth * 0.5f;
        gap.y0 = 0.0f;
        gap.y1 = kDoorHeight;
        wall.gaps.push_back(gap);
        // Перед дверью с обеих сторон — свободно.
        const float g0 = gap.a0 - 0.1f;
        const float g1 = gap.a1 + 0.1f;
        if (alongX) {
            take(a, {g0, ra.z1 - kDoorClear, g1, ra.z1});
            take(b, {g0, rb.z0, g1, rb.z0 + kDoorClear});
            ++data_[static_cast<std::size_t>(a)].doors[kSouth];
            ++data_[static_cast<std::size_t>(b)].doors[kNorth];
        } else {
            take(a, {ra.x1 - kDoorClear, g0, ra.x1, g1});
            take(b, {rb.x0, g0, rb.x0 + kDoorClear, g1});
            ++data_[static_cast<std::size_t>(a)].doors[kEast];
            ++data_[static_cast<std::size_t>(b)].doors[kWest];
        }
    }
    wall.a0 -= kInnerWall * 0.5f;
    wall.a1 += kInnerWall * 0.5f;
    out_.walls.push_back(std::move(wall));
    return !doors.empty();
}

void Planner::outerWalls() {
    // Наружные стены — по кускам вдоль комнат: у каждой свои обои.
    const Rect outer{out_.outerX0, out_.outerZ0, out_.outerX1, out_.outerZ1};
    for (std::size_t i = 0; i < out_.rooms.size(); ++i) {
        const Rect r = roomRect(static_cast<int>(i));
        for (int side = 0; side < 4; ++side) {
            if (std::abs(r.at(side) - inner_.at(side)) > 0.02f) {
                continue;
            }
            data_[i].outer[static_cast<std::size_t>(side)] = true;
            InteriorWall wall;
            wall.outer = true;
            wall.alongX = alongXSide(side);
            const bool low = side == kWest || side == kNorth;
            const float face = r.at(side);
            const float edge = outer.at(side);
            wall.lo = low ? edge : face;
            wall.hi = low ? face : edge;
            // Вдоль: до середины соседних перегородок, в углах — до угла здания.
            const int before = alongXSide(side) ? kWest : kNorth;
            const int after = alongXSide(side) ? kEast : kSouth;
            wall.a0 = std::abs(r.from(side) - inner_.at(before)) < 0.02f ? outer.at(before) : r.from(side) - kInnerWall * 0.5f;
            wall.a1 = std::abs(r.to(side) - inner_.at(after)) < 0.02f ? outer.at(after) : r.to(side) + kInnerWall * 0.5f;
            wall.colorLo = wall.colorHi = out_.rooms[i].wall;
            for (const Span& span : buildingWindows_[static_cast<std::size_t>(side)]) {
                if (span.a1 <= r.from(side) + 0.05f || span.a0 >= r.to(side) - 0.05f) {
                    continue;
                }
                WallGap gap;
                gap.a0 = std::max(span.a0, r.from(side));
                gap.a1 = std::min(span.a1, r.to(side));
                gap.y0 = span.y0;
                gap.y1 = span.y1;
                gap.window = true;
                wall.gaps.push_back(gap);
                data_[i].windows[static_cast<std::size_t>(side)].push_back({gap.a0, gap.a1, gap.y0, gap.y1, false});
            }
            out_.walls.push_back(std::move(wall));
        }
    }
}

bool Planner::free(int room, const Rect& rect, bool soft) const {
    if (!rect.inside(roomRect(room))) {
        return false;
    }
    const RoomData& data = data_[static_cast<std::size_t>(room)];
    for (const Rect& t : data.taken) {
        if (rect.overlaps(t, -0.01f)) {
            return false;
        }
    }
    if (soft) {
        for (const Rect& t : data.soft) {
            if (rect.overlaps(t, -0.01f)) {
                return false;
            }
        }
    }
    return true;
}

std::optional<Rect> Planner::against(int room, int side, float length, float depth, bool tall, float prefer) const {
    const Rect r = roomRect(room);
    const float a0 = r.from(side);
    const float a1 = r.to(side);
    const float span = a1 - a0;
    if (length > span - 2.0f * kGap || depth > (alongXSide(side) ? r.d() : r.w()) - 0.3f) {
        return std::nullopt;
    }
    const float center = a0 + kGap + length * 0.5f + (span - 2.0f * kGap - length) * std::clamp(prefer, 0.0f, 1.0f);
    for (int k = 0; k < 80; ++k) {
        const float offset = static_cast<float>((k + 1) / 2) * 0.1f * (k % 2 == 0 ? 1.0f : -1.0f);
        const float s = center + offset - length * 0.5f;
        if (s < a0 + kGap - 1e-3f || s + length > a1 - kGap + 1e-3f) {
            continue;
        }
        Rect c;
        switch (side) {
        case kWest:
            c = {r.x0 + kGap, s, r.x0 + kGap + depth, s + length};
            break;
        case kEast:
            c = {r.x1 - kGap - depth, s, r.x1 - kGap, s + length};
            break;
        case kNorth:
            c = {s, r.z0 + kGap, s + length, r.z0 + kGap + depth};
            break;
        default:
            c = {s, r.z1 - kGap - depth, s + length, r.z1 - kGap};
            break;
        }
        if (!free(room, c)) {
            continue;
        }
        if (tall) {
            bool blocked = false;
            for (const Span& window : data_[static_cast<std::size_t>(room)].windows[static_cast<std::size_t>(side)]) {
                blocked = blocked || (s < window.a1 + 0.1f && window.a0 - 0.1f < s + length);
            }
            if (blocked) {
                continue;
            }
        }
        return c;
    }
    return std::nullopt;
}

void Planner::addFurniture(FurnitureKind kind, int room, const Rect& rect, float facing, float height, Rgb color,
                           float base) {
    Furniture f;
    f.kind = kind;
    f.x = rect.cx();
    f.z = rect.cz();
    // Ширина — поперёк фронта: у предмета, смотрящего по Z, — вдоль X.
    const bool alongZ = std::abs(std::sin(facing)) < 0.5f;
    f.width = alongZ ? rect.w() : rect.d();
    f.depth = alongZ ? rect.d() : rect.w();
    f.height = height;
    f.base = base;
    f.facing = facing;
    f.color = color;
    f.room = room;
    f.seed = bits();
    out_.furniture.push_back(f);
    if (kind != FurnitureKind::Rug && kind != FurnitureKind::Lamp) {
        take(room, rect);
    }
}

void Planner::spot(SpotKind kind, float x, float z, float heading, float height, int room) {
    InteriorSpot s;
    s.kind = kind;
    s.x = x;
    s.z = z;
    s.heading = heading;
    s.height = height;
    s.room = room;
    s.flat = out_.rooms[static_cast<std::size_t>(room)].flat;
    out_.spots.push_back(s);
}

std::array<int, 4> Planner::sidesByLength(int room) const {
    const Rect r = roomRect(room);
    std::array<int, 4> sides = {kWest, kEast, kNorth, kSouth};
    const RoomData& data = data_[static_cast<std::size_t>(room)];
    std::stable_sort(sides.begin(), sides.end(), [&](int a, int b) {
        const float la = r.to(a) - r.from(a) - (data.doors[static_cast<std::size_t>(a)] > 0 ? 100.0f : 0.0f);
        const float lb = r.to(b) - r.from(b) - (data.doors[static_cast<std::size_t>(b)] > 0 ? 100.0f : 0.0f);
        return la > lb;
    });
    return sides;
}

void Planner::lamp(int room) {
    const Rect r = roomRect(room);
    const float area = r.w() * r.d();
    const int count = area > 40.0f ? 2 : 1;
    const bool alongX = r.w() >= r.d();
    const float height = out_.ceiling - out_.floor;
    for (int i = 0; i < count; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float x = alongX ? r.x0 + r.w() * t : r.cx();
        const float z = alongX ? r.cz() : r.z0 + r.d() * t;
        addFurniture(FurnitureKind::Lamp, room, {x - 0.2f, z - 0.2f, x + 0.2f, z + 0.2f}, 0.0f, 0.22f,
                     rgb(236, 220, 180), height - 0.55f);
        out_.lamps.push_back({x, out_.floor + height - 0.6f, z, room});
    }
}

void Planner::entryDoor(int room, int side, float along) {
    const Rect r = roomRect(room);
    const float a = std::clamp(along, r.from(side) + 0.5f, r.to(side) - 0.5f);
    Rect panel;
    switch (side) {
    case kWest:
        panel = {r.x0, a - 0.45f, r.x0 + 0.05f, a + 0.45f};
        break;
    case kEast:
        panel = {r.x1 - 0.05f, a - 0.45f, r.x1, a + 0.45f};
        break;
    case kNorth:
        panel = {a - 0.45f, r.z0, a + 0.45f, r.z0 + 0.05f};
        break;
    default:
        panel = {a - 0.45f, r.z1 - 0.05f, a + 0.45f, r.z1};
        break;
    }
    addFurniture(FurnitureKind::Door, room, panel, facingFrom(side), kDoorHeight, rgb(96, 66, 46));
    // Перед дверью свободно.
    Rect clear = panel;
    switch (side) {
    case kWest:
        clear.x1 = r.x0 + kDoorClear;
        break;
    case kEast:
        clear.x0 = r.x1 - kDoorClear;
        break;
    case kNorth:
        clear.z1 = r.z0 + kDoorClear;
        break;
    default:
        clear.z0 = r.z1 - kDoorClear;
        break;
    }
    take(room, clear);
    ++data_[static_cast<std::size_t>(room)].doors[static_cast<std::size_t>(side)];
}

// --- Обстановка -----------------------------------------------------------------

void Planner::table(int room, int seats, float avoidSide) {
    const Rect r = roomRect(room);
    constexpr float chair = 0.45f;
    const float reach = chair + 0.15f;
    // Стол вдоль длинной стороны комнаты, поближе к середине и подальше от
    // рабочей стены. В тесной кухне — к стене, стулья с трёх, двух или одной стороны.
    const bool alongX = r.w() >= r.d();
    float cx = r.cx();
    float cz = r.cz();
    if (avoidSide >= 0.0f) {
        const int side = static_cast<int>(avoidSide);
        cx += side == kWest ? 0.35f : side == kEast ? -0.35f : 0.0f;
        cz += side == kNorth ? 0.35f : side == kSouth ? -0.35f : 0.0f;
    }
    const int longA = alongX ? kNorth : kWest;
    const int longB = alongX ? kSouth : kEast;
    const int endA = alongX ? kWest : kNorth;
    const int endB = alongX ? kEast : kSouth;
    const std::vector<std::vector<int>> layouts = {
        {longA, longB, endA, endB}, {longA, longB},       {longA, endA, endB}, {longB, endA, endB},
        {longA, endA},              {longA, endB},        {longB, endA},       {longB, endB},
        {longA},                    {longB},              {endA, endB}};
    const float sizes[][2] = {{seats > 4 ? 1.6f : 1.2f, 0.8f}, {1.0f, 0.7f}};
    for (const auto& size : sizes) {
        const float tw = size[0];
        const float td = size[1];
        const float w = alongX ? tw : td;
        const float d = alongX ? td : tw;
        for (const std::vector<int>& sides : layouts) {
            const auto has = [&](int side) { return std::find(sides.begin(), sides.end(), side) != sides.end(); };
            std::optional<Rect> best;
            float bestDistance = 1e9f;
            for (float x = r.x0 + w * 0.5f; x <= r.x1 - w * 0.5f + 1e-3f; x += 0.15f) {
                for (float z = r.z0 + d * 0.5f; z <= r.z1 - d * 0.5f + 1e-3f; z += 0.15f) {
                    const Rect top{x - w * 0.5f, z - d * 0.5f, x + w * 0.5f, z + d * 0.5f};
                    const Rect around{top.x0 - (has(kWest) ? reach : 0.0f), top.z0 - (has(kNorth) ? reach : 0.0f),
                                      top.x1 + (has(kEast) ? reach : 0.0f), top.z1 + (has(kSouth) ? reach : 0.0f)};
                    const float distance = std::hypot(x - cx, z - cz);
                    if (distance < bestDistance && free(room, top) && free(room, around, false)) {
                        bestDistance = distance;
                        best = top;
                    }
                }
            }
            if (!best) {
                continue;
            }
            const Rect top = *best;
            addFurniture(FurnitureKind::Table, room, top, 0.0f, 0.76f, rgb(146, 106, 70));
            take(room, {top.x0 - (has(kWest) ? reach : 0.0f), top.z0 - (has(kNorth) ? reach : 0.0f),
                        top.x1 + (has(kEast) ? reach : 0.0f), top.z1 + (has(kSouth) ? reach : 0.0f)});
            // Стулья: по длинным сторонам (напротив друг друга), потом в торцах.
            struct Place {
                float x;
                float z;
            };
            std::vector<Place> places;
            const int perSide = tw > 1.4f ? 3 : tw > 1.1f ? 2 : 1;
            const auto at = [&](int side, float t) -> Place {
                switch (side) {
                case kNorth:
                    return {top.cx() + t * top.w(), top.z0 - 0.3f};
                case kSouth:
                    return {top.cx() + t * top.w(), top.z1 + 0.3f};
                case kWest:
                    return {top.x0 - 0.3f, top.cz() + t * top.d()};
                default:
                    return {top.x1 + 0.3f, top.cz() + t * top.d()};
                }
            };
            for (int i = 0; i < perSide; ++i) {
                const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(perSide) - 0.5f;
                for (const int side : {longA, longB}) {
                    if (has(side)) {
                        places.push_back(at(side, t));
                    }
                }
            }
            for (const int side : {endA, endB}) {
                if (has(side)) {
                    places.push_back(at(side, 0.0f));
                }
            }
            const int count = std::min(std::max(2, seats), static_cast<int>(places.size()));
            for (int i = 0; i < count; ++i) {
                const Place p = places[static_cast<std::size_t>(i)];
                const float heading = std::atan2(top.cx() - p.x, top.cz() - p.z);
                // Стул смотрит туда же, куда сидящий; округление — до 90°.
                const float facing = std::round(heading / (kPi * 0.5f)) * (kPi * 0.5f);
                Furniture f;
                f.kind = FurnitureKind::Chair;
                f.x = p.x;
                f.z = p.z;
                f.width = chair;
                f.depth = chair;
                f.height = 0.95f;
                f.facing = facing;
                f.color = rgb(120, 84, 54);
                f.room = room;
                f.seed = bits();
                out_.furniture.push_back(f);
                spot(SpotKind::Seat, p.x, p.z, facing, kSeat, room);
            }
            return;
        }
    }
}

void Planner::furnishKitchen(int room, int residents) {
    const std::array<int, 4> sides = sidesByLength(room);
    // Рабочая стена: стол-шкафы, плита, мойка. Окно над ними — к месту.
    int workSide = -1;
    for (const int side : sides) {
        const Rect r = roomRect(room);
        const float length = std::min(r.to(side) - r.from(side) - 0.4f, 3.6f);
        if (length < 1.8f) {
            continue;
        }
        const float prefer = unit() < 0.5f ? 0.0f : 1.0f;
        if (const std::optional<Rect> run = against(room, side, length, 0.6f, false, prefer)) {
            workSide = side;
            const int pieces = static_cast<int>(length / 0.6f);
            const int stoveAt = static_cast<int>(bits() % static_cast<std::uint32_t>(std::max(1, pieces)));
            const int sinkAt = (stoveAt + 2) % std::max(1, pieces);
            const bool alongX = alongXSide(side);
            const float a0 = alongX ? run->x0 : run->z0;
            const float piece = length / static_cast<float>(pieces);
            const float facing = facingFrom(side);
            for (int i = 0; i < pieces; ++i) {
                const float s = a0 + piece * static_cast<float>(i);
                const Rect cell = alongX ? Rect{s, run->z0, s + piece, run->z1} : Rect{run->x0, s, run->x1, s + piece};
                FurnitureKind kind = FurnitureKind::Counter;
                Rgb color = rgb(224, 220, 206);
                if (i == stoveAt) {
                    kind = FurnitureKind::Stove;
                    color = rgb(236, 236, 232);
                } else if (i == sinkAt && pieces > 2) {
                    kind = FurnitureKind::Sink;
                }
                addFurniture(kind, room, cell, facing, 0.9f, color);
                if (kind == FurnitureKind::Stove) {
                    const float fx = std::sin(facing);
                    const float fz = std::cos(facing);
                    spot(SpotKind::Stove, cell.cx() + fx * 0.75f, cell.cz() + fz * 0.75f, facing + kPi, 0.0f, room);
                }
            }
            // Перед рабочей стеной — проход.
            Rect aisle = *run;
            switch (side) {
            case kWest:
                aisle = {run->x1, run->z0, run->x1 + 0.7f, run->z1};
                break;
            case kEast:
                aisle = {run->x0 - 0.7f, run->z0, run->x0, run->z1};
                break;
            case kNorth:
                aisle = {run->x0, run->z1, run->x1, run->z1 + 0.7f};
                break;
            default:
                aisle = {run->x0, run->z0 - 0.7f, run->x1, run->z0};
                break;
            }
            data_[static_cast<std::size_t>(room)].soft.push_back(aisle);
            break;
        }
    }
    // Холодильник — высокий: не у окна.
    for (const int side : sides) {
        if (const std::optional<Rect> fridge = against(room, side, 0.62f, 0.65f, true, unit())) {
            addFurniture(FurnitureKind::Fridge, room, *fridge, facingFrom(side), 1.8f, rgb(236, 238, 236));
            break;
        }
    }
    table(room, std::max(2, residents), workSide >= 0 ? static_cast<float>(workSide) : -1.0f);
    lamp(room);
}

void Planner::furnishLiving(int room, int entry) {
    const Rect r = roomRect(room);
    std::array<int, 4> sides = sidesByLength(room);
    // Диван — у длинной стены подальше от входа.
    std::stable_sort(sides.begin(), sides.end(), [&](int a, int b) { return (a == entry) < (b == entry); });
    int sofaSide = -1;
    Rect sofa;
    for (const int side : sides) {
        if (const std::optional<Rect> place = against(room, side, 2.0f, 0.9f, false, 0.5f)) {
            sofaSide = side;
            sofa = *place;
            const float facing = facingFrom(side);
            addFurniture(FurnitureKind::Sofa, room, sofa, facing, 0.85f,
                         unit() < 0.5f ? rgb(118, 70, 58) : rgb(76, 92, 112));
            const float fx = std::sin(facing);
            const float fz = std::cos(facing);
            const float ax = alongXSide(side) ? 1.0f : 0.0f;
            const float az = alongXSide(side) ? 0.0f : 1.0f;
            for (const float t : {-0.5f, 0.5f}) {
                spot(SpotKind::Sofa, sofa.cx() + ax * t + fx * 0.12f, sofa.cz() + az * t + fz * 0.12f, facing,
                     kSofaSeat, room);
            }
            // Столик перед диваном и ковёр.
            const float cx = sofa.cx() + fx * 1.05f;
            const float cz = sofa.cz() + fz * 1.05f;
            const float hw = alongXSide(side) ? 0.5f : 0.28f;
            const float hd = alongXSide(side) ? 0.28f : 0.5f;
            const Rect coffee{cx - hw, cz - hd, cx + hw, cz + hd};
            if (free(room, coffee)) {
                addFurniture(FurnitureKind::Table, room, coffee, facing, 0.45f, rgb(110, 78, 52));
            }
            const float rw = alongXSide(side) ? 1.1f : 0.8f;
            const float rd = alongXSide(side) ? 0.8f : 1.1f;
            addFurniture(FurnitureKind::Rug, room, {cx - rw, cz - rd, cx + rw, cz + rd}, facing, 0.012f,
                         unit() < 0.5f ? rgb(150, 54, 50) : rgb(80, 96, 70));
            break;
        }
    }
    if (sofaSide >= 0) {
        // Телевизор напротив дивана.
        if (const std::optional<Rect> tv = against(room, opposite(sofaSide), 1.4f, 0.45f, false, 0.5f)) {
            addFurniture(FurnitureKind::TvStand, room, *tv, facingFrom(opposite(sofaSide)), 0.5f, rgb(92, 66, 46));
        }
        // Кресло — сбоку.
        for (const int side : {sofaSide ^ 2, (sofaSide ^ 2) ^ 1}) {
            const int s = side & 3;
            if (s == sofaSide || s == opposite(sofaSide)) {
                continue;
            }
            const float prefer = sofaSide == kWest || sofaSide == kNorth ? 0.0f : 1.0f;
            if (const std::optional<Rect> chair = against(room, s, 0.9f, 0.9f, false, prefer)) {
                const float facing = facingFrom(s);
                addFurniture(FurnitureKind::Armchair, room, *chair, facing, 0.85f, rgb(132, 96, 66));
                spot(SpotKind::Sofa, chair->cx() + std::sin(facing) * 0.12f, chair->cz() + std::cos(facing) * 0.12f,
                     facing, kSofaSeat, room);
                break;
            }
        }
    }
    // Книжный шкаф.
    for (const int side : sides) {
        if (const std::optional<Rect> shelf = against(room, side, 1.0f, 0.35f, true, unit())) {
            addFurniture(FurnitureKind::Shelf, room, *shelf, facingFrom(side), 1.9f, rgb(104, 74, 50));
            break;
        }
    }
    // Двое могут стоять посреди комнаты лицом друг к другу.
    const bool alongX = r.w() >= r.d();
    for (const float t : {-0.45f, 0.45f}) {
        const float x = r.cx() + (alongX ? t : 0.0f);
        const float z = r.cz() + (alongX ? 0.0f : t);
        const float heading = std::atan2(r.cx() - x, r.cz() - z);
        spot(SpotKind::Stand, x, z, heading, 0.0f, room);
    }
    lamp(room);
}

bool Planner::bed(int room, bool wide, bool alongWall) {
    const float width = wide ? 1.6f : 0.9f;
    constexpr float length = 2.05f;
    const std::array<int, 4> sides = sidesByLength(room);
    for (const int side : sides) {
        // Изголовьем к стене — или боком вдоль неё (в тесной комнате).
        const std::optional<Rect> place =
            alongWall ? against(room, side, length, width, false, unit()) : against(room, side, width, length, false, 0.5f);
        if (!place) {
            continue;
        }
        float facing = facingFrom(side);
        if (alongWall) {
            // Ноги — вдоль стены, к дальнему от угла концу.
            const bool alongX = alongXSide(side);
            const Rect r = roomRect(room);
            const bool towardHigh = alongX ? place->cx() < r.cx() : place->cz() < r.cz();
            facing = alongX ? (towardHigh ? kPi * 0.5f : -kPi * 0.5f) : (towardHigh ? 0.0f : kPi);
        }
        addFurniture(FurnitureKind::Bed, room, *place, facing, 1.0f, unit() < 0.5f ? rgb(170, 60, 56) : rgb(70, 96, 140));
        // Таз — посередине кровати, голова — к изголовью.
        const float fx = std::sin(facing);
        const float fz = std::cos(facing);
        const int sleepers = wide ? 2 : 1;
        for (int k = 0; k < sleepers; ++k) {
            const float across = sleepers == 2 ? (k == 0 ? -0.38f : 0.38f) : 0.0f;
            spot(SpotKind::Bed, place->cx() + fz * across, place->cz() - fx * across, facing, kMattress, room);
        }
        if (!alongWall) {
            // Тумбочка у изголовья, сбоку.
            const float hx = place->cx() - fx * (length * 0.5f - 0.21f);
            const float hz = place->cz() - fz * (length * 0.5f - 0.21f);
            for (const float s : {1.0f, -1.0f}) {
                const float off = (width * 0.5f + 0.3f) * s;
                const float nx = hx + fz * off;
                const float nz = hz - fx * off;
                const Rect stand{nx - 0.22f, nz - 0.21f, nx + 0.22f, nz + 0.21f};
                if (free(room, stand)) {
                    addFurniture(FurnitureKind::Nightstand, room, stand, facing, 0.52f, rgb(112, 80, 54));
                    break;
                }
            }
        }
        return true;
    }
    return false;
}

int Planner::furnishBedroom(int room, int beds) {
    // Двуспальная — двоим, иначе односпальные; в тесноте — боком к стене.
    const bool twin = beds >= 2 && unit() < 0.35f;
    int placed = 0;
    while (placed < beds) {
        if (beds - placed >= 2 && !twin && bed(room, true, false)) {
            placed += 2;
            continue;
        }
        if (bed(room, false, false) || bed(room, false, true)) {
            ++placed;
            continue;
        }
        break;
    }
    const std::array<int, 4> sides = sidesByLength(room);
    for (const int side : sides) {
        if (const std::optional<Rect> wardrobe = against(room, side, 1.2f, 0.6f, true, unit())) {
            addFurniture(FurnitureKind::Wardrobe, room, *wardrobe, facingFrom(side), 2.0f, rgb(116, 84, 58));
            break;
        }
    }
    lamp(room);
    return placed;
}

void Planner::furnishBath(int room) {
    const std::array<int, 4> sides = sidesByLength(room);
    for (const int side : sides) {
        if (const std::optional<Rect> tub = against(room, side, 1.7f, 0.75f, false, 0.0f)) {
            addFurniture(FurnitureKind::Tub, room, *tub, facingFrom(side), 0.55f, rgb(240, 242, 244));
            break;
        }
    }
    for (const int side : sides) {
        if (const std::optional<Rect> sink = against(room, side, 0.55f, 0.45f, false, 1.0f)) {
            addFurniture(FurnitureKind::Sink, room, *sink, facingFrom(side), 0.85f, rgb(240, 242, 244));
            break;
        }
    }
    lamp(room);
}

void Planner::furnishStairs(int room) {
    const Rect r = roomRect(room);
    // Марш вдоль длинной стороны клетки у дальней стены.
    const bool alongX = r.w() >= r.d();
    const int side = alongX ? (doorSide_ == kNorth ? kSouth : kNorth) : (doorSide_ == kWest ? kEast : kWest);
    if (const std::optional<Rect> steps = against(room, side, std::min(3.2f, (alongX ? r.w() : r.d()) - 0.4f), 1.2f,
                                                  false, 0.5f)) {
        const float facing = alongX ? kPi * 0.5f : 0.0f; // поднимается по длинной оси
        Furniture f;
        f.kind = FurnitureKind::Steps;
        f.x = steps->cx();
        f.z = steps->cz();
        // У марша ширина поперёк подъёма, глубина — вдоль.
        f.width = alongX ? steps->d() : steps->w();
        f.depth = alongX ? steps->w() : steps->d();
        f.height = out_.ceiling - out_.floor;
        f.facing = facing;
        f.color = rgb(150, 148, 142);
        f.room = room;
        out_.furniture.push_back(f);
        take(room, *steps);
    }
    spot(SpotKind::Stand, r.cx(), r.cz(), 0.0f, 0.0f, room);
    lamp(room);
}

void Planner::furnishHall(int room) {
    const std::array<int, 4> sides = sidesByLength(room);
    for (const int side : sides) {
        if (const std::optional<Rect> desk = against(room, side, 2.2f, 0.7f, false, 0.5f)) {
            const float facing = facingFrom(side);
            addFurniture(FurnitureKind::Counter, room, *desk, facing, 1.05f, rgb(160, 150, 136));
            spot(SpotKind::Stove, desk->cx() + std::sin(facing) * 0.75f, desk->cz() + std::cos(facing) * 0.75f,
                 facing + kPi, 0.0f, room);
            break;
        }
    }
    const Rect r = roomRect(room);
    spot(SpotKind::Stand, r.cx(), r.cz(), 0.0f, 0.0f, room);
    lamp(room);
}

void Planner::furnishWard(int room) {
    const std::array<int, 4> sides = sidesByLength(room);
    int beds = 0;
    for (const int side : sides) {
        for (int i = 0; i < 3 && beds < 4; ++i) {
            const std::optional<Rect> bed = against(room, side, 0.9f, 2.0f, false, static_cast<float>(i) * 0.5f);
            if (!bed) {
                break;
            }
            const float facing = facingFrom(side);
            addFurniture(FurnitureKind::Bed, room, *bed, facing, 1.0f, rgb(220, 226, 230));
            spot(SpotKind::Bed, bed->cx(), bed->cz(), facing, kMattress, room);
            // Проход между кроватями.
            take(room, {bed->x0 - 0.3f, bed->z0 - 0.3f, bed->x1 + 0.3f, bed->z1 + 0.3f});
            ++beds;
        }
    }
    lamp(room);
}

void Planner::furnishClassroom(int room) {
    const Rect r = roomRect(room);
    // Доска у одной из длинных стен, парты рядами лицом к ней.
    const bool alongX = r.w() >= r.d();
    const int boardSide = alongX ? kNorth : kWest;
    if (const std::optional<Rect> board = against(room, boardSide, std::min(3.0f, (alongX ? r.w() : r.d()) - 1.0f),
                                                  0.06f, false, 0.5f)) {
        addFurniture(FurnitureKind::Board, room, *board, facingFrom(boardSide), 1.2f, rgb(40, 70, 52), 0.85f);
    }
    const float facing = facingFrom(boardSide) + kPi; // ученики смотрят на доску
    const float rowsSpan = (alongX ? r.d() : r.w()) - 2.4f;
    const float colsSpan = (alongX ? r.w() : r.d()) - 1.2f;
    const int rows = std::clamp(static_cast<int>(rowsSpan / 1.3f), 1, 4);
    const int cols = std::clamp(static_cast<int>(colsSpan / 1.6f), 1, 4);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            const float u = (alongX ? r.x0 : r.z0) + 0.6f + colsSpan * (static_cast<float>(j) + 0.5f) / static_cast<float>(cols);
            const float v = (alongX ? r.z0 : r.x0) + 1.8f + rowsSpan * (static_cast<float>(i) + 0.5f) / static_cast<float>(rows);
            const float x = alongX ? u : v;
            const float z = alongX ? v : u;
            const float hw = alongX ? 0.6f : 0.25f;
            const float hd = alongX ? 0.25f : 0.6f;
            const Rect desk{x - hw, z - hd, x + hw, z + hd};
            if (!free(room, desk)) {
                continue;
            }
            addFurniture(FurnitureKind::Desk, room, desk, facingFrom(boardSide), 0.72f, rgb(170, 140, 100));
            const float back = 0.45f;
            const float sx = x - std::sin(facingFrom(boardSide)) * back;
            const float sz = z - std::cos(facingFrom(boardSide)) * back;
            spot(SpotKind::Desk, sx, sz, facingFrom(boardSide) + kPi, kSeat, room);
        }
    }
    (void)facing;
    lamp(room);
}

void Planner::furnishOffice(int room) {
    const std::array<int, 4> sides = sidesByLength(room);
    int desks = 0;
    for (const int side : sides) {
        if (desks >= 3) {
            break;
        }
        if (const std::optional<Rect> desk = against(room, side, 1.3f, 0.7f, false, unit())) {
            const float facing = facingFrom(side);
            addFurniture(FurnitureKind::Desk, room, *desk, facing, 0.75f, rgb(120, 96, 70));
            spot(SpotKind::Desk, desk->cx() + std::sin(facing) * 0.75f, desk->cz() + std::cos(facing) * 0.75f,
                 facing + kPi, kSeat, room);
            ++desks;
        }
    }
    for (const int side : sides) {
        if (const std::optional<Rect> shelf = against(room, side, 1.0f, 0.4f, true, unit())) {
            addFurniture(FurnitureKind::Shelf, room, *shelf, facingFrom(side), 1.9f, rgb(90, 90, 96));
            break;
        }
    }
    lamp(room);
}

void Planner::furnishSales(int room) {
    const Rect r = roomRect(room);
    // Прилавок у боковой стены ближе ко входу; продавец — между ним и стеной.
    const int side = alongXSide(doorSide_) ? (unit() < 0.5f ? kWest : kEast) : (unit() < 0.5f ? kNorth : kSouth);
    const float prefer = doorSide_ == kWest || doorSide_ == kNorth ? 0.15f : 0.85f;
    if (const std::optional<Rect> zone = against(room, side, 2.6f, 1.7f, false, prefer)) {
        const float facing = facingFrom(side);
        const float fx = std::sin(facing);
        const float fz = std::cos(facing);
        // Прилавок — в передних 0,6 м зоны.
        Rect counter = *zone;
        switch (side) {
        case kWest:
            counter.x0 = zone->x1 - 0.6f;
            break;
        case kEast:
            counter.x1 = zone->x0 + 0.6f;
            break;
        case kNorth:
            counter.z0 = zone->z1 - 0.6f;
            break;
        default:
            counter.z1 = zone->z0 + 0.6f;
            break;
        }
        take(room, *zone);
        addFurniture(FurnitureKind::Counter, room, counter, facing, 1.0f, rgb(150, 112, 76));
        spot(SpotKind::Stove, counter.cx() - fx * 0.75f, counter.cz() - fz * 0.75f, facing, 0.0f, room);
        // Полки за спиной продавца.
        const Rect back = side == kWest    ? Rect{zone->x0, zone->z0, zone->x0 + 0.4f, zone->z1}
                          : side == kEast  ? Rect{zone->x1 - 0.4f, zone->z0, zone->x1, zone->z1}
                          : side == kNorth ? Rect{zone->x0, zone->z0, zone->x1, zone->z0 + 0.4f}
                                           : Rect{zone->x0, zone->z1 - 0.4f, zone->x1, zone->z1};
        addFurniture(FurnitureKind::Shelf, room, back, facing, 1.8f, rgb(120, 100, 80));
    }
    // Ряды стеллажей посреди зала, параллельно входной стене.
    const bool rowsAlongX = alongXSide(doorSide_);
    const float span = rowsAlongX ? r.w() : r.d();
    const float deep = rowsAlongX ? r.d() : r.w();
    const int rows = std::clamp(static_cast<int>((deep - 2.6f) / 1.8f), 1, 4);
    for (int i = 0; i < rows; ++i) {
        const float v = 1.9f + (deep - 3.2f) * (static_cast<float>(i) + 0.5f) / static_cast<float>(rows);
        const float length = std::min(3.2f, span - 4.0f);
        if (length < 1.2f) {
            break;
        }
        const float u0 = (rowsAlongX ? r.x0 : r.z0) + (span - length) * 0.5f;
        const float vv = doorSide_ == kNorth || doorSide_ == kWest ? (rowsAlongX ? r.z0 : r.x0) + v
                                                                  : (rowsAlongX ? r.z1 : r.x1) - v;
        const Rect shelf = rowsAlongX ? Rect{u0, vv - 0.25f, u0 + length, vv + 0.25f}
                                      : Rect{vv - 0.25f, u0, vv + 0.25f, u0 + length};
        if (free(room, shelf)) {
            addFurniture(FurnitureKind::Shelf, room, shelf, facingFrom(doorSide_) + kPi, 1.7f, rgb(170, 160, 150));
        }
    }
    spot(SpotKind::Stand, r.cx(), r.cz(), facingFrom(doorSide_), 0.0f, room);
    lamp(room);
}

void Planner::furnishStoreroom(int room) {
    const std::array<int, 4> sides = sidesByLength(room);
    for (const int side : sides) {
        for (int i = 0; i < 3; ++i) {
            const bool shelf = (bits() & 1u) != 0u;
            if (const std::optional<Rect> place =
                    against(room, side, shelf ? 1.4f : 0.7f, shelf ? 0.5f : 0.7f, shelf, unit())) {
                addFurniture(shelf ? FurnitureKind::Shelf : FurnitureKind::Crate, room, *place, facingFrom(side),
                             shelf ? 1.9f : 0.7f, shelf ? rgb(110, 104, 96) : rgb(140, 112, 72));
            }
        }
    }
    lamp(room);
}

void Planner::furnishCells(int room) {
    const std::array<int, 4> sides = sidesByLength(room);
    for (const int side : sides) {
        if (const std::optional<Rect> bench = against(room, side, 1.9f, 0.7f, false, unit())) {
            addFurniture(FurnitureKind::Bed, room, *bench, facingFrom(side) + kPi * 0.5f, 0.45f, rgb(120, 120, 124));
            break;
        }
    }
    // Решётка вдоль перегородки с дверью.
    const Rect r = roomRect(room);
    for (int side = 0; side < 4; ++side) {
        if (data_[static_cast<std::size_t>(room)].doors[static_cast<std::size_t>(side)] == 0) {
            continue;
        }
        const float a0 = r.from(side) + 0.1f;
        const float a1 = r.to(side) - 0.1f;
        const float at = r.at(side);
        const float in = side == kWest || side == kNorth ? 0.08f : -0.08f;
        const Rect bars = alongXSide(side) ? Rect{a0, std::min(at, at + in), a1, std::max(at, at + in)}
                                           : Rect{std::min(at, at + in), a0, std::max(at, at + in), a1};
        Furniture f;
        f.kind = FurnitureKind::Bars;
        f.x = bars.cx();
        f.z = bars.cz();
        f.width = a1 - a0;
        f.depth = 0.08f;
        f.height = 2.3f;
        f.facing = facingFrom(side);
        f.color = rgb(70, 72, 78);
        f.room = room;
        out_.furniture.push_back(f);
        break;
    }
    lamp(room);
}

void Planner::furnishBarn(int room) {
    const Rect r = roomRect(room);
    const std::array<int, 4> sides = sidesByLength(room);
    // Стойла вдоль дальней длинной стены.
    const int stallSide = sides[0];
    const float a0 = r.from(stallSide);
    const float a1 = r.to(stallSide);
    for (float a = a0 + 2.4f; a < a1 - 1.0f; a += 2.6f) {
        const float at = r.at(stallSide);
        const float in = stallSide == kWest || stallSide == kNorth ? 2.4f : -2.4f;
        const Rect stall = alongXSide(stallSide)
                               ? Rect{a - 0.05f, std::min(at, at + in), a + 0.05f, std::max(at, at + in)}
                               : Rect{std::min(at, at + in), a - 0.05f, std::max(at, at + in), a + 0.05f};
        addFurniture(FurnitureKind::Stall, room, stall, facingFrom(stallSide), 1.3f, rgb(120, 92, 62));
    }
    // Тюки сена штабелями.
    for (std::size_t k = 1; k < sides.size(); ++k) {
        for (int i = 0; i < 2; ++i) {
            if (const std::optional<Rect> bales = against(room, sides[k], 2.4f, 1.2f, false, unit())) {
                const float height = (bits() & 1u) != 0u ? 1.2f : 0.6f;
                addFurniture(FurnitureKind::Bales, room, *bales, facingFrom(sides[k]), height, rgb(196, 168, 96));
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        const float x = range(r.x0 + 1.5f, r.x1 - 1.5f);
        const float z = range(r.z0 + 1.5f, r.z1 - 1.5f);
        const Rect crate{x - 0.35f, z - 0.35f, x + 0.35f, z + 0.35f};
        if (free(room, crate)) {
            addFurniture(FurnitureKind::Crate, room, crate, 0.0f, 0.7f, rgb(140, 112, 72));
        }
    }
    spot(SpotKind::Stand, r.cx(), r.cz(), facingFrom(doorSide_), 0.0f, room);
    lamp(room);
}

// --- Планировки -------------------------------------------------------------------

std::vector<int> Planner::planHome(const Rect& rect, int entry, int flat) {
    // Оси дома от входа: глубина — от входной стены внутрь, ширина — вдоль неё.
    const bool depthX = entry == kWest || entry == kEast;
    const float depth = depthX ? rect.w() : rect.d();
    const float width = depthX ? rect.d() : rect.w();
    const float half = kInnerWall * 0.5f;
    // Прямоугольник по глубине [d0, d1] от входа и ширине [w0, w1].
    const auto part = [&](float d0, float d1, float w0, float w1) {
        Rect r;
        if (depthX) {
            r.z0 = rect.z0 + w0;
            r.z1 = rect.z0 + w1;
            r.x0 = entry == kWest ? rect.x0 + d0 : rect.x1 - d1;
            r.x1 = entry == kWest ? rect.x0 + d1 : rect.x1 - d0;
        } else {
            r.x0 = rect.x0 + w0;
            r.x1 = rect.x0 + w1;
            r.z0 = entry == kNorth ? rect.z0 + d0 : rect.z1 - d1;
            r.z1 = entry == kNorth ? rect.z0 + d1 : rect.z1 - d0;
        }
        return r;
    };
    // Мировая координата по глубине.
    const auto depthWorld = [&](float d) {
        if (depthX) {
            return entry == kWest ? rect.x0 + d : rect.x1 - d;
        }
        return entry == kNorth ? rect.z0 + d : rect.z1 - d;
    };
    const float widthBase = depthX ? rect.z0 : rect.x0;

    // Глубина: спереди — кухня и гостиная, сзади — спальни.
    const bool split = depth >= 6.2f;
    float front = depth;
    if (split) {
        front = std::clamp(depth * range(0.46f, 0.56f), 3.1f, depth - 3.0f);
        const float a = depthWorld(3.0f);
        const float b = depthWorld(depth - 2.9f);
        const float at = safeSplit(depthX, depthWorld(front), std::min(a, b), std::max(a, b));
        front = std::abs(at - depthWorld(0.0f));
    }
    const float frontEnd = split ? front - half : front;

    // Передняя часть: кухня сбоку, гостиная рядом.
    std::vector<int> frontRooms;
    if (width >= 5.6f) {
        const bool kitchenFirst = unit() < 0.5f;
        const float kitchenWidth = std::clamp(width * range(0.38f, 0.48f), 2.6f, width - 3.0f);
        float cut = kitchenFirst ? kitchenWidth : width - kitchenWidth;
        cut = safeSplit(!depthX, widthBase + cut, widthBase + 2.5f, widthBase + width - 2.5f) - widthBase;
        const int first = addRoom(kitchenFirst ? RoomKind::Kitchen : RoomKind::Living,
                                  part(0.0f, frontEnd, 0.0f, cut - half), flat);
        const int second = addRoom(kitchenFirst ? RoomKind::Living : RoomKind::Kitchen,
                                   part(0.0f, frontEnd, cut + half, width), flat);
        partition(first, second, true, unit() < 0.5f ? 0.8f : 0.2f);
        frontRooms = {first, second};
    } else {
        frontRooms = {addRoom(RoomKind::Living, part(0.0f, frontEnd, 0.0f, width), flat)};
    }
    if (!split) {
        return frontRooms;
    }

    // Задняя часть: спальни (и ванная, если широко).
    std::vector<std::pair<RoomKind, float>> plan; // вид и доля ширины
    if (width >= 12.0f) {
        plan = {{RoomKind::Bedroom, 0.4f}, {RoomKind::Bath, 0.2f}, {RoomKind::Bedroom, 0.4f}};
    } else if (width >= 9.2f) {
        plan = {{RoomKind::Bedroom, 0.5f}, {RoomKind::Bedroom, 0.5f}};
    } else if (width >= 7.0f) {
        plan = {{RoomKind::Bedroom, 0.66f}, {RoomKind::Bath, 0.34f}};
    } else {
        plan = {{RoomKind::Bedroom, 1.0f}};
    }
    if (unit() < 0.5f) {
        std::reverse(plan.begin(), plan.end());
    }
    std::vector<int> backRooms;
    float w = 0.0f;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        float end = width;
        if (i + 1 < plan.size()) {
            end = safeSplit(!depthX, widthBase + w + width * plan[i].second, widthBase + w + 1.9f,
                            widthBase + width - 1.9f) -
                  widthBase;
        }
        const float w0 = i == 0 ? 0.0f : w + half;
        const float w1 = i + 1 == plan.size() ? width : end - half;
        backRooms.push_back(addRoom(plan[i].first, part(front + half, depth, w0, w1), flat));
        w = end;
    }
    // Между задними комнатами — глухо; каждая открывается в переднюю, с
    // которой у неё больше общего.
    for (std::size_t i = 0; i + 1 < backRooms.size(); ++i) {
        partition(backRooms[i], backRooms[i + 1], false);
    }
    for (const int back : backRooms) {
        int best = frontRooms.front();
        float bestShare = -100.0f;
        for (const int f : frontRooms) {
            const Rect fb = roomRect(f);
            const Rect bb = roomRect(back);
            const float share = depthX ? std::min(fb.z1, bb.z1) - std::max(fb.z0, bb.z0)
                                       : std::min(fb.x1, bb.x1) - std::max(fb.x0, bb.x0);
            if (share < kDoorWidth + 0.4f) {
                continue;
            }
            // Ванная и спальня — лучше не из кухни.
            const bool kitchen = out_.rooms[static_cast<std::size_t>(f)].kind == RoomKind::Kitchen;
            const float weight = share - (kitchen ? 1.5f : 0.0f);
            if (weight > bestShare) {
                bestShare = weight;
                best = f;
            }
        }
        const float bias = unit() < 0.5f ? 0.15f : 0.85f;
        for (const int f : frontRooms) {
            partition(f, back, f == best, bias);
        }
    }
    return frontRooms;
}

void Planner::planApartmentLevel() {
    const Rect& I = inner_;
    const bool longX = I.w() >= I.d();
    const float a0 = longX ? I.x0 : I.z0;
    const float a1 = longX ? I.x1 : I.z1;
    const float half = kInnerWall * 0.5f;
    // Лестница — во всю глубину напротив входа.
    const float along = std::clamp(doorAlong_, a0 + kStairsWidth * 0.5f + 0.1f, a1 - kStairsWidth * 0.5f - 0.1f);
    float s0 = safeSplit(longX, along - kStairsWidth * 0.5f, a0 + 0.1f, along - 1.2f);
    float s1 = safeSplit(longX, along + kStairsWidth * 0.5f, along + 1.2f, a1 - 0.1f);
    const float left = s0 - half - a0;
    const float right = a1 - (s1 + half);
    const int total = out_.flats;
    int nLeft = static_cast<int>(std::lround(static_cast<float>(total) * left / std::max(0.1f, left + right)));
    nLeft = std::clamp(nLeft, left >= 4.0f ? 1 : 0, right >= 4.0f ? total - 1 : total);
    if (left < 4.0f) {
        nLeft = 0;
    } else if (right < 4.0f) {
        nLeft = total;
    }
    const int nRight = total - nLeft;
    const float stairs0 = nLeft == 0 ? a0 : s0 + half;
    const float stairs1 = nRight == 0 ? a1 : s1 - half;
    const auto strip = [&](float u0, float u1) {
        return longX ? Rect{u0, I.z0, u1, I.z1} : Rect{I.x0, u0, I.x1, u1};
    };
    const int stairs = addRoom(RoomKind::Stairs, strip(stairs0, stairs1), -1);
    flatEntry_.assign(static_cast<std::size_t>(total), kSouth);
    int flat = 0;
    for (int part = 0; part < 2; ++part) {
        const int count = part == 0 ? nLeft : nRight;
        if (count == 0) {
            continue;
        }
        const float u0 = part == 0 ? a0 : s1 + half;
        const float u1 = part == 0 ? s0 - half : a1;
        const int entry = part == 0 ? (longX ? kEast : kSouth) : (longX ? kWest : kNorth);
        float u = u0;
        std::vector<std::vector<int>> fronts;
        for (int i = 0; i < count; ++i) {
            float end = u1;
            if (i + 1 < count) {
                end = safeSplit(longX, u0 + (u1 - u0) * static_cast<float>(i + 1) / static_cast<float>(count),
                                u + 4.0f, u1 - 4.0f);
            }
            const float f0 = i == 0 ? u : u + half;
            const float f1 = i + 1 == count ? end : end - half;
            flatEntry_[static_cast<std::size_t>(flat)] = entry;
            fronts.push_back(planHome(strip(f0, f1), entry, flat));
            ++flat;
            u = end;
        }
        // У ближней к лестнице квартиры — дверь на лестницу, у остальных —
        // закрытая дверь на стене входа (за ней — соседи).
        for (std::size_t i = 0; i < fronts.size(); ++i) {
            const bool nearStairs = part == 0 ? i + 1 == fronts.size() : i == 0;
            bool door = false;
            for (const int room : fronts[i]) {
                const bool living = out_.rooms[static_cast<std::size_t>(room)].kind == RoomKind::Living;
                if (nearStairs) {
                    door = partition(stairs, room, living && !door) || door;
                } else if (living) {
                    panels_.emplace_back(room, entry);
                }
            }
        }
    }
    // Глухие стены между соседними квартирами.
    const std::size_t count = out_.rooms.size();
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            const InteriorRoom& a = out_.rooms[i];
            const InteriorRoom& b = out_.rooms[j];
            if (a.flat >= 0 && b.flat >= 0 && a.flat != b.flat) {
                partition(static_cast<int>(i), static_cast<int>(j), false);
            }
        }
    }
}

void Planner::planHallWings(RoomKind hall, RoomKind wing, float wingWidth, float hallWidth) {
    const Rect& I = inner_;
    const bool longX = I.w() >= I.d();
    const float a0 = longX ? I.x0 : I.z0;
    const float a1 = longX ? I.x1 : I.z1;
    const float half = kInnerWall * 0.5f;
    const float along = std::clamp(doorAlong_, a0 + hallWidth * 0.5f, a1 - hallWidth * 0.5f);
    float s0 = safeSplit(longX, along - hallWidth * 0.5f, a0, along - 1.5f);
    float s1 = safeSplit(longX, along + hallWidth * 0.5f, along + 1.5f, a1);
    if (s0 - a0 < 2.5f) {
        s0 = a0;
    }
    if (a1 - s1 < 2.5f) {
        s1 = a1;
    }
    const auto strip = [&](float u0, float u1) {
        return longX ? Rect{u0, I.z0, u1, I.z1} : Rect{I.x0, u0, I.x1, u1};
    };
    const int hallRoom = addRoom(hall, strip(s0 <= a0 ? a0 : s0 + half, s1 >= a1 ? a1 : s1 - half), -1);
    for (int part = 0; part < 2; ++part) {
        const float u0 = part == 0 ? a0 : s1 + half;
        const float u1 = part == 0 ? s0 - half : a1;
        if (u1 - u0 < 2.5f) {
            continue;
        }
        const int count = std::max(1, static_cast<int>(std::lround((u1 - u0) / wingWidth)));
        std::vector<float> cuts(static_cast<std::size_t>(count + 1));
        cuts.front() = u0;
        cuts.back() = u1;
        for (int i = 1; i < count; ++i) {
            const float at = u0 + (u1 - u0) * static_cast<float>(i) / static_cast<float>(count);
            cuts[static_cast<std::size_t>(i)] = safeSplit(longX, at, at - 1.0f, at + 1.0f);
        }
        // От холла наружу: каждая следующая комната открывается в предыдущую.
        int previous = hallRoom;
        for (int k = 0; k < count; ++k) {
            const int i = part == 0 ? count - 1 - k : k;
            const float r0 = cuts[static_cast<std::size_t>(i)] + (i > 0 ? half : 0.0f);
            const float r1 = cuts[static_cast<std::size_t>(i + 1)] - (i + 1 < count ? half : 0.0f);
            const int room = addRoom(wing, strip(r0, r1), -1);
            partition(previous, room, true, 0.5f);
            previous = room;
        }
    }
}

void Planner::planFrontBack(RoomKind frontKind, RoomKind backKind, float backDepth) {
    const Rect& I = inner_;
    const float half = kInnerWall * 0.5f;
    Rect front = I;
    Rect back = I;
    switch (doorSide_) {
    case kNorth:
        back.z0 = I.z1 - backDepth + half;
        front.z1 = I.z1 - backDepth - half;
        break;
    case kSouth:
        back.z1 = I.z0 + backDepth - half;
        front.z0 = I.z0 + backDepth + half;
        break;
    case kWest:
        back.x0 = I.x1 - backDepth + half;
        front.x1 = I.x1 - backDepth - half;
        break;
    default:
        back.x1 = I.x0 + backDepth - half;
        front.x0 = I.x0 + backDepth + half;
        break;
    }
    const int f = addRoom(frontKind, front, -1);
    const int b = addRoom(backKind, back, -1);
    partition(f, b, true, unit());
}

int Planner::residents(int flat) const {
    if (flat >= 0 && static_cast<std::size_t>(flat) < in_.residents.size()) {
        return std::max(1, in_.residents[static_cast<std::size_t>(flat)]);
    }
    return 2;
}

void Planner::furnishAll() {
    // Кроватей в квартире — не меньше жильцов: поровну по спальням.
    const std::size_t flats = static_cast<std::size_t>(std::max(1, out_.flats));
    std::vector<int> bedrooms(flats, 0);
    for (const InteriorRoom& room : out_.rooms) {
        if (room.kind == RoomKind::Bedroom && room.flat >= 0 && static_cast<std::size_t>(room.flat) < flats) {
            ++bedrooms[static_cast<std::size_t>(room.flat)];
        }
    }
    std::vector<int> bedsLeft(flats);
    for (std::size_t f = 0; f < flats; ++f) {
        bedsLeft[f] = residents(static_cast<int>(f));
    }
    for (std::size_t i = 0; i < out_.rooms.size(); ++i) {
        const int room = static_cast<int>(i);
        const InteriorRoom info = out_.rooms[i];
        const std::size_t flat = static_cast<std::size_t>(std::clamp(info.flat, 0, static_cast<int>(flats) - 1));
        switch (info.kind) {
        case RoomKind::Kitchen:
            furnishKitchen(room, residents(static_cast<int>(flat)));
            break;
        case RoomKind::Living:
            furnishLiving(room, flat < flatEntry_.size() ? flatEntry_[flat] : doorSide_);
            break;
        case RoomKind::Bedroom: {
            int& left = bedsLeft[flat];
            int& rooms = bedrooms[flat];
            const int beds = std::clamp((left + rooms - 1) / std::max(1, rooms), 1, 3);
            left = std::max(0, left - furnishBedroom(room, beds));
            rooms = std::max(1, rooms - 1);
            break;
        }
        case RoomKind::Bath:
            furnishBath(room);
            break;
        case RoomKind::Stairs:
            furnishStairs(room);
            break;
        case RoomKind::Hall:
            furnishHall(room);
            break;
        case RoomKind::Sales:
            furnishSales(room);
            break;
        case RoomKind::Storeroom:
            furnishStoreroom(room);
            break;
        case RoomKind::Ward:
            furnishWard(room);
            break;
        case RoomKind::Office:
            furnishOffice(room);
            break;
        case RoomKind::Classroom:
            furnishClassroom(room);
            break;
        case RoomKind::Cells:
            furnishCells(room);
            break;
        case RoomKind::Barn:
            furnishBarn(room);
            break;
        }
    }
    // Не поместились — ещё кровать в спальне, а нет — в гостиной (раскладушка).
    for (std::size_t flat = 0; flat < flats; ++flat) {
        for (const RoomKind kind : {RoomKind::Bedroom, RoomKind::Living, RoomKind::Kitchen}) {
            for (std::size_t i = 0; i < out_.rooms.size() && bedsLeft[flat] > 0; ++i) {
                if (out_.rooms[i].kind != kind || out_.rooms[i].flat != static_cast<int>(flat)) {
                    continue;
                }
                while (bedsLeft[flat] > 0 &&
                       (bed(static_cast<int>(i), false, false) || bed(static_cast<int>(i), false, true))) {
                    --bedsLeft[flat];
                }
            }
        }
    }
}

void Planner::run() {
    const int level = std::clamp(in_.level, 0, std::max(0, in_.storeys - 1));
    out_.kind = in_.kind;
    out_.level = level;
    out_.floor = in_.floor + static_cast<float>(level) * in_.storey;
    out_.ceiling = out_.floor + in_.storey - 0.25f;
    out_.outerX0 = in_.x0;
    out_.outerZ0 = in_.z0;
    out_.outerX1 = in_.x1;
    out_.outerZ1 = in_.z1;
    inner_ = {in_.x0 + kOuterWall, in_.z0 + kOuterWall, in_.x1 - kOuterWall, in_.z1 - kOuterWall};
    out_.x0 = inner_.x0;
    out_.z0 = inner_.z0;
    out_.x1 = inner_.x1;
    out_.z1 = inner_.z1;
    out_.flats = flatsPerLevel(in_.kind, in_.x0, in_.z0, in_.x1, in_.z1);

    // Вход: к какой стене ближе середина двери.
    const float dWest = std::abs(in_.doorX - in_.x0);
    const float dEast = std::abs(in_.doorX - in_.x1);
    const float dNorth = std::abs(in_.doorZ - in_.z0);
    const float dSouth = std::abs(in_.doorZ - in_.z1);
    const float best = std::min({dWest, dEast, dNorth, dSouth});
    doorSide_ = best == dSouth ? kSouth : best == dNorth ? kNorth : best == dWest ? kWest : kEast;
    doorAlong_ = alongXSide(doorSide_) ? in_.doorX : in_.doorZ;

    // Окна этого этажа — по сторонам здания.
    for (const WallOpening& opening : in_.openings) {
        if (opening.storey != level || opening.door) {
            continue;
        }
        int side = 0;
        if (opening.alongX) {
            side = std::abs(opening.plane - in_.z0) < std::abs(opening.plane - in_.z1) ? kNorth : kSouth;
        } else {
            side = std::abs(opening.plane - in_.x0) < std::abs(opening.plane - in_.x1) ? kWest : kEast;
        }
        buildingWindows_[static_cast<std::size_t>(side)].push_back(
            {opening.a0, opening.a1, opening.y0 - out_.floor, opening.y1 - out_.floor, false});
    }

    switch (in_.kind) {
    case BuildingKind::House:
        flatEntry_ = {doorSide_};
        planHome(inner_, doorSide_, 0);
        break;
    case BuildingKind::Apartment:
        planApartmentLevel();
        break;
    case BuildingKind::Shop:
        planFrontBack(RoomKind::Sales, RoomKind::Storeroom, 3.0f);
        break;
    case BuildingKind::PoliceStation:
        planFrontBack(RoomKind::Office, RoomKind::Cells, 2.8f);
        break;
    case BuildingKind::Hospital:
        planHallWings(RoomKind::Hall, RoomKind::Ward, 6.0f, 5.0f);
        break;
    case BuildingKind::School:
        planHallWings(RoomKind::Hall, RoomKind::Classroom, 7.5f, 4.0f);
        break;
    case BuildingKind::Barn:
        addRoom(RoomKind::Barn, inner_, -1);
        break;
    }
    outerWalls();
    // Входная дверь — изнутри закрытой створкой (только на первом этаже).
    if (level == 0) {
        for (std::size_t i = 0; i < out_.rooms.size(); ++i) {
            const Rect r = roomRect(static_cast<int>(i));
            if (data_[i].outer[static_cast<std::size_t>(doorSide_)] && doorAlong_ > r.from(doorSide_) &&
                doorAlong_ < r.to(doorSide_)) {
                entryDoor(static_cast<int>(i), doorSide_, doorAlong_);
                break;
            }
        }
    }
    for (const auto& [room, side] : panels_) {
        const Rect r = roomRect(room);
        entryDoor(room, side, (r.from(side) + r.to(side)) * 0.5f);
    }
    furnishAll();
}

} // namespace

float Furniture::minX() const {
    const bool alongZ = std::abs(std::sin(facing)) < 0.5f;
    return x - (alongZ ? width : depth) * 0.5f;
}
float Furniture::maxX() const {
    const bool alongZ = std::abs(std::sin(facing)) < 0.5f;
    return x + (alongZ ? width : depth) * 0.5f;
}
float Furniture::minZ() const {
    const bool alongZ = std::abs(std::sin(facing)) < 0.5f;
    return z - (alongZ ? depth : width) * 0.5f;
}
float Furniture::maxZ() const {
    const bool alongZ = std::abs(std::sin(facing)) < 0.5f;
    return z + (alongZ ? depth : width) * 0.5f;
}

int InteriorLayout::roomAt(float x, float z) const {
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        const InteriorRoom& r = rooms[i];
        if (x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int flatsPerLevel(BuildingKind kind, float x0, float z0, float x1, float z1) {
    if (kind != BuildingKind::Apartment) {
        return 1;
    }
    const float length = std::max(x1 - x0, z1 - z0) - 2.0f * kOuterWall - kStairsWidth;
    return std::max(1, static_cast<int>(std::lround(length / kFlatWidth)));
}

int livingLevels(BuildingKind kind, int storeys) {
    return kind == BuildingKind::Apartment ? std::max(1, storeys) : 1;
}

InteriorLayout planInterior(const InteriorInput& input) {
    InteriorLayout layout;
    Planner planner(input, layout);
    planner.run();
    return layout;
}

std::vector<Placement> placeOccupants(const InteriorLayout& layout, std::vector<Occupant> occupants) {
    std::sort(occupants.begin(), occupants.end(), [](const Occupant& a, const Occupant& b) { return a.id < b.id; });
    std::vector<bool> used(layout.spots.size(), false);
    // Кто-то в квартире ест — бодрствующие домашние садятся с ним за стол.
    std::vector<int> meals;
    for (const Occupant& o : occupants) {
        if (o.what == Occupation::Eat && std::find(meals.begin(), meals.end(), o.flat) == meals.end()) {
            meals.push_back(o.flat);
        }
    }
    const auto takeSpot = [&](std::initializer_list<SpotKind> kinds, int flat) {
        for (const SpotKind kind : kinds) {
            for (std::size_t i = 0; i < layout.spots.size(); ++i) {
                const InteriorSpot& s = layout.spots[i];
                if (!used[i] && s.kind == kind && (s.flat == flat || s.flat < 0)) {
                    used[i] = true;
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    };
    std::vector<Placement> result;
    result.reserve(occupants.size());
    int strays = 0;
    for (const Occupant& o : occupants) {
        Placement p;
        p.id = o.id;
        p.what = o.what;
        const bool meal = std::find(meals.begin(), meals.end(), o.flat) != meals.end();
        int s = -1;
        switch (o.what) {
        case Occupation::Sleep:
            s = takeSpot({SpotKind::Bed, SpotKind::Sofa}, o.flat);
            break;
        case Occupation::Eat:
            s = takeSpot({SpotKind::Seat, SpotKind::Desk, SpotKind::Sofa}, o.flat);
            p.meal = s >= 0 && layout.spots[static_cast<std::size_t>(s)].kind == SpotKind::Seat;
            break;
        case Occupation::Treat:
            s = takeSpot({SpotKind::Seat, SpotKind::Sofa, SpotKind::Desk}, o.flat);
            break;
        case Occupation::Rest:
            s = meal ? takeSpot({SpotKind::Seat}, o.flat) : -1;
            p.meal = s >= 0;
            if (s < 0) {
                s = takeSpot({SpotKind::Sofa, SpotKind::Seat, SpotKind::Desk}, o.flat);
            }
            break;
        case Occupation::Talk:
        case Occupation::Other:
            s = meal ? takeSpot({SpotKind::Seat}, o.flat) : -1;
            p.meal = s >= 0;
            if (s < 0 && o.what == Occupation::Other && (o.id & 1u) == 0u) {
                s = takeSpot({SpotKind::Stove}, o.flat); // хлопочет на кухне
            }
            if (s < 0) {
                s = takeSpot({SpotKind::Stand, SpotKind::Stove, SpotKind::Sofa, SpotKind::Seat}, o.flat);
            }
            break;
        }
        if (s >= 0) {
            const InteriorSpot& spot = layout.spots[static_cast<std::size_t>(s)];
            p.spot = s;
            p.kind = spot.kind;
            p.x = spot.x;
            p.z = spot.z;
            p.heading = spot.heading;
            p.height = spot.height;
            p.room = spot.room;
            if (o.what == Occupation::Sleep && spot.kind == SpotKind::Sofa) {
                p.heading += kPi * 0.5f; // лёг вдоль дивана
            }
        } else {
            // Места не нашлось: стоит посреди комнаты своей квартиры.
            int room = -1;
            for (std::size_t i = 0; i < layout.rooms.size(); ++i) {
                if (layout.rooms[i].flat == o.flat) {
                    room = static_cast<int>(i);
                    break;
                }
            }
            if (room < 0 && !layout.rooms.empty()) {
                room = 0;
            }
            if (room >= 0) {
                const InteriorRoom& r = layout.rooms[static_cast<std::size_t>(room)];
                const float angle = static_cast<float>(strays) * 2.4f;
                const float reach = std::min({0.5f + 0.25f * static_cast<float>(strays), (r.x1 - r.x0) * 0.4f,
                                              (r.z1 - r.z0) * 0.4f});
                p.x = (r.x0 + r.x1) * 0.5f + std::sin(angle) * reach;
                p.z = (r.z0 + r.z1) * 0.5f + std::cos(angle) * reach;
                p.heading = angle + kPi;
                p.room = room;
            }
            ++strays;
        }
        result.push_back(p);
    }
    return result;
}

} // namespace lw
