#include "sim/systems/WorkSystem.h"

#include "sim/Scenario.h"
#include "sim/Work.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Завоз: до скольких единиц пополняются запасы (больше — не трогают).
struct Stock {
    int food, water, bandages, medicine;
};
constexpr Stock kTownShopStock = {40, 20, 4, 2};
constexpr Stock kVillageShopStock = {25, 12, 2, 1};
constexpr Stock kHospitalStock = {10, 15, 20, 12};

void topUp(Inventory& inventory, ItemKind kind, int level) {
    if (inventory.count(kind) < level) {
        inventory.set(kind, level);
    }
}

// Завоз в магазины и больницы, где ещё работают (этап 7.4: бросили работу —
// не возят): до supply от обычного запаса (в панике — перебои). Деревенский
// магазин берёт еду сперва с ферм своей деревни — урожай, — остальное привозят.
void restock(World& world, float supply) {
    const auto share = [supply](int level) {
        return static_cast<int>(std::lround(static_cast<float>(level) * supply));
    };
    const std::vector<EntityId> manned = mannedWorkplaces(world);
    entt::registry& registry = world.registry();
    for (const Places::SettlementPlace& place : world.places().settlements()) {
        const bool town =
            registry.get<Settlement>(world.find(place.id)).kind == SettlementKind::Town;
        for (const Places::BuildingPlace& building : place.buildings) {
            const entt::entity entity = world.find(building.id);
            auto* workplace = registry.try_get<Workplace>(entity);
            auto* inventory = registry.try_get<Inventory>(entity);
            if (workplace == nullptr || inventory == nullptr ||
                !std::binary_search(manned.begin(), manned.end(), building.id)) {
                continue;
            }
            if (building.kind == BuildingKind::Hospital) {
                const Stock& stock = kHospitalStock;
                topUp(*inventory, ItemKind::Food, share(stock.food));
                topUp(*inventory, ItemKind::Water, share(stock.water));
                topUp(*inventory, ItemKind::Bandage, share(stock.bandages));
                topUp(*inventory, ItemKind::Medicine, share(stock.medicine));
                continue;
            }
            if (building.kind != BuildingKind::Shop) {
                continue;
            }
            const Stock& stock = town ? kTownShopStock : kVillageShopStock;
            const int food = share(stock.food);
            if (!town) {
                for (const Places::BuildingPlace& farm : place.buildings) {
                    if (farm.kind != BuildingKind::Barn || inventory->count(ItemKind::Food) >= food) {
                        continue;
                    }
                    if (auto* harvest = registry.try_get<Inventory>(world.find(farm.id))) {
                        inventory->add(ItemKind::Food,
                                       harvest->take(ItemKind::Food,
                                                     food - inventory->count(ItemKind::Food)));
                    }
                }
            }
            topUp(*inventory, ItemKind::Food, food);
            topUp(*inventory, ItemKind::Water, share(stock.water));
            topUp(*inventory, ItemKind::Bandage, share(stock.bandages));
            topUp(*inventory, ItemKind::Medicine, share(stock.medicine));
            workplace->till = 0; // выручку сдали поставщику
        }
    }
}

} // namespace

float harvestPerHour(Season season) {
    switch (season) {
    case Season::Spring:
        return 0.3f; // сев
    case Season::Summer:
        return 0.8f;
    case Season::Autumn:
        return 1.2f; // уборка
    case Season::Winter:
        return 0.1f; // скотина, погреб
    }
    return 0.0f;
}

std::uint16_t tapWater(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return 4;
    case BuildingKind::Apartment:
        return 10;
    default:
        return 0;
    }
}

void updateWork(World& world) {
    const std::uint64_t tick = world.tick();
    if (tick % kTicksPerGameMinute != 0) {
        return;
    }
    entt::registry& registry = world.registry();
    const CalendarTime time = calendarAt(tick);
    const bool hourly = time.minute == 0;
    // Что ещё работает — по фазе мира (этап 7.4).
    const PhaseRules& rules = rulesOf(world);

    // Кто на месте. Сложение не зависит от порядка обхода.
    for (const auto [entity, workplace] : registry.view<Workplace>().each()) {
        workplace.onDuty = 0;
    }
    for (const auto [entity, person, job] : registry.view<const Person, const Job>().each()) {
        if (!isOnDuty(world, entity)) {
            continue;
        }
        if (auto* workplace = registry.try_get<Workplace>(world.find(job.workplace))) {
            workplace->onDuty = static_cast<std::uint8_t>(std::min(255, workplace->onDuty + 1));
        }
        if (hourly && rules.economy) {
            if (auto* wallet = registry.try_get<Wallet>(entity)) {
                wallet->money += professionSpec(job.profession).wage;
            }
        }
    }
    if (!hourly) {
        return;
    }

    // Урожай: каждый фермер на месте растит свою долю.
    for (const auto [entity, building, workplace, inventory] :
         registry.view<const Building, Workplace, Inventory>().each()) {
        if (building.kind != BuildingKind::Barn || workplace.onDuty == 0) {
            continue;
        }
        workplace.harvest += static_cast<float>(workplace.onDuty) * harvestPerHour(time.season);
        const float whole = std::floor(workplace.harvest);
        inventory.add(ItemKind::Food, static_cast<int>(whole));
        workplace.harvest -= whole;
    }

    // Водопровод: в жилье вода не кончается.
    if (rules.tapWater) {
        for (const auto [entity, building, inventory] : registry.view<const Building, Inventory>().each()) {
            topUp(inventory, ItemKind::Water, tapWater(building.kind));
        }
    }
    if (time.hour != kRestockHour) {
        return;
    }
    if (rules.supply > 0.0f) {
        restock(world, rules.supply);
    }
    if (rules.economy) {
        for (const auto [entity, person, wallet] : registry.view<const Person, Wallet>().each()) {
            const auto* job = registry.try_get<Job>(entity);
            if (job == nullptr || job->profession == Profession::None) {
                wallet.money += kAllowance;
            }
        }
    }
}

} // namespace lw
