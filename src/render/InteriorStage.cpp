#include "render/InteriorStage.h"

#include "render/Terrain3D.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/World.h"

#include <algorithm>

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

void InteriorStage::update(const World& world) {
    placements_.clear();
    index_.clear();
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
