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

void SimDriver::advance(double realDt) {
    using Clock = std::chrono::steady_clock;

    ticksLastFrame_ = 0;
    behind_ = false;

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
