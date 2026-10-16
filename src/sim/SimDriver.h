#pragma once

#include <cstdint>

namespace lw {

class World;

// Связывает реальное время с фиксированным тиком симуляции.
// Скорость x1/x10/x100 — это сколько секунд мира проходит за реальную секунду.
class SimDriver {
public:
    explicit SimDriver(World& world);

    // Вызывать раз в кадр. realDt — реальные секунды с прошлого кадра.
    void advance(double realDt);

    void setSpeed(int multiplier);
    int speed() const { return speed_; }

    void setPaused(bool paused) { paused_ = paused; }
    void togglePause() { paused_ = !paused_; }
    bool paused() const { return paused_; }

    // Выполнить один тик на паузе.
    void requestStep() { stepRequested_ = true; }

    // Промотка: прогнать мир вперёд на ticks тиков как можно быстрее.
    // Идёт порциями по кадрам (окно не зависает), на паузе тоже; повторный
    // вызов добавляет к оставшемуся. Результат тот же, что и при обычном ходе.
    void requestSkip(std::uint64_t ticks);
    void cancelSkip();
    bool skipping() const { return skipRemaining_ > 0; }
    std::uint64_t skipRemaining() const { return skipRemaining_; }
    // Доля выполненной промотки, 0..1.
    float skipProgress() const;

    // Сколько реального времени за кадр можно потратить на симуляцию.
    // Если тики не укладываются, отставание отбрасывается: мир идёт медленнее
    // заданной скорости, но окно не зависает. На состояние мира на данном тике
    // это не влияет — только на то, сколько тиков успело пройти.
    void setFrameBudgetMs(double milliseconds) { frameBudgetMs_ = milliseconds; }

    // Для плавной отрисовки: какая доля следующего тика уже накоплена, 0..1.
    // Мир хранит состояние на конец последнего тика; отрисовка показывает его
    // между предпоследним и последним тиком (отстаёт меньше чем на тик) — так
    // движение не дёргается с частотой тиков. На паузе — 1: ровно последний тик.
    float interpolationAlpha() const;

    int ticksLastFrame() const { return ticksLastFrame_; }
    double measuredTicksPerSecond() const { return measuredTicksPerSecond_; }
    // true, если в последнем кадре симуляция не успела за заданной скоростью.
    bool behind() const { return behind_; }

private:
    void updateStats(double realDt);

    World& world_;
    int speed_ = 1;
    bool paused_ = false;
    bool stepRequested_ = false;
    double accumulator_ = 0.0;
    double frameBudgetMs_ = 12.0;
    // На промотку кадр может тратить больше: смотреть всё равно не на что.
    double skipBudgetMs_ = 60.0;
    std::uint64_t skipRemaining_ = 0;
    std::uint64_t skipTotal_ = 0;

    int ticksLastFrame_ = 0;
    bool behind_ = false;
    double statsTime_ = 0.0;
    int statsTicks_ = 0;
    double measuredTicksPerSecond_ = 0.0;
};

} // namespace lw
