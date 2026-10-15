#include "sim/systems/ResourceSystem.h"

#include "sim/Calendar.h"
#include "sim/Climate.h"
#include "sim/World.h"

#include <algorithm>

namespace lw {

namespace {

// Слабее этого — морось: в бочку почти ничего не попадает.
constexpr float kRainThreshold = 0.05f;

} // namespace

std::uint16_t rainBarrel(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return 4;
    case BuildingKind::Apartment:
        return 10; // бочки на всех жильцов
    case BuildingKind::Barn:
        return 2;
    case BuildingKind::Shop:
        return 0; // воду в магазине только завезённую
    }
    return 0;
}

void updateResources(World& world) {
    if ((world.tick() + kStartGameTick) % kTicksPerGameMinute != 0) {
        return;
    }
    const CalendarTime time = calendarAt(world.tick());
    Resources& resources = world.resources();
    if (time.minuteOfDay == 0) {
        resources.growForest(time.season, time.dayOfSeason);
        // Мясо портится: за сутки от любого запаса остаётся половина.
        for (const auto [entity, inventory] : world.registry().view<Inventory>().each()) {
            inventory.set(ItemKind::Meat, inventory.count(ItemKind::Meat) / 2);
        }
    }

    const Weather& weather = world.weather();
    if (weather.precipitation <= kRainThreshold ||
        isSnowing(seaLevelTemperature(time, weather))) {
        return;
    }
    const int portions = resources.collectRain(weather.precipitation, 1.0f);
    if (portions == 0) {
        return;
    }
    // Бочки независимы друг от друга, поэтому порядок обхода не важен.
    auto view = world.registry().view<const Building, Inventory>();
    for (const auto [entity, building, inventory] : view.each()) {
        const std::uint16_t barrel = rainBarrel(building.kind);
        if (inventory.count(ItemKind::Water) < barrel) {
            inventory.set(ItemKind::Water,
                          std::min<int>(barrel, inventory.count(ItemKind::Water) + portions));
        }
    }
}

} // namespace lw
