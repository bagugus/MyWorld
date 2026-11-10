#include "sim/gen/MapGenerator.h"

#include "sim/Profiler.h"
#include "sim/gen/GenContext.h"

#include <optional>

namespace lw {

GeneratedMap generateMap(Map& map, Rng& rng, Profiler* profiler) {
    GeneratedMap result;
    std::optional<gen::Context> ctx;
    // Шаг генерации с замером времени (если есть куда писать).
    const auto run = [profiler](const char* name, auto&& step) {
        if (profiler == nullptr) {
            step();
            return;
        }
        const StartupScope scope(*profiler, name);
        step();
    };
    run("world/map/context", [&] { ctx.emplace(map, rng, result, profiler); });

    // Порядок важен: каждый шаг опирается на предыдущие и тянет числа из
    // одного rng. Перестановка шагов меняет карты всех seed.
    run("world/map/elevation", [&] { gen::generateElevation(*ctx); });
    run("world/map/water", [&] { gen::generateWater(*ctx); });
    run("world/map/ground", [&] { gen::generateGround(*ctx); });
    run("world/map/settlements", [&] { gen::placeSettlements(*ctx); });
    // После поселений: вокруг них поля и нет леса.
    run("world/map/vegetation", [&] { gen::generateVegetation(*ctx); });
    // После леса: дорога через лес дороже.
    run("world/map/roads", [&] { gen::buildRoads(*ctx); });
    run("world/map/streets", [&] { gen::buildStreets(*ctx); });
    run("world/map/buildings", [&] { gen::placeBuildings(*ctx); });
    run("world/map/roadsides", [&] { gen::clearRoadsides(*ctx); });
    // Без рандома: меняет только виды зданий.
    run("world/map/roles", [&] { gen::assignBuildingRoles(*ctx); });
    return result;
}

} // namespace lw
