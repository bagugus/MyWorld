#pragma once

#include "sim/Components.h"
#include "sim/Rng.h"

namespace lw {

// Черты характера нового человека. Каждая — колокол вокруг 0,5 (среднее трёх
// равномерных чисел): крайние характеры редки. Лидерство тянется к смелости и
// общительности, агрессия — против альтруизма.
Traits generateTraits(Rng& rng);

} // namespace lw
