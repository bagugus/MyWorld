#pragma once

#include <cstdint>

namespace lw {

// Детерминированный генератор случайных чисел мира (PCG32).
//
// Весь рандом симуляции идёт только через него: никаких rand() и
// std::uniform_*_distribution (их результат зависит от стандартной библиотеки).
class Rng {
public:
    explicit Rng(std::uint64_t seed, std::uint64_t stream = 0);

    std::uint32_t nextU32();
    std::uint64_t nextU64();

    // Целое в [0, bound), равномерно, без смещения. bound > 0.
    std::uint32_t below(std::uint32_t bound);
    // Целое в [lo, hi] включительно. lo <= hi.
    int rangeInt(int lo, int hi);
    // Вещественное в [0, 1).
    float unit();
    // Вещественное в [lo, hi).
    float range(float lo, float hi);
    // true с вероятностью p.
    bool chance(float p);

    // Независимый поток для подсистемы. Зависит только от исходного seed и
    // streamId, а не от того, сколько чисел уже вытянуто: новый вызов рандома
    // в одной системе не сдвигает последовательности остальных.
    Rng fork(std::uint64_t streamId) const;

    // Полное состояние — для хеша мира и сохранения.
    std::uint64_t seed() const { return seed_; }
    std::uint64_t state() const { return state_; }
    std::uint64_t increment() const { return inc_; }

private:
    std::uint64_t seed_;
    std::uint64_t state_;
    std::uint64_t inc_;
};

} // namespace lw
