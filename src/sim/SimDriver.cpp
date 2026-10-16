#include "sim/SimDriver.h"

#include "sim/World.h"

#include <algorithm>
#include <chrono>

namespace lw {

namespace {

// Длинный кадр (перетаскивание окна, точка останова) не должен порождать
// лавину тиков.
constexpr double kMaxFrameDt = 0.25;
constexpr double kStatsWindow = 0.5;

} // namespace

SimDriver::SimDriver(World& world) : world_(world) {}

void SimDriver::setSpeed(int multiplier) {
    speed_ = std::max(1, multiplier);
}

void SimDriver::requestSkip(std::uint64_t ticks) {
    if (ticks == 0) {
        return;
    }
    if (skipRemaining_ == 0) {
        skipTotal_ = 0;
    }
    skipRemaining_ += ticks;
    skipTotal_ += ticks;
}

void SimDriver::cancelSkip() {
    skipRemaining_ = 0;
    skipTotal_ = 0;
}

float SimDriver::skipProgress() const {
    if (skipTotal_ == 0) {
        return 1.0f;
    }
    return static_cast<float>(skipTotal_ - skipRemaining_) / static_cast<float>(skipTotal_);
}

void SimDriver::advance(double realDt) {
    using Clock = std::chrono::steady_clock;

    ticksLastFrame_ = 0;
    behind_ = false;

    if (skipRemaining_ > 0) {
        accumulator_ = 0.0;
        stepRequested_ = false;
        const Clock::time_point start = Clock::now();
        while (skipRemaining_ > 0) {
            world_.step();
            --skipRemaining_;
            ++ticksLastFrame_;
            if ((ticksLastFrame_ & 7) == 0) {
                const std::chrono::duration<double, std::milli> elapsed = Clock::now() - start;
                if (elapsed.count() > skipBudgetMs_) {
                    break;
                }
            }
        }
        if (skipRemaining_ == 0) {
            skipTotal_ = 0;
        }
        updateStats(realDt);
        return;
    }

    if (paused_) {
        accumulator_ = 0.0;
        if (stepRequested_) {
            world_.step();
            ticksLastFrame_ = 1;
        }
        stepRequested_ = false;
        updateStats(realDt);
        return;
    }
    stepRequested_ = false;

    const double tickDt = 1.0 / World::kTicksPerSecond;
    accumulator_ += std::min(realDt, kMaxFrameDt) * speed_;

    const Clock::time_point start = Clock::now();
    while (accumulator_ >= tickDt) {
        world_.step();
        accumulator_ -= tickDt;
        ++ticksLastFrame_;

        // Часы спрашиваем не на каждом тике — это заметно на x100.
        if ((ticksLastFrame_ & 7) == 0 && accumulator_ >= tickDt) {
            const std::chrono::duration<double, std::milli> elapsed = Clock::now() - start;
            if (elapsed.count() > frameBudgetMs_) {
                accumulator_ = 0.0;
                behind_ = true;
                break;
            }
        }
    }

    updateStats(realDt);
}

float SimDriver::interpolationAlpha() const {
    if (paused_ || skipRemaining_ > 0) {
        return 1.0f;
    }
    const double alpha = accumulator_ * World::kTicksPerSecond;
    return static_cast<float>(std::clamp(alpha, 0.0, 1.0));
}

void SimDriver::updateStats(double realDt) {
    statsTime_ += realDt;
    statsTicks_ += ticksLastFrame_;
    if (statsTime_ >= kStatsWindow) {
        measuredTicksPerSecond_ = statsTicks_ / statsTime_;
        statsTime_ = 0.0;
        statsTicks_ = 0;
    }
}

} // namespace lw
