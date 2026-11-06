#pragma once

#include "sim/Components.h"
#include "sim/Id.h"

#include <cstdint>
#include <vector>

namespace lw {

// Комнаты внутри зданий (этап 12а.14) — раскладка без графики: по виду,
// участку, двери и seed здания (из id) — комнаты, стены с проёмами дверей и
// окон, мебель коробками, места для людей (кровать, стул у стола, диван, у
// плиты) и лампы. Детерминированно, только облик: симуляция и хеш мира не
// меняются (человек в доме для симуляции по-прежнему стоит у своей двери).
// Рисует render/Models3D (emitInterior), кто где сидит — placeOccupants.
//
// Координаты — мира (x — восток, z — юг; высоты мебели и проёмов — от пола
// этажа). Здания стоят по осям, поэтому и всё внутри — по осям.

// Проём в наружной стене — окно или входная дверь (render/Models3D,
// buildingOpenings: те же окна, что снаружи).
struct WallOpening {
    bool door = false;
    bool alongX = true;   // стена идёт вдоль оси X мира (её плоскость — z = plane), иначе вдоль Z
    float plane = 0.0f;   // наружная плоскость стены: z или x мира
    float outward = 1.0f; // куда наружу по нормали стены: +1 или −1
    float a0 = 0.0f;      // вдоль стены, мир (x или z)
    float a1 = 0.0f;
    float y0 = 0.0f;      // по высоте, мир
    float y1 = 0.0f;
    int storey = 0;
};

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

enum class RoomKind : std::uint8_t {
    Hall,      // прихожая
    Kitchen,
    Living,    // гостиная
    Bedroom,
    Bath,
    Stairs,    // лестничная клетка многоэтажки
    Sales,     // торговый зал
    Storeroom, // склад, подсобка
    Ward,      // палата
    Office,    // кабинет, дежурная часть
    Classroom,
    Cells,     // камеры
    Barn,      // сарай
};

// Комната: прямоугольник между гранями стен.
struct InteriorRoom {
    RoomKind kind = RoomKind::Living;
    float x0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float z1 = 0.0f;
    int flat = 0; // квартира на этаже (в доме — 0)
    Rgb wall;     // обои
    Rgb floor;    // доски, плитка, линолеум
};

// Проём в стене: дверь (y0 = 0) или окно, по высоте — от пола.
struct WallGap {
    float a0 = 0.0f;
    float a1 = 0.0f;
    float y0 = 0.0f;
    float y1 = 0.0f;
    bool window = false;
};

// Стена — брус по осям: across — толщина (z для стены вдоль X), along — длина.
struct InteriorWall {
    bool alongX = true;
    float lo = 0.0f; // поперёк: от и до (z для стены вдоль X, иначе x)
    float hi = 0.0f;
    float a0 = 0.0f; // вдоль
    float a1 = 0.0f;
    bool outer = false;
    std::vector<WallGap> gaps;
    Rgb colorLo; // грань со стороны lo (обои той комнаты)
    Rgb colorHi;
};

enum class FurnitureKind : std::uint8_t {
    Bed,
    Table,
    Chair,
    Sofa,
    Armchair,
    Wardrobe,
    Counter,   // кухонный стол-шкаф, прилавок
    Stove,
    Fridge,
    Sink,
    Shelf,     // стеллаж с книгами, полки магазина
    Nightstand,
    TvStand,
    Rug,
    Tub,
    Lamp,      // абажур под потолком
    Desk,      // парта, письменный стол
    Board,     // школьная доска на стене
    Bars,      // решётка камеры
    Bales,     // тюки сена
    Crate,
    Stall,     // перегородка стойла
    Door,      // закрытая входная дверь изнутри
    Steps,     // лестничный марш
};

// Предмет мебели: середина основания (x, z), ширина поперёк фронта, глубина,
// высота; facing — куда смотрит перёд (как у моделей: atan2(dx, dz), шагами
// по 90°). Кровать перёдом — к ногам, стул — туда, куда смотрит сидящий.
struct Furniture {
    FurnitureKind kind = FurnitureKind::Table;
    float x = 0.0f;
    float z = 0.0f;
    float width = 1.0f;
    float depth = 1.0f;
    float height = 1.0f;
    float base = 0.0f; // от пола (абажур — под потолком)
    float facing = 0.0f;
    Rgb color;
    int room = -1;
    std::uint32_t seed = 0; // разброс мелочи: книги на полках, одеяло
    // Прямоугольник на полу (мир) с учётом поворота.
    float minX() const;
    float maxX() const;
    float minZ() const;
    float maxZ() const;
};

// Где может быть человек и в какой позе.
enum class SpotKind : std::uint8_t {
    Bed,   // лёжа на кровати: heading — к ногам
    Seat,  // на стуле у стола: лицом к столу
    Sofa,  // на диване или в кресле
    Desk,  // за партой, письменным столом
    Stove, // стоя у плиты или у стола-шкафа: лицом к нему
    Stand, // стоя посреди комнаты
};

struct InteriorSpot {
    SpotKind kind = SpotKind::Stand;
    float x = 0.0f;
    float z = 0.0f;
    float heading = 0.0f; // куда обращён (atan2(dx, dz))
    float height = 0.0f;  // поверхность под ним от пола: матрас, сиденье
    int room = -1;
    int flat = 0;
};

struct InteriorLamp {
    float x = 0.0f;
    float y = 0.0f; // мир
    float z = 0.0f;
    int room = -1;
};

struct InteriorLayout {
    BuildingKind kind = BuildingKind::House;
    int level = 0;        // этаж
    float floor = 0.0f;   // пол этажа, мир
    float ceiling = 0.0f; // потолок, мир
    // Участок здания и внутренняя грань наружных стен.
    float outerX0 = 0.0f;
    float outerZ0 = 0.0f;
    float outerX1 = 0.0f;
    float outerZ1 = 0.0f;
    float x0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float z1 = 0.0f;
    int flats = 1; // квартир на этаже
    std::vector<InteriorRoom> rooms;
    std::vector<InteriorWall> walls;
    std::vector<Furniture> furniture;
    std::vector<InteriorSpot> spots;
    std::vector<InteriorLamp> lamps;

    // Комната в точке; −1 — в стене или снаружи.
    int roomAt(float x, float z) const;
};

// Что нужно знать о здании.
struct InteriorInput {
    BuildingKind kind = BuildingKind::House;
    float x0 = 0.0f; // участок, мир
    float z0 = 0.0f;
    float x1 = 0.0f;
    float z1 = 0.0f;
    float floor = 0.0f;  // пол первого этажа, мир
    float storey = 3.0f; // высота этажа
    int storeys = 1;
    float doorX = 0.0f;  // середина входной двери на стене, мир
    float doorZ = 0.0f;
    std::uint64_t seed = 0; // из id здания
    std::vector<WallOpening> openings;
    int level = 0; // какой этаж
    // Сколько живёт в каждой квартире этажа (кроватей не меньше); нет — двое.
    std::vector<int> residents;
};

// Сколько квартир на этаже (в доме и не жилом — 1).
int flatsPerLevel(BuildingKind kind, float x0, float z0, float x1, float z1);
// Сколько этажей с комнатами: в доме — первый (спальни там же), в
// многоэтажке — все.
int livingLevels(BuildingKind kind, int storeys);

InteriorLayout planInterior(const InteriorInput& input);

// Кто дома и что делает — куда его посадить.
enum class Occupation : std::uint8_t {
    Sleep,
    Eat,
    Rest,  // отдыхает
    Treat, // перевязывается
    Talk,  // общается с домашними
    Other, // просто дома
};

struct Occupant {
    EntityId id = kInvalidId;
    int flat = 0;
    Occupation what = Occupation::Other;
};

struct Placement {
    EntityId id = kInvalidId;
    int spot = -1; // −1 — стоит, где место нашлось
    SpotKind kind = SpotKind::Stand;
    float x = 0.0f;
    float z = 0.0f;
    float heading = 0.0f;
    float height = 0.0f; // поверхность под ним от пола
    int room = -1;
    bool meal = false;   // за общим столом: кто-то в квартире ест — остальные садятся рядом
    Occupation what = Occupation::Other;
};

// Места жильцов: спящие — в кроватях своей квартиры, едящие — за столом (и
// бодрствующие домашние с ними — общий ужин), отдыхающие — на диване и в
// кресле, прочие — стоя. Детерминированно по id: тот же вход — те же места.
std::vector<Placement> placeOccupants(const InteriorLayout& layout, std::vector<Occupant> occupants);

} // namespace lw
