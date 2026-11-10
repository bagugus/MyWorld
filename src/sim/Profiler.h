#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lw {

// Встроенный профайлер (этап 2а.1): сколько реального времени уходит на каждую
// систему тика и на каждый шаг старта мира. Только читает часы — на состояние
// мира не влияет, в хеш не входит, детерминизм не трогает.
//
// Секция — имя с вложенностью через «/»: «perception», «perception/rays».
// Имена регистрируются один раз на процесс (profileSection), номер секции
// один и тот же во всех мирах. Время вложенной секции входит и в родителя.
//
// Замер тика включается setEnabled (выключенный стоит одну проверку флага на
// область). Шаги старта мира пишутся всегда: их немного и они нужны сразу.

// Номер секции по имени (создаёт при первом обращении).
int profileSection(const char* name);
// Имя секции и глубина вложенности (число «/» в имени).
const std::string& profileSectionName(int section);
int profileSectionDepth(int section);
int profileSectionCount();
// Все секции деревом: верхний уровень в порядке регистрации (то есть в порядке
// систем тика), вложенные — сразу за родителем.
std::vector<int> profileSectionsInTreeOrder();

class Profiler {
public:
    using Clock = std::chrono::steady_clock;

    struct Totals {
        double seconds = 0.0;
        std::uint64_t calls = 0;
    };

    // Шаг старта мира: имя, глубина и время.
    struct StartupStep {
        std::string name;
        int depth = 0;
        double seconds = 0.0;
    };

    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }

    void add(int section, double seconds) {
        if (static_cast<std::size_t>(section) >= totals_.size()) {
            totals_.resize(static_cast<std::size_t>(section) + 1);
        }
        Totals& totals = totals_[static_cast<std::size_t>(section)];
        totals.seconds += seconds;
        ++totals.calls;
    }
    // Тик целиком: от его числа считается время на тик.
    void countTick(double seconds) {
        ++ticks_;
        tickSeconds_ += seconds;
    }

    Totals totals(int section) const {
        return static_cast<std::size_t>(section) < totals_.size()
                   ? totals_[static_cast<std::size_t>(section)]
                   : Totals{};
    }
    std::uint64_t ticks() const { return ticks_; }
    double tickSeconds() const { return tickSeconds_; }
    void reset() {
        totals_.clear();
        ticks_ = 0;
        tickSeconds_ = 0.0;
    }

    // Шаги старта — в порядке начала (родитель раньше вложенных).
    std::size_t beginStartup(const char* name);
    void endStartup(std::size_t step, double seconds) { startup_[step].seconds = seconds; }
    const std::vector<StartupStep>& startup() const { return startup_; }
    double startupSeconds() const; // сумма шагов верхнего уровня

private:
    bool enabled_ = false;
    std::vector<Totals> totals_;
    std::uint64_t ticks_ = 0;
    double tickSeconds_ = 0.0;
    std::vector<StartupStep> startup_;
};

// Область замера: от конструктора до деструктора.
class ProfileScope {
public:
    ProfileScope(Profiler& profiler, int section)
        : profiler_(profiler.enabled() ? &profiler : nullptr), section_(section) {
        if (profiler_ != nullptr) {
            start_ = Profiler::Clock::now();
        }
    }
    ~ProfileScope() {
        if (profiler_ != nullptr) {
            profiler_->add(section_, std::chrono::duration<double>(Profiler::Clock::now() - start_).count());
        }
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    Profiler* profiler_;
    int section_;
    Profiler::Clock::time_point start_{};
};

// Шаг старта мира: пишется всегда.
class StartupScope {
public:
    StartupScope(Profiler& profiler, const char* name)
        : profiler_(profiler), step_(profiler.beginStartup(name)), start_(Profiler::Clock::now()) {}
    ~StartupScope() {
        profiler_.endStartup(step_, std::chrono::duration<double>(Profiler::Clock::now() - start_).count());
    }
    StartupScope(const StartupScope&) = delete;
    StartupScope& operator=(const StartupScope&) = delete;

private:
    Profiler& profiler_;
    std::size_t step_;
    Profiler::Clock::time_point start_;
};

// Скользящее окно для интерфейса: раз в period секунд — среднее время секций
// на тик за последний отрезок.
class ProfileWindow {
public:
    struct Row {
        int section = 0;
        double msPerTick = 0.0;
        double share = 0.0; // доля времени тика
    };

    // Вызывать раз в кадр; true — отрезок закончился и rows() обновились.
    bool sample(const Profiler& profiler, double realDt, double period = 1.0);
    const std::vector<Row>& rows() const { return rows_; }
    double msPerTick() const { return msPerTick_; }
    double ticksPerSecond() const { return ticksPerSecond_; }

private:
    std::vector<Profiler::Totals> last_;
    std::uint64_t lastTicks_ = 0;
    double lastTickSeconds_ = 0.0;
    double elapsed_ = 0.0;
    std::vector<Row> rows_;
    double msPerTick_ = 0.0;
    double ticksPerSecond_ = 0.0;
};

// Строка памяти: что и сколько байт.
struct MemoryItem {
    std::string name;
    std::size_t bytes = 0;
};

template <typename T>
std::size_t vectorBytes(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

} // namespace lw

#define LW_PROFILE_CONCAT_INNER(a, b) a##b
#define LW_PROFILE_CONCAT(a, b) LW_PROFILE_CONCAT_INNER(a, b)
// Замер до конца блока: LW_PROFILE(world.profiler(), "perception/rays");
#define LW_PROFILE(profiler, name)                                                          \
    static const int LW_PROFILE_CONCAT(lwProfileSection_, __LINE__) = ::lw::profileSection(name); \
    ::lw::ProfileScope LW_PROFILE_CONCAT(lwProfileScope_, __LINE__)((profiler),            \
                                                                    LW_PROFILE_CONCAT(lwProfileSection_, __LINE__))
