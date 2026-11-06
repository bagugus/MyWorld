#include "ui/Showrunner.h"

#include "sim/Calendar.h"
#include "sim/Cast.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/Story.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/Captions.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"
#include "ui/SeriesFrame.h"
#include "ui/StoryText.h"
#include "ui/Ui.h"

#include <raymath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

namespace lw::ui {

namespace {

// Эпизод: не короче kEpisodeMin реальных секунд (дальше кончается с первой же
// сценой) и не длиннее kEpisodeMax (сцену обрывают, если не идёт кульминация).
constexpr float kEpisodeMin = 300.0f;
constexpr float kEpisodeMax = 600.0f;
// Сцена: не короче kSceneMin и не длиннее kSceneMax секунд (мирная — kCalm…);
// кульминация держит сцену, пока не случится, но не дольше kSceneMax × kClimaxHold.
constexpr float kSceneMin = 25.0f;
constexpr float kSceneMax = 110.0f;
constexpr float kCalmMin = 14.0f;
constexpr float kCalmMax = 35.0f;
constexpr float kClimaxHold = 2.0f;
// Сцене скучно столько секунд подряд (после минимальной длины) — конец.
constexpr float kDullFor = 6.0f;
// Интересность кадра ниже этого — скучно.
constexpr float kSceneDull = 0.08f;
// Случилось (предвестие сбылось, герой погиб) — досмотреть столько секунд.
constexpr float kAftermath = 10.0f;
// Сцена стоит показа, если её оценка не ниже kSceneWorthy; скачок времени
// кончается, когда у героев появилось что-то не скучнее kWakeScore.
constexpr float kSceneWorthy = 0.12f;
constexpr float kWakeScore = 0.25f;
// Веса сцены: линия A, линия B, с героем эпизода; уже показанное предвестие,
// до которого ещё далеко (герой только идёт), — kWaiting; та же, что и прошлая
// сцена, — kRepeat (линии чередуются).
constexpr float kLineAWeight = 2.0f;
constexpr float kLineBWeight = 1.6f;
constexpr float kHeroWeight = 1.4f;
constexpr float kWaiting = 0.25f;
constexpr float kRepeat = 0.6f;
constexpr float kAsleep = 0.3f;
// Линия главного героя сериала берётся в эпизод, если она не скучнее этой доли
// самой интересной.
constexpr float kStarredShare = 0.6f;
// Предвестие «созрело», когда до кульминации не больше kRipeEta секунд симуляции.
constexpr float kRipeEta = 120.0f;
// Срочное: кульминация через столько реальных секунд (как у режиссёра).
constexpr float kLead = 15.0f;
constexpr float kStillRate = 0.05f;
// Переходы: затемнение за kFadeDown, проявка за kFadeUp секунд; вводный титр —
// kIntro (после скачка — kIntroJump, к кульминации — kIntroHurry); установочный
// план — kEstablish.
constexpr float kFadeDown = 0.7f;
constexpr float kFadeUp = 1.0f;
constexpr float kIntro = 2.8f;
constexpr float kIntroJump = 3.8f;
constexpr float kIntroHurry = 1.2f;
constexpr float kEstablish = 4.0f;
// Перебивка — kCutaway секунд; ту же угрозу снова — не раньше kIronyRepeat;
// угроза ближе kIronyEta секунд симуляции — уже не перебивка, а сцена.
constexpr float kCutaway = 9.0f;
constexpr float kIronyRepeat = 180.0f;
constexpr float kIronyEta = 45.0f;
// Укушенный рядом со знакомым — ближе этого, метры.
constexpr float kIronyNear = 40.0f;
// Хозяин дома дальше этого от двери — «далеко от дома».
constexpr float kAwayFromHome = 60.0f;
// Начало и конец эпизода.
constexpr float kOpeningCard = 5.0f;
constexpr float kRecapTime = 8.0f;
constexpr float kCliffhanger = 8.0f;
constexpr float kClosing = 4.0f;
constexpr std::size_t kRecapLines = 4;
// Скачок: порции промотки по kJumpChunk тиков; не короче kJumpMin (если не
// срочно) и не длиннее kJumpMax тиков, не дольше kJumpReal реальных секунд.
// Будит: предвестие у знакомых ближе kWakeEta секунд симуляции, событие летописи
// героя весом не меньше kWakeWeight, сцена не скучнее kWakeScore.
constexpr std::uint64_t kJumpChunk = 20 * kTicksPerSecond;
constexpr std::uint64_t kJumpMin = 20 * kTicksPerGameMinute;
constexpr std::uint64_t kJumpMax = 6 * kTicksPerGameHour;
constexpr float kJumpReal = 10.0f;
constexpr float kWakeEta = 90.0f;
constexpr float kWakeWeight = 1.0f;
// Зритель знакомится с теми, кто в сцене ближе kKnownNear метров к герою.
constexpr float kKnownNear = 40.0f;
// Человек у двери здания — ближе этого, метры: сцена «в» этом здании.
constexpr float kAtDoor = 10.0f;
// Установочный план: вокруг здания — поля kPlaceMargin метров, вокруг человека
// под открытым небом — круг kPlaceRadius.
constexpr float kPlaceMargin = 22.0f;
constexpr float kPlaceRadius = 45.0f;
// Ближе этого к поселению — «у деревни», дальше — «в N км от».
constexpr float kNearSettlement = 1200.0f;
// Сколько людей держать в «знакомых» (не больше): лишнее не страшно.
constexpr std::size_t kCastIds = 6;

bool isPersonAlive(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    return entity != entt::null && world.registry().all_of<Person>(entity);
}

bool asleep(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    const auto* person = entity != entt::null ? world.registry().try_get<Person>(entity) : nullptr;
    return person != nullptr && person->asleep;
}

bool hasName(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    return entity != entt::null && world.registry().all_of<Name>(entity);
}

// «John Miller» без номера; без имени — подпись интерфейса.
std::string nameOf(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    if (entity != entt::null) {
        if (const auto* name = world.registry().try_get<Name>(entity)) {
            return name->first + " " + name->last;
        }
    }
    return frame::clean(idLabel(id));
}

std::string firstNameOf(const World& world, EntityId id) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    if (entity != entt::null) {
        if (const auto* name = world.registry().try_get<Name>(entity)) {
            return name->first;
        }
    }
    return nameOf(world, id);
}

const char* pronoun(const World& world, EntityId id) {
    return isFemale(world, id) ? "она" : "он";
}

std::string capitalized(const char* lower) {
    // Только для своих слов: «утро» → «Утро» (кириллица в UTF-8 — два байта).
    static constexpr std::pair<const char*, const char*> kCaps[] = {
        {"утро", "Утро"}, {"день", "День"}, {"вечер", "Вечер"}, {"ночь", "Ночь"},
        {"он", "Он"},     {"она", "Она"}};
    for (const auto& [from, to] : kCaps) {
        if (std::strcmp(from, lower) == 0) {
            return to;
        }
    }
    return lower;
}

const char* timeOfDay(int hour) {
    if (hour >= 5 && hour < 11) {
        return "утро";
    }
    if (hour >= 11 && hour < 17) {
        return "день";
    }
    if (hour >= 17 && hour < 22) {
        return "вечер";
    }
    return "ночь";
}

// «утром», «днём», «вечером», «ночью».
const char* timeOfDayWhen(int hour) {
    if (hour >= 5 && hour < 11) {
        return "утром";
    }
    if (hour >= 11 && hour < 17) {
        return "днём";
    }
    if (hour >= 17 && hour < 22) {
        return "вечером";
    }
    return "ночью";
}

// «в 350 метрах», «в километре», «в двух километрах», «в 14 км».
std::string distanceAt(float meters) {
    char text[64];
    if (meters < 1000.0f) {
        const int rounded = std::max(50, static_cast<int>(std::lround(meters / 50.0f)) * 50);
        if (rounded >= 1000) {
            return "в километре";
        }
        std::snprintf(text, sizeof(text), "в %d метрах", rounded);
        return text;
    }
    const int km = static_cast<int>(std::lround(meters / 1000.0f));
    if (km <= 1) {
        return "в километре";
    }
    constexpr const char* kLocative[] = {"", "", "двух", "трёх", "четырёх", "пяти",
                                         "шести", "семи", "восьми", "девяти", "десяти"};
    if (km < static_cast<int>(std::size(kLocative))) {
        return std::string("в ") + kLocative[km] + " километрах";
    }
    std::snprintf(text, sizeof(text), "в %d км", km);
    return text;
}

// «Полчаса спустя», «Три часа спустя», «На следующее утро».
std::string jumpPhrase(std::uint64_t from, std::uint64_t to) {
    const std::uint64_t ticks = to - std::min(to, from);
    const CalendarTime start = calendarAt(from);
    const CalendarTime now = calendarAt(to);
    const auto hours = static_cast<int>((ticks + kTicksPerGameHour / 2) / kTicksPerGameHour);
    if (now.day != start.day && now.hour >= 5 && now.hour < 11 && hours >= 4) {
        return "На следующее утро";
    }
    if (now.day != start.day && hours >= 10) {
        return "На следующий день";
    }
    const std::uint64_t minutes = ticks / kTicksPerGameMinute;
    if (minutes < 20) {
        return "Немного позже";
    }
    if (minutes < 45) {
        return "Полчаса спустя";
    }
    if (minutes < 90) {
        return "Час спустя";
    }
    constexpr const char* kHours[] = {"",       "",       "Два",         "Три",        "Четыре",
                                      "Пять",   "Шесть",  "Семь",        "Восемь",     "Девять",
                                      "Десять", "Одиннадцать", "Двенадцать"};
    const int count = std::clamp(hours, 2, static_cast<int>(std::size(kHours)) - 1);
    return std::string(kHours[count]) + (count <= 4 ? " часа спустя" : " часов спустя");
}

// Когда зритель видел то, что было на тике seen: «только что», «утром»,
// «вчера», «несколько дней назад».
std::string whenSeen(std::uint64_t seen, std::uint64_t now) {
    if (now - std::min(now, seen) < kTicksPerGameHour) {
        return "только что";
    }
    const CalendarTime then = calendarAt(seen);
    const CalendarTime today = calendarAt(now);
    if (then.day == today.day) {
        return timeOfDayWhen(then.hour);
    }
    if (then.day + 1 == today.day) {
        return "вчера";
    }
    return "несколько дней назад";
}

struct Place {
    std::string line;
    EntityId building = kInvalidId;
    Vector2 at{};
};

Vector2 doorOf(const Building& building) {
    return {Map::cellCenter(building.door.x), Map::cellCenter(building.door.y)};
}

Vector2 centerOf(const Building& building) {
    return {(static_cast<float>(building.origin.x) + building.width * 0.5f) * Map::kCellSize,
            (static_cast<float>(building.origin.y) + building.height * 0.5f) * Map::kCellSize};
}

// «дом семьи Miller», «база группы John Miller», «больница».
std::string buildingTitle(const World& world, entt::entity entity, EntityId buildingId, EntityId who) {
    const entt::registry& registry = world.registry();
    const Building& building = registry.get<Building>(entity);
    for (const auto [groupEntity, group] : registry.view<const Group>().each()) {
        if (group.base == buildingId) {
            return "база группы " + nameOf(world, group.leader);
        }
    }
    switch (building.kind) {
    case BuildingKind::House:
    case BuildingKind::Apartment: {
        // Фамилия жильцов: сначала того, о ком сцена, если он здесь живёт.
        std::string family;
        const entt::entity self = world.find(who);
        if (self != entt::null) {
            const auto* person = registry.try_get<Person>(self);
            const auto* name = registry.try_get<Name>(self);
            if (person != nullptr && name != nullptr && person->home == buildingId) {
                family = name->last;
            }
        }
        if (family.empty()) {
            EntityId first = kInvalidId;
            for (const auto [personEntity, person, name, id] : registry.view<const Person, const Name, const Id>().each()) {
                if (person.home == buildingId && (first == kInvalidId || id.value < first)) {
                    first = id.value;
                    family = name.last;
                }
            }
        }
        const char* kind = building.kind == BuildingKind::House ? "дом" : "многоэтажка";
        if (family.empty()) {
            return building.kind == BuildingKind::House ? "пустой дом" : kind;
        }
        return building.kind == BuildingKind::House ? "дом семьи " + family : "квартира семьи " + family;
    }
    case BuildingKind::Barn:
        return "ферма";
    case BuildingKind::Shop:
        return "магазин";
    case BuildingKind::Hospital:
        return "больница";
    case BuildingKind::PoliceStation:
        return "полицейский участок";
    case BuildingKind::School:
        return "школа";
    default:
        return "дом";
    }
}

// Как назвать местность под открытым небом: «Лес», «Поле», «Берег реки».
const char* terrainTitle(const Map& map, Cell cell) {
    if (!map.contains(cell)) {
        return "Глушь";
    }
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const Cell near{cell.x + dx, cell.y + dy};
            if (!map.contains(near)) {
                continue;
            }
            switch (map.water.at(near.x, near.y)) {
            case Water::Sea:
                return "Морской берег";
            case Water::Lake:
                return "Берег озера";
            case Water::River:
                return "Берег реки";
            default:
                break;
            }
        }
    }
    if (map.cover.at(cell.x, cell.y) == Cover::Forest) {
        return "Лес";
    }
    if (map.road.at(cell.x, cell.y) != Road::None) {
        return "Дорога";
    }
    switch (map.ground.at(cell.x, cell.y)) {
    case Ground::Field:
        return "Поле";
    case Ground::Rock:
        return "Скалы";
    case Ground::Sand:
        return "Пустошь";
    default:
        return "Луг";
    }
}

// Где сцена: «Old Village, дом семьи Miller», «Лес у деревни Old Village».
Place describePlace(const World& world, EntityId id) {
    Place place;
    if (!positionOf(world, id, 1.0f, place.at)) {
        return place;
    }
    const entt::registry& registry = world.registry();
    const entt::entity self = world.find(id);
    entt::entity buildingEntity = entt::null;
    if (const auto* person = registry.try_get<Person>(self); person != nullptr && person->indoors) {
        buildingEntity = world.find(person->home);
        place.building = person->home;
    }
    if (registry.all_of<Building>(self)) {
        buildingEntity = self;
        place.building = id;
    }
    if (buildingEntity == entt::null) {
        float nearest = kAtDoor;
        for (const auto [entity, building, buildingId] : registry.view<const Building, const Id>().each()) {
            const float x0 = static_cast<float>(building.origin.x) * Map::kCellSize;
            const float y0 = static_cast<float>(building.origin.y) * Map::kCellSize;
            const bool inside = place.at.x >= x0 && place.at.y >= y0 &&
                                place.at.x < x0 + building.width * Map::kCellSize &&
                                place.at.y < y0 + building.height * Map::kCellSize;
            const float distance = inside ? 0.0f : Vector2Distance(place.at, doorOf(building));
            if (distance < nearest || (inside && distance <= nearest)) {
                nearest = distance;
                buildingEntity = entity;
                place.building = buildingId.value;
            }
        }
    }
    if (buildingEntity != entt::null && !registry.all_of<Building>(buildingEntity)) {
        buildingEntity = entt::null;
        place.building = kInvalidId;
    }
    // Поселение: здания — его, иначе ближайшее, в чей круг попадает точка.
    entt::entity settlementEntity = entt::null;
    if (buildingEntity != entt::null) {
        settlementEntity = world.find(registry.get<Building>(buildingEntity).settlement);
    }
    float closest = std::numeric_limits<float>::infinity();
    entt::entity nearestSettlement = entt::null;
    if (settlementEntity == entt::null) {
        for (const auto [entity, settlement] : registry.view<const Settlement>().each()) {
            const Vector2 center{Map::cellCenter(settlement.center.x), Map::cellCenter(settlement.center.y)};
            const float distance = Vector2Distance(center, place.at);
            const float radius = static_cast<float>(settlement.radius) * Map::kCellSize;
            if (distance <= radius * 1.1f) {
                settlementEntity = entity;
                break;
            }
            if (distance - radius < closest) {
                closest = std::max(0.0f, distance - radius);
                nearestSettlement = entity;
            }
        }
    }
    if (settlementEntity != entt::null && registry.all_of<Settlement>(settlementEntity)) {
        place.line = registry.get<Settlement>(settlementEntity).name;
        if (buildingEntity != entt::null) {
            place.line += ", " + buildingTitle(world, buildingEntity, place.building, id);
        }
        return place;
    }
    const Map& map = world.map();
    place.line = terrainTitle(map, Map::cellAt(place.at.x, place.at.y));
    if (nearestSettlement != entt::null) {
        const Settlement& settlement = registry.get<Settlement>(nearestSettlement);
        const char* kind = settlement.kind == SettlementKind::Town ? "города" : "деревни";
        if (closest <= kNearSettlement) {
            place.line += std::string(" у ") + kind + " " + settlement.name;
        } else {
            char text[32];
            std::snprintf(text, sizeof(text), " в %.0f км от ", std::max(1.0f, std::round(closest / 1000.0f)));
            place.line += text + std::string(kind) + " " + settlement.name;
        }
    }
    return place;
}

// Все, о ком кандидат (не считая длинного списка участников).
void collectIds(const StoryTracker& stories, const StoryPick& pick, std::vector<EntityId>& ids) {
    for (const EntityId id : {pick.target, pick.hero, pick.visit}) {
        ids.push_back(id);
    }
    if (const Story* story = stories.find(pick.story)) {
        ids.push_back(story->hero);
        ids.push_back(story->other);
        for (std::size_t i = 0; i < story->cast.size() && i < kCastIds; ++i) {
            ids.push_back(story->cast[i]);
        }
    }
    if (pick.hasOmen) {
        ids.push_back(pick.omen.hero);
        if (pick.omen.kind != ForesightKind::Raid) {
            ids.push_back(pick.omen.other); // у орды other — поселение
        }
    }
}

} // namespace

bool StoryPick::same(const StoryPick& other) const {
    if (story != 0 || other.story != 0) {
        return story == other.story;
    }
    if (visit != kInvalidId || other.visit != kInvalidId) {
        return visit == other.visit;
    }
    return hasOmen && other.hasOmen && omen.same(other.omen);
}

bool positionOf(const World& world, EntityId id, float alpha, Vector2& at) {
    const entt::entity entity = id != kInvalidId ? world.find(id) : entt::null;
    if (entity == entt::null) {
        return false;
    }
    const entt::registry& registry = world.registry();
    if (const auto* position = registry.try_get<Position>(entity)) {
        at = {position->x, position->y};
        if (const auto* velocity = registry.try_get<Velocity>(entity)) {
            // Как рисует рендер: между прошлым и последним тиком.
            const float back = (1.0f - std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
            at.x -= velocity->x * back;
            at.y -= velocity->y * back;
        }
        return true;
    }
    if (const auto* building = registry.try_get<Building>(entity)) {
        at = centerOf(*building);
        return true;
    }
    if (const auto* horde = registry.try_get<Horde>(entity)) {
        return positionOf(world, horde->leader, alpha, at);
    }
    if (const auto* group = registry.try_get<Group>(entity)) {
        return positionOf(world, group->leader, alpha, at);
    }
    if (const auto* event = registry.try_get<WorldEvent>(entity)) {
        at = {Map::cellCenter(event->site.x), Map::cellCenter(event->site.y)};
        return true;
    }
    if (const auto* settlement = registry.try_get<Settlement>(entity)) {
        at = {Map::cellCenter(settlement->center.x), Map::cellCenter(settlement->center.y)};
        return true;
    }
    return false;
}

const char* showPhaseTitle(ShowPhase phase) {
    constexpr const char* kNames[] = {"выключен",      "начало эпизода", "затемнение", "скачок времени",
                                      "вводный титр",  "общий план",     "сцена",      "перебивка",
                                      "клиффхэнгер",   "конец эпизода"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ShowPhase::Count));
    const auto index = static_cast<std::size_t>(phase);
    return index < std::size(kNames) ? kNames[index] : "?";
}

void Showrunner::start() {
    *this = Showrunner{};
    begin_ = true;
    phase_ = ShowPhase::Opening;
}

void Showrunner::stop() {
    phase_ = ShowPhase::Off;
    begin_ = false;
}

void Showrunner::setPhase(ShowPhase phase) {
    phase_ = phase;
    phaseAge_ = 0.0f;
}

void Showrunner::note(std::string text) {
    if (active() && !text.empty()) {
        notes_.push_back(frame::clean(text));
    }
}

bool Showrunner::isHero(EntityId id) const {
    return id != kInvalidId && std::find(heroes_.begin(), heroes_.end(), id) != heroes_.end();
}

bool Showrunner::isKnown(EntityId id) const {
    return id != kInvalidId && (isHero(id) || known_.contains(id));
}

EntityId Showrunner::knownIn(const World& world, EntityId settlement) const {
    // Сначала герои эпизода, потом остальные знакомые; знакомых немного.
    EntityId found = kInvalidId;
    const entt::registry& registry = world.registry();
    const auto lives = [&](EntityId id) {
        const entt::entity entity = world.find(id);
        const auto* person = entity != entt::null ? registry.try_get<Person>(entity) : nullptr;
        if (person == nullptr) {
            return false;
        }
        const entt::entity home = world.find(person->home);
        return home != entt::null && registry.all_of<Building>(home) &&
               registry.get<Building>(home).settlement == settlement;
    };
    for (const EntityId id : heroes_) {
        if (lives(id)) {
            return id;
        }
    }
    for (const EntityId id : known_) {
        if (lives(id) && (found == kInvalidId || id < found)) {
            found = id; // наименьший id — чтобы выбор не зависел от порядка в set
        }
    }
    return found;
}

bool Showrunner::involves(const ShowInput& in, const StoryPick& pick, bool knownOnly) const {
    std::vector<EntityId> ids;
    collectIds(*in.stories, pick, ids);
    for (const EntityId id : ids) {
        if (knownOnly ? isKnown(id) : isHero(id)) {
            return true;
        }
    }
    // Связан с героем эпизода общими событиями — зритель знает его через героя.
    if (knownOnly) {
        for (const EntityId id : ids) {
            for (const EntityId hero : heroes_) {
                if (id != kInvalidId && id != hero && in.cast->link(id, hero) > 0.0f) {
                    return true;
                }
            }
        }
    }
    // Орда идёт туда, где живёт знакомый, — угроза знакомому.
    if (knownOnly && pick.hasOmen && pick.omen.kind == ForesightKind::Raid) {
        return knownIn(*in.world, pick.omen.other) != kInvalidId;
    }
    return false;
}

float Showrunner::score(const ShowInput& in, const StoryPick& pick) const {
    if (!pick.valid() || pick.visit != kInvalidId || pick.target == kInvalidId) {
        return 0.0f; // навестить героя — только когда показывать нечего (visitPick)
    }
    const bool hero = involves(in, pick, false);
    if (!hero && !involves(in, pick, true)) {
        return 0.0f; // на экране только знакомые или угроза знакомому
    }
    float weight = hero ? kHeroWeight : 1.0f;
    if (lineA_.valid() && pick.same(lineA_)) {
        weight = kLineAWeight;
    } else if (lineB_.valid() && pick.same(lineB_)) {
        weight = kLineBWeight;
    }
    const bool ripe = pick.hasOmen && pick.omen.eta <= kRipeEta;
    if (pick.hasOmen && !ripe &&
        std::any_of(established_.begin(), established_.end(),
                    [&](const Foresight& omen) { return omen.same(pick.omen); })) {
        weight *= kWaiting; // уже видели, куда он идёт: вернёмся, когда подойдёт
    }
    if (last_.valid() && pick.same(last_) && !ripe) {
        // Прошлую сцену повторяют, только если в её линии что-то случилось.
        const Story* story = in.stories->find(pick.story);
        if (story == nullptr || story->updated <= lastSceneTick_ || !story->active()) {
            return 0.0f;
        }
        weight *= kRepeat;
    }
    if (!pick.hasOmen && asleep(*in.world, pick.target)) {
        weight *= kAsleep; // спит — смотреть нечего: такую ночь вырезают скачком
    }
    return pick.interest * weight;
}

const StoryPick* Showrunner::best(const ShowInput& in, float& bestScore) const {
    const StoryPick* found = nullptr;
    bestScore = 0.0f;
    for (const StoryPick& pick : *in.picks) {
        const float value = score(in, pick);
        if (value > bestScore) {
            bestScore = value;
            found = &pick;
        }
    }
    return found;
}

bool Showrunner::urgentElsewhere(const ShowInput& in) const {
    for (const StoryPick& pick : *in.picks) {
        if (!pick.hasOmen || pick.same(in.current) || in.simRate <= kStillRate ||
            pick.omen.eta / in.simRate > kLead || !involves(in, pick, false)) {
            continue;
        }
        return true;
    }
    return false;
}

bool Showrunner::irony(const ShowInput& in, StoryPick& pick, std::string& text) const {
    const World& world = *in.world;
    const entt::registry& registry = world.registry();
    const std::uint64_t now = world.tick();
    for (const Foresight& omen : *in.omens) {
        if (omen.eta < kIronyEta || omen.kind == ForesightKind::CacheRace ||
            (in.current.hasOmen && in.current.omen.same(omen)) || omen.hero == in.current.target) {
            continue;
        }
        if (std::any_of(ironies_.begin(), ironies_.end(), [&](const auto& shown) {
                return shown.first.same(omen) && clock_ - shown.second < kIronyRepeat;
            })) {
            continue;
        }
        // Кому грозит и знает ли он.
        EntityId victim = kInvalidId;
        const auto aware = [&](EntityId who, EntityId of) {
            const entt::entity entity = world.find(who);
            const auto* awareness = entity != entt::null ? registry.try_get<Awareness>(entity) : nullptr;
            return awareness != nullptr && awareness->find(of) != nullptr;
        };
        std::string line;
        switch (omen.kind) {
        case ForesightKind::Revenge: {
            victim = omen.other;
            if (!isKnown(victim) || !isPersonAlive(world, victim) || aware(victim, omen.hero)) {
                continue;
            }
            Vector2 a{};
            Vector2 b{};
            if (!positionOf(world, omen.hero, 1.0f, a) || !positionOf(world, victim, 1.0f, b)) {
                continue;
            }
            line = nameOf(world, omen.hero) + " " + intentLine(world, omen.hero) + ". " +
                   capitalized(pronoun(world, omen.hero)) + " уже " + distanceAt(Vector2Distance(a, b)) +
                   ", а " + firstNameOf(world, victim) + " ещё не знает.";
            break;
        }
        case ForesightKind::Theft: {
            victim = omen.other;
            const entt::entity owner = world.find(victim);
            const entt::entity house = world.find(omen.focus);
            if (!isKnown(victim) || owner == entt::null || house == entt::null ||
                !registry.all_of<Building>(house) || !registry.all_of<Person>(owner)) {
                continue;
            }
            Vector2 at{};
            if (!positionOf(world, victim, 1.0f, at) ||
                Vector2Distance(at, doorOf(registry.get<Building>(house))) < kAwayFromHome) {
                continue; // хозяин дома — это уже не ирония, а сцена
            }
            line = nameOf(world, omen.hero) + " " + intentLine(world, omen.hero) + ". " +
                   firstNameOf(world, victim) + " сейчас далеко от дома и ничего не знает.";
            break;
        }
        case ForesightKind::Turning: {
            // Знакомый рядом с укушенным (или с телом, которое встанет).
            const Vector2 place{omen.x, omen.y};
            float nearest = kIronyNear;
            for (const EntityId id : heroes_) {
                Vector2 at{};
                if (id != omen.hero && isPersonAlive(world, id) && positionOf(world, id, 1.0f, at) &&
                    Vector2Distance(at, place) < nearest) {
                    nearest = Vector2Distance(at, place);
                    victim = id;
                }
            }
            if (victim == kInvalidId) {
                for (const EntityId id : known_) {
                    Vector2 at{};
                    if (id != omen.hero && isPersonAlive(world, id) && positionOf(world, id, 1.0f, at) &&
                        Vector2Distance(at, place) < nearest) {
                        nearest = Vector2Distance(at, place);
                        victim = id;
                    }
                }
            }
            if (victim == kInvalidId) {
                continue;
            }
            const entt::entity body = world.find(omen.hero);
            const bool dead = body != entt::null && registry.all_of<Corpse>(body);
            line = dead ? "Тело " + nameOf(world, omen.hero) + " вот-вот встанет. Рядом — " +
                              nameOf(world, victim) + ", и " + pronoun(world, victim) + " ещё не знает."
                        : "Укус " + nameOf(world, omen.hero) + " воспаляется: скоро " +
                              pronoun(world, omen.hero) + " обратится. Рядом — " + nameOf(world, victim) +
                              ", и " + pronoun(world, victim) + " ещё не знает.";
            break;
        }
        case ForesightKind::Raid: {
            victim = knownIn(world, omen.other);
            const entt::entity horde = world.find(omen.focus);
            const entt::entity village = world.find(omen.other);
            const entt::entity person = world.find(victim);
            if (victim == kInvalidId || horde == entt::null || village == entt::null ||
                !registry.all_of<Horde>(horde) || !registry.all_of<Settlement>(village)) {
                continue;
            }
            const Horde& info = registry.get<Horde>(horde);
            if (std::any_of(info.members.begin(), info.members.end(),
                            [&](EntityId member) { return aware(victim, member); })) {
                continue; // уже видит или слышит их
            }
            const Vector2 from{info.x, info.y};
            const entt::entity home = world.find(registry.get<Person>(person).home);
            std::string where;
            float distance = 0.0f;
            if (home != entt::null && registry.all_of<Building>(home)) {
                distance = Vector2Distance(from, doorOf(registry.get<Building>(home)));
                where = "от дома " + nameOf(world, victim);
            } else {
                const Settlement& settlement = registry.get<Settlement>(village);
                distance = Vector2Distance(from, {Map::cellCenter(settlement.center.x),
                                                  Map::cellCenter(settlement.center.y)});
                where = "от " + settlement.name + ", где живёт " + nameOf(world, victim);
            }
            if (const auto seen = seenHordes_.find(omen.focus); seen != seenHordes_.end()) {
                line = "Орда, которую мы видели " + whenSeen(seen->second, now) + ", " + distanceAt(distance) +
                       " " + where + ".";
            } else {
                char size[64];
                std::snprintf(size, sizeof(size), "Орда — %zu зомби — ", info.members.size());
                line = size + distanceAt(distance) + " " + where + ".";
            }
            line += " " + capitalized(pronoun(world, victim)) + " ещё не знает.";
            break;
        }
        default:
            continue;
        }
        // Кандидат режиссёра для этого предвестия, если есть, иначе своё.
        pick = {};
        for (const StoryPick& candidate : *in.picks) {
            if (candidate.hasOmen && candidate.omen.same(omen)) {
                pick = candidate;
                break;
            }
        }
        if (!pick.valid()) {
            pick.hasOmen = true;
            pick.omen = omen;
            pick.target = omen.hero;
            pick.interest = omen.interest;
        }
        if (pick.target == kInvalidId || world.find(pick.target) == entt::null) {
            continue;
        }
        text = std::move(line);
        return true;
    }
    return false;
}

EntityId Showrunner::leadOf(const ShowInput& in, const StoryPick& pick) const {
    if (!pick.valid()) {
        return kInvalidId;
    }
    const Story* story = in.stories->find(pick.story);
    const EntityId omenHero =
        pick.hasOmen && pick.omen.kind != ForesightKind::Raid ? pick.omen.hero : kInvalidId;
    for (const EntityId id : {pick.hero, story != nullptr ? story->hero : kInvalidId, omenHero, pick.target}) {
        if (isPersonAlive(*in.world, id)) {
            return id;
        }
    }
    return kInvalidId;
}

void Showrunner::chooseHeroes(const ShowInput& in) {
    heroes_.clear();
    lineA_ = lineB_ = {};
    const std::vector<StoryPick>& picks = *in.picks;
    // Линия A — продолжение клиффхэнгера, иначе самая интересная с героем сериала.
    if (cliff_.valid()) {
        for (const StoryPick& pick : picks) {
            if (pick.same(cliff_)) {
                lineA_ = pick;
                break;
            }
        }
    }
    // Иначе — самая интересная линия; линия главного героя сериала — если она
    // не намного скучнее (сериалу нужно ядро, но не ценой скуки).
    const auto pickLine = [&](EntityId except, const StoryPick* other) {
        const StoryPick* top = nullptr;
        const StoryPick* starred = nullptr;
        for (const StoryPick& pick : picks) {
            const EntityId lead = leadOf(in, pick);
            if (pick.visit != kInvalidId || lead == kInvalidId || lead == except ||
                (other != nullptr && pick.same(*other))) {
                continue;
            }
            if (top == nullptr) {
                top = &pick;
            }
            if (starred == nullptr && pick.hero != kInvalidId) {
                starred = &pick;
            }
        }
        if (starred != nullptr && top != nullptr && starred->interest >= kStarredShare * top->interest) {
            return *starred;
        }
        return top != nullptr ? *top : StoryPick{};
    };
    if (!lineA_.valid()) {
        lineA_ = pickLine(kInvalidId, nullptr);
    }
    const EntityId heroA = leadOf(in, lineA_);
    lineB_ = pickLine(heroA, &lineA_);
    const auto add = [&](EntityId id) {
        if (id != kInvalidId && heroes_.size() < 3 && !isHero(id) && isPersonAlive(*in.world, id)) {
            heroes_.push_back(id);
        }
    };
    add(heroA);
    add(leadOf(in, lineB_));
    // Третий — главный герой сериала: сперва уже знакомый зрителю.
    for (const bool knownOnly : {true, false}) {
        for (const Protagonist& hero : in.cast->heroes()) {
            if (heroes_.size() < 3 && hero.alive() && (!knownOnly || known_.contains(hero.id))) {
                add(hero.id);
            }
        }
        if (heroes_.size() >= 3) {
            break;
        }
    }
    if (!heroes_.empty()) {
        return;
    }
    // Героев ещё нет (мирное начало): двое жителей самого большого поселения
    // с самой богатой биографией.
    const World& world = *in.world;
    const entt::registry& registry = world.registry();
    EntityId largest = kInvalidId;
    int radius = -1;
    for (const auto [entity, settlement, id] : registry.view<const Settlement, const Id>().each()) {
        if (settlement.radius > radius || (settlement.radius == radius && id.value < largest)) {
            radius = settlement.radius;
            largest = id.value;
        }
    }
    std::vector<std::pair<float, EntityId>> residents;
    for (const auto [entity, person, name, id] : registry.view<const Person, const Name, const Id>().each()) {
        const entt::entity home = world.find(person.home);
        if (home != entt::null && registry.all_of<Building>(home) &&
            registry.get<Building>(home).settlement == largest) {
            residents.emplace_back(in.cast->weight(id.value), id.value);
        }
    }
    std::sort(residents.begin(), residents.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    for (std::size_t i = 0; i < residents.size() && heroes_.size() < 2; ++i) {
        add(residents[i].second);
    }
}

void Showrunner::beginEpisode(const ShowInput& in, ShowOrders& orders) {
    ++episode_;
    episodeClock_ = 0.0f;
    episodeTick_ = in.world->tick();
    scenes_ = 0;
    established_.clear();
    notes_.clear();
    last_ = {};
    resume_.reset();
    breatherDue_ = false;
    jumpedIdle_ = false;
    chooseHeroes(in);
    starring_.clear();
    for (const EntityId id : heroes_) {
        starring_ += (starring_.empty() ? "" : ", ") + nameOf(*in.world, id);
    }
    const CalendarTime time = calendarAt(in.world->tick());
    char day[64];
    std::snprintf(day, sizeof(day), "День %d · %02d:%02d", time.day, time.hour, time.minute);
    dayLine_ = day;
    lastDay_ = static_cast<std::uint64_t>(time.day);
    setPhase(ShowPhase::Opening);
    curtainTarget_ = 1.0f;
    orders.pause = true; // на титры мир стоит
}

void Showrunner::meet(const ShowInput& in) {
    const World& world = *in.world;
    const EntityId target = in.current.target;
    Vector2 center{};
    if (!positionOf(world, target, 1.0f, center)) {
        return;
    }
    if (hasName(world, target)) {
        known_.insert(target);
    }
    std::vector<EntityId> ids;
    collectIds(*in.stories, in.current, ids);
    if (const Story* story = in.stories->find(in.current.story); story != nullptr && story->kind == StoryKind::Group) {
        if (const entt::entity group = world.find(story->focus);
            group != entt::null && world.registry().all_of<Group>(group)) {
            const std::vector<EntityId>& members = world.registry().get<Group>(group).members;
            ids.insert(ids.end(), members.begin(), members.end());
        }
    }
    for (const EntityId id : ids) {
        Vector2 at{};
        if (id != kInvalidId && hasName(world, id) && positionOf(world, id, 1.0f, at) &&
            Vector2Distance(at, center) <= kKnownNear) {
            known_.insert(id);
        }
    }
}

StoryPick Showrunner::visitPick(const ShowInput& in) const {
    StoryPick pick;
    float oldest = std::numeric_limits<float>::infinity();
    // Сперва те, кто не спит.
    const bool anyAwake = std::any_of(heroes_.begin(), heroes_.end(), [&](EntityId id) {
        return isPersonAlive(*in.world, id) && !asleep(*in.world, id);
    });
    for (const EntityId id : heroes_) {
        if (!isPersonAlive(*in.world, id) || (anyAwake && asleep(*in.world, id))) {
            continue;
        }
        const auto it = lastShown_.find(id);
        const float shown = it != lastShown_.end() ? it->second : -std::numeric_limits<float>::infinity();
        if (shown < oldest || !pick.valid()) {
            oldest = shown;
            pick.visit = pick.target = pick.hero = id;
            pick.interest = kSceneDull;
        }
    }
    return pick;
}

void Showrunner::startScene(const ShowInput& in, const StoryPick& pick, ShowOrders& orders, bool calm, bool hurry) {
    const World& world = *in.world;
    scene_ = {};
    scene_.pick = pick;
    scene_.calm = calm;
    scene_.hurry = hurry;
    const Place place = describePlace(world, pick.target);
    scene_.place = place.line;
    scene_.building = place.building;
    scene_.at = place.at;
    const CalendarTime time = calendarAt(world.tick());
    if (static_cast<std::uint64_t>(time.day) != lastDay_) {
        char text[64];
        std::snprintf(text, sizeof(text), "День %d, %02d:%02d", time.day, time.hour, time.minute);
        scene_.time = text;
        lastDay_ = static_cast<std::uint64_t>(time.day);
    } else {
        scene_.time = capitalized(timeOfDay(time.hour));
    }
    scene_.wasAlive = isPersonAlive(world, pick.target);
    orders.show = pick;
    setPhase(ShowPhase::Intro);
    curtainTarget_ = 1.0f;
    dull_ = 0.0f;
    ++scenes_;
    last_ = pick;
    lastSceneTick_ = world.tick();
    if (pick.hasOmen &&
        std::none_of(established_.begin(), established_.end(),
                     [&](const Foresight& omen) { return omen.same(pick.omen); })) {
        established_.push_back(pick.omen);
    }
    if (pick.hasOmen && pick.omen.kind == ForesightKind::Raid) {
        seenHordes_[pick.omen.focus] = world.tick();
    }
    if (isHero(pick.target)) {
        lastShown_[pick.target] = clock_;
    }
}

void Showrunner::startCutaway(const ShowInput& in, const StoryPick& pick, std::string text, bool resume,
                              ShowOrders& orders) {
    if (resume) {
        resume_ = scene_;
    }
    scene_ = {};
    scene_.pick = pick;
    scene_.caption = std::move(text);
    orders.show = pick;
    setPhase(ShowPhase::Cutaway);
    curtainTarget_ = 0.0f;
    std::erase_if(ironies_, [this](const auto& shown) { return clock_ - shown.second >= kIronyRepeat; });
    ironies_.emplace_back(pick.omen, clock_);
    if (pick.omen.kind == ForesightKind::Raid) {
        seenHordes_[pick.omen.focus] = in.world->tick();
    }
    if (hasName(*in.world, pick.target)) {
        known_.insert(pick.target); // зритель его видел: теперь знаком
    }
}

void Showrunner::startJump(const ShowInput& in, ShowOrders& orders) {
    setPhase(ShowPhase::Jump);
    curtainTarget_ = 1.0f;
    jumpStart_ = in.world->tick();
    jumpReal_ = 0.0f;
    jumpCursor_ = in.world->chronicle().events().size();
    orders.skip = kJumpChunk;
}

bool Showrunner::wake(const ShowInput& in) {
    const World& world = *in.world;
    const std::vector<ChronicleEvent>& events = world.chronicle().events();
    bool woke = false;
    for (std::size_t i = jumpCursor_; i < events.size() && !woke; ++i) {
        const ChronicleEvent& event = events[i];
        for (const EntityId who : {event.subject, event.object, event.third}) {
            if (isKnown(who) && chronicleWeight(event, who) >= kWakeWeight) {
                woke = true; // с героем случилось важное
                break;
            }
        }
    }
    jumpCursor_ = events.size();
    if (woke) {
        return true;
    }
    for (const StoryPick& pick : *in.picks) {
        if (pick.hasOmen && pick.omen.eta <= kWakeEta && involves(in, pick, true)) {
            return true; // вот-вот кульминация у знакомых
        }
    }
    if (world.tick() - std::min(world.tick(), jumpStart_) < kJumpMin) {
        return false;
    }
    float value = 0.0f;
    best(in, value);
    return value >= kWakeScore;
}

void Showrunner::endScene(const ShowInput& in, bool hurry) {
    if (scene_.sawClimax && (in.lost || scene_.deadFor > 0.0f)) {
        breatherDue_ = true; // после кульминации — передышка
    }
    jumpedIdle_ = false;
    fadeHurry_ = hurry;
    next_ = Next::Choose;
    setPhase(ShowPhase::FadeOut);
    curtainTarget_ = 1.0f;
}

void Showrunner::startCliffhanger(const ShowInput& in, ShowOrders& orders) {
    const World& world = *in.world;
    // Что не решено у героев эпизода: сперва то, что вот-вот случится.
    const StoryPick* chosen = nullptr;
    float top = 0.0f;
    for (const StoryPick& pick : *in.picks) {
        if (pick.visit != kInvalidId || !involves(in, pick, false)) {
            continue;
        }
        const float value = pick.interest * (pick.hasOmen ? 2.0f : 1.0f);
        if (value > top) {
            top = value;
            chosen = &pick;
        }
    }
    cliff_ = chosen != nullptr ? *chosen : in.current;
    const Foresight& omen = cliff_.omen;
    const Story* story = in.stories->find(cliff_.story);
    if (cliff_.hasOmen) {
        switch (omen.kind) {
        case ForesightKind::Revenge:
            cliffLine_ = nameOf(world, omen.hero) + " всё ближе к " + nameOf(world, omen.other) +
                         ". Чем кончится месть?";
            break;
        case ForesightKind::Raid: {
            const entt::entity village = world.find(omen.other);
            const std::string name = village != entt::null && world.registry().all_of<Settlement>(village)
                                         ? world.registry().get<Settlement>(village).name
                                         : std::string("поселению");
            cliffLine_ = "Орда подходит к " + name + ".";
            if (const EntityId victim = knownIn(world, omen.other); victim != kInvalidId) {
                cliffLine_ += " Там " + nameOf(world, victim) + ".";
            }
            break;
        }
        case ForesightKind::Turning: {
            const entt::entity body = world.find(omen.hero);
            const bool dead = body != entt::null && world.registry().all_of<Corpse>(body);
            cliffLine_ = dead ? "Тело " + nameOf(world, omen.hero) + " вот-вот встанет."
                              : nameOf(world, omen.hero) + " вот-вот обратится, а рядом люди.";
            break;
        }
        case ForesightKind::CacheRace:
            cliffLine_ = nameOf(world, omen.hero) + " и " + nameOf(world, omen.other) +
                         " спешат к одному грузу. Кто успеет первым?";
            break;
        case ForesightKind::Theft:
            cliffLine_ = nameOf(world, omen.hero) + " " + intentLine(world, omen.hero) + ".";
            break;
        default:
            cliffLine_.clear();
            break;
        }
    } else if (story != nullptr && story->active()) {
        switch (story->kind) {
        case StoryKind::Feud:
            cliffLine_ = std::string(story->betrayal ? "Предательство" : "Вражда") + " между " +
                         nameOf(world, story->hero) + " и " + nameOf(world, story->other) +
                         " не окончена. Чем она кончится?";
            break;
        case StoryKind::Group: {
            const entt::entity group = world.find(story->focus);
            const EntityId leader = group != entt::null && world.registry().all_of<Group>(group)
                                        ? world.registry().get<Group>(group).leader
                                        : story->hero;
            cliffLine_ = "Что ждёт группу " + nameOf(world, leader) + "?";
            break;
        }
        case StoryKind::Infection:
            cliffLine_ = nameOf(world, story->hero) + (isFemale(world, story->hero) ? " укушена" : " укушен") +
                         ". Сколько осталось?";
            break;
        case StoryKind::Salvage:
            cliffLine_ = "Кому достанется груз?";
            break;
        default:
            cliffLine_ = frame::clean(titleOf(*story)) + ". Чем кончится?";
            break;
        }
    } else if (const std::string intent = intentLine(world, cliff_.target); !intent.empty()) {
        cliffLine_ = nameOf(world, cliff_.target) + " " + intent + ".";
    } else {
        cliffLine_.clear();
    }
    cliffLine_ = frame::clean(cliffLine_);
    if (cliff_.valid() && !cliff_.same(in.current)) {
        orders.show = cliff_;
    }
    setPhase(ShowPhase::Cliffhanger);
    curtainTarget_ = 0.0f;
}

void Showrunner::proceed(const ShowInput& in, ShowOrders& orders) {
    resume_.reset();
    StoryPick ironic;
    std::string text;
    if (irony(in, ironic, text)) {
        startCutaway(in, ironic, std::move(text), false, orders);
        return;
    }
    float value = 0.0f;
    const StoryPick* top = best(in, value);
    lastBest_ = value;
    lastPicks_ = in.picks->size();
    lastHeroPicks_ = static_cast<std::size_t>(std::count_if(in.picks->begin(), in.picks->end(), [&](const StoryPick& pick) {
        return pick.visit == kInvalidId && involves(in, pick, false);
    }));
    lastTopInterest_ = in.picks->empty() ? 0.0f : in.picks->front().interest;
    if (breatherDue_) {
        breatherDue_ = false;
        const StoryPick visit = visitPick(in);
        if (visit.valid() && !(top != nullptr && fadeHurry_)) {
            startScene(in, visit, orders, true, false);
            scene_.breather = true;
            return;
        }
    }
    if (top != nullptr && value >= kSceneWorthy) {
        const bool continuing = last_.valid() && top->same(last_);
        const bool hurry = (fadeHurry_ && top->hasOmen) || continuing;
        startScene(in, *top, orders, false, hurry);
        if (continuing) {
            // Та же сцена дальше: затемнение — склейка во времени, без титра места.
            setPhase(ShowPhase::Scene);
            curtainTarget_ = 0.0f;
        }
        return;
    }
    const bool allAsleep = std::none_of(heroes_.begin(), heroes_.end(), [&](EntityId id) {
        return isPersonAlive(*in.world, id) && !asleep(*in.world, id);
    });
    if ((!jumpedIdle_ || allAsleep) && !in.paused) {
        startJump(in, orders); // скучно — вырезать (и ночь, пока герои спят)
        return;
    }
    if (const StoryPick visit = visitPick(in); visit.valid()) {
        startScene(in, visit, orders, true, false); // быт героя
        return;
    }
    // Совсем некого: общий план поселения.
    startScene(in, top != nullptr ? *top : StoryPick{}, orders, true, false);
}

bool Showrunner::establishing() const {
    return (phase_ == ShowPhase::Intro || phase_ == ShowPhase::Establishing) && !scene_.hurry &&
           scene_.pick.valid();
}

ShotPlan Showrunner::placePlan(const World& world, float alpha) const {
    ShotPlan plan;
    const entt::registry& registry = world.registry();
    const entt::entity building = scene_.building != kInvalidId ? world.find(scene_.building) : entt::null;
    if (building != entt::null && registry.all_of<Building>(building)) {
        const Building& info = registry.get<Building>(building);
        const Vector2 center = centerOf(info);
        const float half = std::max(info.width, info.height) * Map::kCellSize * 0.5f + kPlaceMargin;
        plan.valid = true;
        plan.size = ShotSize::Wide;
        plan.subjectId = scene_.building;
        plan.subject = center;
        for (const Vector2 side : {Vector2{-1.0f, 0.0f}, Vector2{1.0f, 0.0f}, Vector2{0.0f, -1.0f}, Vector2{0.0f, 1.0f}}) {
            plan.extra.push_back(Vector2Add(center, Vector2Scale(side, half)));
        }
        const Vector2 door = doorOf(info);
        plan.viewFrom = std::atan2(door.x - center.x, door.y - center.y) * RAD2DEG;
        return plan;
    }
    Vector2 at{};
    if (!positionOf(world, scene_.pick.target, alpha, at)) {
        return plan;
    }
    plan.valid = true;
    plan.size = ShotSize::Wide;
    plan.subjectId = scene_.pick.target;
    plan.subject = at;
    for (const Vector2 side : {Vector2{-1.0f, 0.0f}, Vector2{1.0f, 0.0f}, Vector2{0.0f, -1.0f}, Vector2{0.0f, 1.0f}}) {
        plan.extra.push_back(Vector2Add(at, Vector2Scale(side, kPlaceRadius)));
    }
    return plan;
}

ShowOrders Showrunner::update(const ShowInput& in, float dt) {
    ShowOrders orders;
    if (phase_ == ShowPhase::Off || in.world == nullptr || in.picks == nullptr) {
        return orders;
    }
    clock_ += dt;
    phaseAge_ += dt;
    episodeClock_ += dt;
    // Затемнение и проявка.
    const float rate = curtainTarget_ > curtain_ ? dt / kFadeDown : dt / kFadeUp;
    curtain_ = curtainTarget_ > curtain_ ? std::min(curtainTarget_, curtain_ + rate)
                                         : std::max(curtainTarget_, curtain_ - rate);
    if (begin_) {
        begin_ = false;
        beginEpisode(in, orders);
        return orders;
    }
    const World& world = *in.world;
    switch (phase_) {
    case ShowPhase::Opening: {
        const float length = recap_.empty() ? kOpeningCard : kRecapTime + kOpeningCard;
        if (phaseAge_ >= length) {
            orders.pause = false;
            recap_.clear();
            proceed(in, orders);
        }
        break;
    }
    case ShowPhase::FadeOut:
        if (curtain_ >= 1.0f) {
            if (next_ == Next::Closing) {
                // Конец эпизода: «ранее в сериях» для следующего — итоги сцен и клиффхэнгер.
                recap_.clear();
                const std::size_t from = notes_.size() > kRecapLines - 1 ? notes_.size() - (kRecapLines - 1) : 0;
                recap_.assign(notes_.begin() + static_cast<std::ptrdiff_t>(from), notes_.end());
                if (recap_.empty()) {
                    for (const EntityId id : heroes_) {
                        for (const std::uint32_t index :
                             notableEvents(world, id, episodeTick_, world.tick() + 1, 1)) {
                            recap_.push_back(frame::clean(chronicleEventText(world.chronicle().events()[index], kInvalidId)));
                        }
                    }
                }
                if (!cliffLine_.empty()) {
                    recap_.push_back(cliffLine_);
                }
                setPhase(ShowPhase::Closing);
                orders.pause = true;
            } else {
                proceed(in, orders);
            }
        }
        break;
    case ShowPhase::Jump: {
        jumpReal_ += dt;
        const CalendarTime time = calendarAt(world.tick());
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", time.hour, time.minute);
        clockLine_ = clock;
        if (in.skipping) {
            break; // порция промотки ещё идёт
        }
        const std::uint64_t elapsed = world.tick() - std::min(world.tick(), jumpStart_);
        const bool woke = wake(in);
        if (woke || elapsed >= kJumpMax || jumpReal_ >= kJumpReal || in.paused) {
            jumpText_ = jumpPhrase(jumpStart_, world.tick());
            jumpedIdle_ = !woke;
            proceed(in, orders);
        } else {
            orders.skip = kJumpChunk;
        }
        break;
    }
    case ShowPhase::Intro: {
        const float length = scene_.hurry ? kIntroHurry : jumpText_.empty() ? kIntro : kIntroJump;
        if (phaseAge_ >= length) {
            jumpText_.clear();
            curtainTarget_ = 0.0f;
            if (establishing()) {
                setPhase(ShowPhase::Establishing);
            } else {
                setPhase(ShowPhase::Scene);
            }
        }
        break;
    }
    case ShowPhase::Establishing:
        if (phaseAge_ >= kEstablish) {
            setPhase(ShowPhase::Scene);
            orders.recut = true; // общий план → ближе
        }
        break;
    case ShowPhase::Scene: {
        Scene& scene = scene_;
        scene.age += dt;
        scene.sawClimax = scene.sawClimax || in.climax;
        if (scene.wasAlive && !isPersonAlive(world, in.current.target)) {
            scene.deadFor += dt;
        }
        const bool waiting = in.current.hasOmen && !in.lost && in.current.omen.eta > kRipeEta;
        const bool dull = !in.climax && (in.currentInterest < kSceneDull || in.quiet || waiting ||
                                         (!in.current.hasOmen && asleep(world, in.current.target)));
        dull_ = dull ? dull_ + dt : 0.0f;
        if (in.refreshed) {
            meet(in);
        }
        const float minLength = scene.calm ? kCalmMin : kSceneMin;
        const float maxLength = scene.calm ? kCalmMax : kSceneMax;
        bool over = false;
        bool hurry = false;
        if (in.climax && scene.age < kSceneMax * kClimaxHold) {
            over = false; // кульминацию досматривают
        } else if (scene.deadFor >= kAftermath && scene.age >= kCalmMin) {
            over = true;
        } else if (in.lost && in.lostFor >= kAftermath && scene.age >= kCalmMin) {
            over = true;
        } else if (scene.age >= maxLength) {
            over = true;
        } else if (scene.age >= minLength) {
            float value = 0.0f;
            if (dull_ >= kDullFor || episodeClock_ >= kEpisodeMax) {
                over = true;
            } else if (in.refreshed && urgentElsewhere(in)) {
                over = true;
                hurry = true; // к кульминации линии эпизода
            } else if (in.refreshed && scene.calm && (best(in, value), value >= kWakeScore)) {
                over = true; // быт уступает тому, что началось
            } else if (in.refreshed) {
                StoryPick ironic;
                std::string text;
                if (irony(in, ironic, text)) {
                    startCutaway(in, ironic, std::move(text), true, orders);
                    break;
                }
            }
        }
        if (over) {
            if (!hurry && episodeClock_ >= kEpisodeMin) {
                startCliffhanger(in, orders);
            } else {
                endScene(in, hurry);
            }
        }
        break;
    }
    case ShowPhase::Cutaway:
        if (phaseAge_ >= kCutaway) {
            if (resume_ && world.find(resume_->pick.target) != entt::null) {
                scene_ = *resume_; // назад к сцене — прямой склейкой
                resume_.reset();
                orders.show = scene_.pick;
                setPhase(ShowPhase::Scene);
                dull_ = 0.0f;
            } else {
                resume_.reset();
                endScene(in, false);
            }
        }
        break;
    case ShowPhase::Cliffhanger:
        if (phaseAge_ >= kCliffhanger) {
            next_ = Next::Closing;
            setPhase(ShowPhase::FadeOut);
            curtainTarget_ = 1.0f;
        }
        break;
    case ShowPhase::Closing:
        if (phaseAge_ >= kClosing) {
            beginEpisode(in, orders);
        }
        break;
    default:
        break;
    }
    return orders;
}

bool Showrunner::hidesTitles() const {
    switch (phase_) {
    case ShowPhase::Opening:
    case ShowPhase::Jump:
    case ShowPhase::Intro:
    case ShowPhase::Establishing:
    case ShowPhase::Cutaway:
    case ShowPhase::Cliffhanger:
    case ShowPhase::Closing:
        return true;
    case ShowPhase::FadeOut:
        return curtain_ > 0.5f;
    default:
        return false;
    }
}

std::string Showrunner::label() const {
    switch (phase_) {
    case ShowPhase::Cutaway:
        return "Тем временем…";
    case ShowPhase::Cliffhanger:
        return "Конец эпизода";
    case ShowPhase::Scene:
        if (scene_.breather) {
            return "Передышка";
        }
        return scene_.calm && scene_.age < 6.0f ? "Будни" : "";
    default:
        return "";
    }
}

std::string Showrunner::status() const {
    char text[192];
    std::snprintf(text, sizeof(text), "эпизод %d (%.0f с), сцена %d — %s; героев %zu, знакомых %zu",
                  episode_, episodeClock_, scenes_, showPhaseTitle(phase_), heroes_.size(), known_.size());
    std::string line = text;
    std::snprintf(text, sizeof(text), "; кандидатов %zu (с героями %zu, лучший %.2f), оценка сцены %.2f",
                  lastPicks_, lastHeroPicks_, lastTopInterest_, lastBest_);
    line += text;
    if (phase_ == ShowPhase::Intro && !jumpText_.empty()) {
        line += "; " + jumpText_;
    }
    if (phase_ == ShowPhase::Intro && !scene_.place.empty()) {
        line += "; " + scene_.place + ". " + scene_.time;
    }
    if (phase_ == ShowPhase::Cutaway || phase_ == ShowPhase::Cliffhanger) {
        line += "; " + (phase_ == ShowPhase::Cutaway ? scene_.caption : cliffLine_);
    }
    return line;
}

void Showrunner::draw(float bar) const {
    using frame::Align;
    if (phase_ == ShowPhase::Off) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = frame::scale();
    const float cx = display.x * 0.5f;
    const float cy = display.y * 0.5f;
    const float wrap = display.x * 0.62f;
    const ImVec4 white{0.95f, 0.94f, 0.9f, 1.0f};
    const ImVec4 muted{0.7f, 0.74f, 0.8f, 1.0f};
    const ImVec4 gold{1.0f, 0.82f, 0.42f, 1.0f};
    const auto tint = [](ImVec4 color, float alpha) {
        color.w *= std::clamp(alpha, 0.0f, 1.0f);
        return color;
    };
    switch (phase_) {
    case ShowPhase::Opening: {
        float age = phaseAge_;
        if (!recap_.empty()) {
            if (age < kRecapTime) {
                // «Ранее в сериях»: итоги прошлого эпизода и чем он кончился.
                const float alpha = frame::fade(age, 0.6f, kRecapTime, 0.8f);
                float total = frame::measure(italicFont(), 24.0f * scale, "Ранее в сериях") + 18.0f * scale;
                for (const std::string& line : recap_) {
                    total += frame::measure(captionFont(), 23.0f * scale, line, wrap) + 8.0f * scale;
                }
                float y = cy - total * 0.5f;
                y += frame::text(italicFont(), 24.0f * scale, cx, y, Align::Center, tint(muted, alpha),
                                 "Ранее в сериях") + 18.0f * scale;
                for (const std::string& line : recap_) {
                    y += frame::text(captionFont(), 23.0f * scale, cx, y, Align::Center, tint(white, alpha), line,
                                     wrap) + 8.0f * scale;
                }
                break;
            }
            age -= kRecapTime;
        }
        const float alpha = frame::fade(age, 0.8f, kOpeningCard, 0.7f);
        char title[32];
        std::snprintf(title, sizeof(title), "Эпизод %d", episode_);
        float y = cy - 70.0f * scale;
        y += frame::text(titleFont(), 58.0f * scale, cx, y, Align::Center, tint(white, alpha), title) + 14.0f * scale;
        y += frame::text(captionFont(), 22.0f * scale, cx, y, Align::Center, tint(muted, alpha), dayLine_) + 10.0f * scale;
        if (!starring_.empty()) {
            const float late = frame::fade(age - 0.8f, 0.8f, kOpeningCard - 0.8f, 0.7f);
            frame::text(italicFont(), 22.0f * scale, cx, y, Align::Center, tint(gold, late),
                        "В главных ролях: " + starring_, wrap);
        }
        break;
    }
    case ShowPhase::Jump:
        // Время идёт: только часы.
        frame::text(italicFont(), 26.0f * scale, cx, cy - 14.0f * scale, Align::Center,
                    tint(muted, 0.55f * frame::fade(phaseAge_, 0.5f)), clockLine_);
        break;
    case ShowPhase::Intro: {
        // Вводный титр: «Три часа спустя», место и время.
        const float length = scene_.hurry ? kIntroHurry : jumpText_.empty() ? kIntro : kIntroJump;
        const float alpha = frame::fade(phaseAge_, 0.5f, length + 0.4f, 0.6f);
        float y = cy - 40.0f * scale;
        if (!jumpText_.empty()) {
            frame::text(italicFont(), 30.0f * scale, cx, y - 64.0f * scale, Align::Center, tint(muted, alpha), jumpText_);
        }
        if (!scene_.place.empty()) {
            const float late = jumpText_.empty() ? alpha : frame::fade(phaseAge_ - 0.9f, 0.5f, length - 0.5f, 0.6f);
            y += frame::text(titleFont(), 36.0f * scale, cx, y, Align::Center, tint(white, late), scene_.place, wrap) +
                 8.0f * scale;
            frame::text(captionFont(), 24.0f * scale, cx, y, Align::Center, tint(muted, late), scene_.time);
        }
        break;
    }
    case ShowPhase::Establishing: {
        // На общем плане место и время остаются внизу.
        const float alpha = frame::fade(phaseAge_, 0.3f, kEstablish, 1.0f);
        const float margin = display.x * 0.07f;
        float y = display.y - bar + bar * 0.14f;
        y += frame::text(titleFont(), 32.0f * scale, margin, y, Align::Left, tint(white, alpha), scene_.place,
                         display.x - 2.0f * margin) + 2.0f * scale;
        frame::text(captionFont(), 21.0f * scale, margin, y, Align::Left, tint(muted, alpha), scene_.time);
        break;
    }
    case ShowPhase::Cutaway: {
        // Драматическая ирония: зритель знает то, чего не знает герой.
        const float alpha = frame::fade(phaseAge_, 0.6f, kCutaway, 0.8f);
        const float y = display.y - bar - 30.0f * scale -
                        frame::measure(titleFont(), 25.0f * scale, scene_.caption, wrap);
        frame::text(titleFont(), 25.0f * scale, cx, y, Align::Center, tint(white, alpha), scene_.caption, wrap);
        break;
    }
    case ShowPhase::Cliffhanger: {
        const float alpha = frame::fade(phaseAge_, 0.8f, kCliffhanger, 0.6f);
        float y = display.y - bar - 40.0f * scale -
                  frame::measure(titleFont(), 26.0f * scale, cliffLine_, wrap) - 34.0f * scale;
        if (!cliffLine_.empty()) {
            y += frame::text(titleFont(), 26.0f * scale, cx, y, Align::Center, tint(white, alpha), cliffLine_, wrap) +
                 8.0f * scale;
        }
        const float late = frame::fade(phaseAge_ - 2.0f, 0.8f, kCliffhanger - 2.0f, 0.6f);
        frame::text(italicFont(), 24.0f * scale, cx, y, Align::Center, tint(gold, late), "Продолжение следует…");
        break;
    }
    case ShowPhase::Closing: {
        const float alpha = frame::fade(phaseAge_, 0.6f, kClosing, 0.6f);
        char title[48];
        std::snprintf(title, sizeof(title), "Конец эпизода %d", episode_);
        frame::text(titleFont(), 44.0f * scale, cx, cy - 26.0f * scale, Align::Center, tint(white, alpha), title);
        break;
    }
    default:
        break;
    }
}

} // namespace lw::ui
