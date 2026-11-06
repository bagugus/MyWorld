#include "render/InteriorStage.h"

#include "render/Terrain3D.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace lw {

namespace {

std::uint64_t mixBits(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ULL;
    value ^= value >> 33;
    return value;
}

std::uint64_t hashText(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : text) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return hash;
}

// Шаг по комнате, м/с симуляции; на ускоренном времени — быстрее, но не
// больше чем во столько раз.
constexpr float kIndoorWalk = 1.1f;
constexpr float kIndoorRateMax = 6.0f;
// Перед проёмом и за ним — точки пути на таком расстоянии от двери, метры.
constexpr float kDoorStep = 0.45f;

// Что делает человек дома — как его усадить.
Occupation occupationOf(const entt::registry& registry, entt::entity entity, const Person& person) {
    if (person.asleep) {
        return Occupation::Sleep;
    }
    const auto* mind = registry.try_get<Mind>(entity);
    if (mind == nullptr) {
        return Occupation::Other;
    }
    switch (mind->action) {
    case Action::Sleep:
        return Occupation::Sleep;
    case Action::Eat:
    case Action::Drink:
        return Occupation::Eat;
    case Action::Idle:
        return Occupation::Rest;
    case Action::Treat:
        return Occupation::Treat;
    case Action::Socialize:
    case Action::Share:
    case Action::Trade:
        return Occupation::Talk;
    default:
        return Occupation::Other;
    }
}

} // namespace

BuildingModel buildingModelOf(const Building& building, EntityId id, const Terrain3D& terrain) {
    BuildingModel model;
    model.kind = building.kind;
    model.x0 = static_cast<float>(building.origin.x) * Map::kCellSize;
    model.z0 = static_cast<float>(building.origin.y) * Map::kCellSize;
    model.x1 = model.x0 + static_cast<float>(building.width) * Map::kCellSize;
    model.z1 = model.z0 + static_cast<float>(building.height) * Map::kCellSize;
    // Стоит на нижней точке участка, пол — над верхней: рельеф (сетка по углам
    // клеток) не прорастает сквозь пол комнат (12а.14).
    model.low = terrain.groundAt(model.x0, model.z0);
    model.high = model.low;
    for (int y = 0; y <= building.height; ++y) {
        for (int x = 0; x <= building.width; ++x) {
            const float ground = terrain.groundAt(model.x0 + static_cast<float>(x) * Map::kCellSize,
                                                  model.z0 + static_cast<float>(y) * Map::kCellSize);
            model.low = std::min(model.low, ground);
            model.high = std::max(model.high, ground);
        }
    }
    // Дверь — на стене напротив клетки входа.
    const float doorX = Map::cellCenter(building.door.x);
    const float doorZ = Map::cellCenter(building.door.y);
    model.door = {std::clamp(doorX, model.x0, model.x1), std::clamp(doorZ, model.z0, model.z1)};
    model.doorGround = terrain.groundAt(doorX, doorZ);
    model.seed = mixBits(id);
    return model;
}

void InteriorStage::open(EntityId building, EntityId focus) {
    building_ = building;
    focus_ = focus;
}

void InteriorStage::close() {
    building_ = kInvalidId;
    focus_ = kInvalidId;
    placements_.clear();
    index_.clear();
    walkers_.clear();
}

void InteriorStage::indexDoors() {
    doors_.clear();
    if (!layout_) {
        return;
    }
    for (const InteriorWall& wall : layout_->walls) {
        if (wall.outer) {
            continue;
        }
        const float across = (wall.lo + wall.hi) * 0.5f;
        const float side = (wall.hi - wall.lo) * 0.5f + 0.3f;
        for (const WallGap& gap : wall.gaps) {
            if (gap.window || gap.y0 > 0.05f) {
                continue;
            }
            Doorway door;
            door.alongX = wall.alongX;
            const float along = (gap.a0 + gap.a1) * 0.5f;
            if (wall.alongX) {
                door.x = along;
                door.z = across;
                door.a = layout_->roomAt(along, across - side);
                door.b = layout_->roomAt(along, across + side);
            } else {
                door.x = across;
                door.z = along;
                door.a = layout_->roomAt(across - side, along);
                door.b = layout_->roomAt(across + side, along);
            }
            if (door.a >= 0 && door.b >= 0 && door.a != door.b) {
                doors_.push_back(door);
            }
        }
    }
}

std::vector<std::pair<float, float>> InteriorStage::route(float x0, float z0, float x1, float z1) const {
    std::vector<std::pair<float, float>> path;
    const int from = layout_ ? layout_->roomAt(x0, z0) : -1;
    const int to = layout_ ? layout_->roomAt(x1, z1) : -1;
    if (from >= 0 && to >= 0 && from != to) {
        // Поиск в ширину по комнатам: через какие проёмы идти.
        const std::size_t rooms = layout_->rooms.size();
        std::vector<int> via(rooms, -1); // каким проёмом вошли в комнату
        std::vector<bool> seen(rooms, false);
        std::deque<int> queue{from};
        seen[static_cast<std::size_t>(from)] = true;
        while (!queue.empty() && !seen[static_cast<std::size_t>(to)]) {
            const int room = queue.front();
            queue.pop_front();
            for (std::size_t i = 0; i < doors_.size(); ++i) {
                const Doorway& door = doors_[i];
                const int next = door.a == room ? door.b : door.b == room ? door.a : -1;
                if (next < 0 || seen[static_cast<std::size_t>(next)]) {
                    continue;
                }
                seen[static_cast<std::size_t>(next)] = true;
                via[static_cast<std::size_t>(next)] = static_cast<int>(i);
                queue.push_back(next);
            }
        }
        if (seen[static_cast<std::size_t>(to)]) {
            // С конца: в каждую комнату room вошли проёмом via[room] из prev —
            // шаг перед проёмом (в prev), сам проём, шаг за ним (в room).
            std::vector<std::pair<float, float>> back;
            for (int room = to; room != from;) {
                const Doorway& door = doors_[static_cast<std::size_t>(via[static_cast<std::size_t>(room)])];
                const int prev = door.a == room ? door.b : door.a;
                const float sign = door.b == room ? 1.0f : -1.0f; // b — с большей стороны стены
                const float dx = door.alongX ? 0.0f : kDoorStep * sign;
                const float dz = door.alongX ? kDoorStep * sign : 0.0f;
                back.emplace_back(door.x + dx, door.z + dz);
                back.emplace_back(door.x, door.z);
                back.emplace_back(door.x - dx, door.z - dz);
                room = prev;
            }
            path.assign(back.rbegin(), back.rend());
        }
    }
    path.emplace_back(x1, z1);
    return path;
}

std::pair<float, float> InteriorStage::entrance() const {
    const InteriorLayout& layout = *layout_;
    const float cx = (layout.x0 + layout.x1) * 0.5f;
    const float cz = (layout.z0 + layout.z1) * 0.5f;
    for (const InteriorRoom& room : layout.rooms) {
        if (room.kind == RoomKind::Stairs) {
            return {(room.x0 + room.x1) * 0.5f, (room.z0 + room.z1) * 0.5f}; // поднялся по лестнице
        }
    }
    for (const InteriorWall& wall : layout.walls) {
        if (!wall.outer) {
            continue;
        }
        for (const WallGap& gap : wall.gaps) {
            if (gap.window || gap.y0 > 0.05f) {
                continue;
            }
            // Входная дверь: шаг внутрь от неё.
            const float along = (gap.a0 + gap.a1) * 0.5f;
            const float across = (wall.lo + wall.hi) * 0.5f;
            if (wall.alongX) {
                return {along, across + (cz > across ? 0.7f : -0.7f)};
            }
            return {across + (cx > across ? 0.7f : -0.7f), along};
        }
    }
    for (const InteriorRoom& room : layout.rooms) {
        if (room.kind == RoomKind::Hall) {
            return {(room.x0 + room.x1) * 0.5f, (room.z0 + room.z1) * 0.5f};
        }
    }
    return {cx, cz};
}

int InteriorStage::flatOf(const World& world, entt::entity person, EntityId building, int totalFlats) {
    if (totalFlats <= 1) {
        return 0;
    }
    const entt::registry& registry = world.registry();
    std::uint64_t key = registry.get<Id>(person).value;
    // Группа на своей базе живёт вместе, остальные — семьями (по фамилии).
    const auto& info = registry.get<Person>(person);
    const entt::entity group = info.group != kInvalidId ? world.find(info.group) : entt::null;
    if (group != entt::null && registry.all_of<Group>(group) && registry.get<Group>(group).base == building) {
        key = info.group ^ 0x6A09E667F3BCC908ULL;
    } else if (const auto* name = registry.try_get<Name>(person)) {
        key = hashText(name->last);
    }
    return static_cast<int>(mixBits(key * 0x9E3779B97F4A7C15ULL ^ building) % static_cast<std::uint64_t>(totalFlats));
}

void InteriorStage::update(const World& world, float dt) {
    placements_.clear();
    index_.clear();
    ++frame_;
    // Скорость времени — по тикам за кадр: на паузе в комнате никто не ходит.
    const std::uint64_t tick = world.tick();
    if (dt > 0.0f) {
        const float instant = static_cast<float>(tick - std::min(tick, lastTick_)) /
                              static_cast<float>(kTicksPerSecond) / dt;
        rate_ += (std::min(instant, 200.0f) - rate_) * std::min(1.0f, dt * 4.0f);
    }
    lastTick_ = tick;
    if (building_ == kInvalidId) {
        return;
    }
    const entt::registry& registry = world.registry();
    const entt::entity entity = world.find(building_);
    if (entity == entt::null || !registry.all_of<Building>(entity)) {
        close();
        return;
    }
    const Building& building = registry.get<Building>(entity);
    const BuildingModel model = buildingModelOf(building, building_, terrain_);
    const BuildingShell shell = buildingShell(model);
    const int perLevel = flatsPerLevel(building.kind, model.x0, model.z0, model.x1, model.z1);
    const int levels = livingLevels(building.kind, shell.storeys);
    const int total = perLevel * levels;
    int level = 0;
    if (const entt::entity focus = world.find(focus_); focus != entt::null && registry.all_of<Person>(focus) &&
                                                       registry.get<Person>(focus).home == building_) {
        level = flatOf(world, focus, building_, total) / perLevel;
    }
    if (!layout_ || layoutBuilding_ != building_ || layoutLevel_ != level) {
        // Кроватей — по числу жильцов квартиры (всех, а не только тех, кто дома).
        std::vector<int> residents(static_cast<std::size_t>(perLevel), 0);
        for (const auto [other, person] : registry.view<const Person>().each()) {
            if (person.home != building_) {
                continue;
            }
            const int flat = flatOf(world, other, building_, total);
            if (flat / perLevel == level) {
                ++residents[static_cast<std::size_t>(flat % perLevel)];
            }
        }
        InteriorInput input;
        input.kind = building.kind;
        input.x0 = model.x0;
        input.z0 = model.z0;
        input.x1 = model.x1;
        input.z1 = model.z1;
        input.floor = shell.floor;
        input.storey = shell.storey;
        input.storeys = shell.storeys;
        input.doorX = model.door.x;
        input.doorZ = model.door.y;
        input.seed = model.seed;
        input.openings = buildingOpenings(model);
        input.level = level;
        input.residents = std::move(residents);
        layout_ = planInterior(input);
        layoutBuilding_ = building_;
        layoutLevel_ = level;
        ++version_;
        indexDoors();
    }
    std::vector<Occupant> occupants;
    for (const auto [other, person, id] : registry.view<const Person, const Id>().each()) {
        if (!person.indoors || person.home != building_) {
            continue;
        }
        const int flat = flatOf(world, other, building_, total);
        if (flat / perLevel != level) {
            continue;
        }
        occupants.push_back({id.value, flat % perLevel, occupationOf(registry, other, person)});
    }
    placements_ = placeOccupants(*layout_, std::move(occupants));
    // Переходы: сменил место — идёт к нему, а не появляется там.
    const bool fresh = walkedVersion_ != version_ || dt <= 0.0f;
    if (walkedVersion_ != version_) {
        walkers_.clear(); // новый дом или этаж — все сразу на местах
        walkedVersion_ = version_;
    }
    const float step = kIndoorWalk * std::clamp(rate_, 0.0f, kIndoorRateMax) * dt;
    for (Placement& p : placements_) {
        auto [it, inserted] = walkers_.try_emplace(p.id);
        Walker& w = it->second;
        w.seen = frame_;
        if (fresh) {
            w = Walker{};
            w.seen = frame_;
            w.x = w.goalX = p.x;
            w.z = w.goalZ = p.z;
            w.heading = p.heading;
            w.spot = w.stage = p.spot;
        } else if (inserted) {
            // Только что вошёл: от входа к своему месту.
            const auto [ex, ez] = entrance();
            w.x = ex;
            w.z = ez;
            w.goalX = ex;
            w.goalZ = ez;
            w.heading = p.heading;
            w.spot = -3; // ещё никуда не шёл
            w.stage = p.spot;
        }
        if (w.spot != p.spot || std::hypot(w.goalX - p.x, w.goalZ - p.z) > 0.05f) {
            // Новое место: путь от того, где он сейчас; камера — на прежнем, пока идёт.
            if (w.spot != -3 && w.path.empty()) {
                w.stage = w.spot;
            }
            w.spot = p.spot;
            w.goalX = p.x;
            w.goalZ = p.z;
            w.path = route(w.x, w.z, p.x, p.z);
        }
        float left = step;
        while (!w.path.empty() && left > 0.0f) {
            const auto [tx, tz] = w.path.front();
            const float dx = tx - w.x;
            const float dz = tz - w.z;
            const float d = std::hypot(dx, dz);
            if (d > 1e-3f) {
                w.heading = std::atan2(dx, dz);
            }
            if (d <= left) {
                w.x = tx;
                w.z = tz;
                left -= d;
                w.path.erase(w.path.begin());
            } else {
                w.x += dx / d * left;
                w.z += dz / d * left;
                left = 0.0f;
            }
        }
        if (w.path.empty()) {
            w.stage = p.spot; // дошёл
            p.stage = p.spot;
            continue;
        }
        // В пути: стоя, лицом по ходу.
        const auto [tx, tz] = w.path.front();
        const float d = std::max(std::hypot(tx - w.x, tz - w.z), 1e-3f);
        p.walking = true;
        p.x = w.x;
        p.z = w.z;
        p.heading = w.heading;
        p.vx = (tx - w.x) / d * kIndoorWalk;
        p.vz = (tz - w.z) / d * kIndoorWalk;
        p.kind = SpotKind::Stand;
        p.height = 0.0f;
        p.meal = false;
        p.what = Occupation::Other;
        p.stage = w.stage;
        if (const int room = layout_->roomAt(w.x, w.z); room >= 0) {
            p.room = room;
        }
    }
    std::erase_if(walkers_, [this](const auto& entry) { return entry.second.seen != frame_; });
    for (std::size_t i = 0; i < placements_.size(); ++i) {
        index_[placements_[i].id] = i;
    }
}

const Placement* InteriorStage::placementOf(EntityId id) const {
    if (!active()) {
        return nullptr;
    }
    const auto it = index_.find(id);
    return it != index_.end() ? &placements_[it->second] : nullptr;
}

} // namespace lw
