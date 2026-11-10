// Общественные здания (этап 7.2): больница, полиция и школа в городах,
// сельский магазин в деревнях.

#include "sim/gen/GenContext.h"

#include <algorithm>
#include <vector>

namespace lw::gen {

namespace {

// Школа — не вплотную к больнице: не ближе этого к ней, клетки (если можно).
constexpr int kSchoolHospitalCells = 30;
// Магазин в деревне — если домов в ней хотя бы столько: в хуторе лавки нет.
constexpr int kVillageShopHouses = 4;

int distanceSq(Cell a, Cell b) {
    const int dx = a.x - b.x;
    const int dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Здания поселения этого вида — от ближайшего к центру; при равном расстоянии —
// в порядке постройки.
std::vector<std::size_t> byCentrality(const Context& ctx, int site, BuildingKind kind) {
    const Cell center = ctx.out.settlements[static_cast<std::size_t>(site)].center;
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < ctx.out.buildings.size(); ++i) {
        const BuildingSite& building = ctx.out.buildings[i];
        if (building.settlement == site && building.kind == kind) {
            result.push_back(i);
        }
    }
    std::stable_sort(result.begin(), result.end(), [&](std::size_t a, std::size_t b) {
        return distanceSq(ctx.out.buildings[a].door, center) <
               distanceSq(ctx.out.buildings[b].door, center);
    });
    return result;
}

} // namespace

// Рандома нет: шаг меняет только виды уже построенных зданий, поэтому рельеф,
// дороги и застройка всех seed остаются прежними.
void assignBuildingRoles(Context& ctx) {
    std::vector<BuildingSite>& buildings = ctx.out.buildings;
    for (int site = 0; site < static_cast<int>(ctx.out.settlements.size()); ++site) {
        if (ctx.out.settlements[static_cast<std::size_t>(site)].kind == SettlementKind::Village) {
            const std::vector<std::size_t> houses = byCentrality(ctx, site, BuildingKind::House);
            if (static_cast<int>(houses.size()) >= kVillageShopHouses) {
                buildings[houses.front()].kind = BuildingKind::Shop;
            }
            continue;
        }
        // Город: больница и школа — в многоэтажках у центра (их нет — в
        // магазинах), полиция — в ближайшем к центру магазине.
        std::vector<std::size_t> large = byCentrality(ctx, site, BuildingKind::Apartment);
        std::vector<std::size_t> shops = byCentrality(ctx, site, BuildingKind::Shop);
        if (!shops.empty()) {
            buildings[shops.front()].kind = BuildingKind::PoliceStation;
            shops.erase(shops.begin());
        }
        if (large.empty()) {
            large = shops;
        }
        if (large.empty()) {
            continue;
        }
        const std::size_t hospital = large.front();
        buildings[hospital].kind = BuildingKind::Hospital;
        large.erase(large.begin());
        if (large.empty()) {
            continue;
        }
        const auto apart = std::find_if(large.begin(), large.end(), [&](std::size_t i) {
            return distanceSq(buildings[i].door, buildings[hospital].door) >=
                   kSchoolHospitalCells * kSchoolHospitalCells;
        });
        buildings[apart != large.end() ? *apart : large.front()].kind = BuildingKind::School;
    }
}

} // namespace lw::gen
