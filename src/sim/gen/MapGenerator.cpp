#include "sim/gen/MapGenerator.h"

#include "sim/gen/GenContext.h"

namespace lw {

GeneratedMap generateMap(Map& map, Rng& rng) {
    GeneratedMap result;
    gen::Context ctx(map, rng, result);

    // Порядок важен: каждый шаг опирается на предыдущие и тянет числа из
    // одного rng. Перестановка шагов меняет карты всех seed.
    gen::generateElevation(ctx);
    gen::generateWater(ctx);
    gen::generateGround(ctx);
    gen::placeSettlements(ctx);
    gen::generateVegetation(ctx); // после поселений: вокруг них поля и нет леса
    gen::buildRoads(ctx);         // после леса: дорога через лес дороже
    gen::buildStreets(ctx);
    gen::placeBuildings(ctx);
    gen::clearRoadsides(ctx);
    return result;
}

} // namespace lw
