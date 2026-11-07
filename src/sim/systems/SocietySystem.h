#pragma once

#include "sim/Components.h"
#include "sim/TimeScale.h"

namespace lw {

class World;

// Реакция общества (этап 7.6): правила — sim/Society.h, подробности и числа —
// docs/architecture.md, раздел «Реакция общества».
//
// Раз в игровую минуту (только у мира со сценарием, с начала вспышки): кто
// услышал официальную передачу (дома, на работе, в магазине, в больнице), кто
// что увидел своими глазами (больного в жару, укушенного, тело, зомби); в часы
// колонн — колонна из каждого пункта эвакуации. Раз в час — карантин: где
// объявить, где снять. Рассказы друг другу — в SocialSystem, решения людей —
// в Utility AI.

// Карантин объявляют, когда в поселении столько случаев (quarantineCases),
// снимают, когда случаев нет kQuarantineClearTicks, или когда власти не стало.
inline constexpr int kQuarantineCases = 2;
inline constexpr std::uint64_t kQuarantineClearTicks = kTicksPerGameDay;

void updateSociety(World& world);

// Началась фаза мира (ScenarioSystem): паника — пункты эвакуации и солдаты,
// коллапс — армия уходит (часть солдат остаётся), карантины брошены,
// последняя передача.
void onPhaseBegan(World& world, WorldPhase phase);

// Колонна из пункта эвакуации point (здание): садятся ждущие у пункта — семьи с
// детьми, старики, остальные, сколько мест; заметно больных солдаты не пускают,
// родитель без своих детей не уезжает. last — последняя колонна (армия уходит).
// Возвращает, скольких увезли.
int departConvoy(World& world, EntityId point, bool last = false);

} // namespace lw
