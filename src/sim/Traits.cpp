#include "sim/Traits.h"

#include <algorithm>

namespace lw {

namespace {

float bell(Rng& rng) {
    return (rng.unit() + rng.unit() + rng.unit()) / 3.0f;
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

Traits generateTraits(Rng& rng) {
    Traits traits;
    // Порядок вызовов задаёт характеры всех людей seed: не менять без нужды.
    traits.bravery = bell(rng);
    traits.greed = bell(rng);
    traits.altruism = bell(rng);
    traits.aggression = clamp01(bell(rng) + 0.4f * (0.5f - traits.altruism));
    traits.sociability = bell(rng);
    traits.leadership =
        clamp01(0.5f * bell(rng) + 0.25f * traits.bravery + 0.25f * traits.sociability);
    traits.curiosity = bell(rng);
    traits.prudence = bell(rng);
    traits.chronotype = bell(rng);
    return traits;
}

} // namespace lw
