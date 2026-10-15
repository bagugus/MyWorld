#pragma once

#include <bit>
#include <concepts>
#include <cstdint>

namespace lw {

// FNV-1a, 64 бита. Значения подаются побайтно, начиная с младшего байта,
// поэтому результат не зависит от порядка байтов платформы.
class Hasher {
public:
    template <std::integral T>
    void add(T value) {
        const auto bits = static_cast<std::uint64_t>(value);
        for (unsigned i = 0; i < sizeof(T); ++i) {
            hash_ ^= (bits >> (i * 8u)) & 0xFFu;
            hash_ *= 0x100000001B3ULL;
        }
    }
    void add(float value) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value))); }
    std::uint64_t value() const { return hash_; }

private:
    std::uint64_t hash_ = 0xCBF29CE484222325ULL;
};

} // namespace lw
