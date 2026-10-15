#include "sim/Rng.h"

#include <cassert>

namespace lw {

namespace {

std::uint64_t splitMix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

} // namespace

Rng::Rng(std::uint64_t seed, std::uint64_t stream)
    : seed_(seed), state_(0), inc_((stream << 1u) | 1u) {
    nextU32();
    state_ += seed;
    nextU32();
}

std::uint32_t Rng::nextU32() {
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

std::uint64_t Rng::nextU64() {
    const std::uint64_t hi = nextU32();
    return (hi << 32u) | nextU32();
}

std::uint32_t Rng::below(std::uint32_t bound) {
    assert(bound > 0);
    // Отбрасываем «хвост» диапазона, из-за которого остаток был бы смещён.
    const std::uint32_t threshold = (0u - bound) % bound;
    for (;;) {
        const std::uint32_t r = nextU32();
        if (r >= threshold) {
            return r % bound;
        }
    }
}

int Rng::rangeInt(int lo, int hi) {
    assert(lo <= hi);
    const std::uint32_t span = static_cast<std::uint32_t>(hi) - static_cast<std::uint32_t>(lo) + 1u;
    const std::uint32_t offset = span == 0 ? nextU32() : below(span);
    return static_cast<int>(static_cast<std::uint32_t>(lo) + offset);
}

float Rng::unit() {
    // 24 старших бита точно представимы во float, результат строго меньше 1.
    return static_cast<float>(nextU32() >> 8u) * (1.0f / 16777216.0f);
}

float Rng::range(float lo, float hi) {
    return lo + (hi - lo) * unit();
}

bool Rng::chance(float p) {
    return unit() < p;
}

Rng Rng::fork(std::uint64_t streamId) const {
    return Rng(splitMix64(seed_ ^ splitMix64(streamId)), streamId);
}

} // namespace lw
