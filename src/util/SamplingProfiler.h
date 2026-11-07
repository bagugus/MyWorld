#pragma once

#include <cstdio>
#include <memory>

namespace lw {

// Сэмплирующий профайлер для поиска горячих функций (этап 2а.1, инструмент
// разработчика): отдельный поток примерно раз в миллисекунду останавливает
// поток, который его запустил, снимает стек и продолжает. В конце — функции по
// числу попаданий: «своё» время (вершина стека) и полное (функция где-то в
// стеке). Имена — из PDB рядом с exe. Только Windows; в других ОС — пусто.
//
// На симуляцию не влияет: только останавливает поток на десятки микросекунд.
class SamplingProfiler {
public:
    SamplingProfiler();
    ~SamplingProfiler();
    SamplingProfiler(const SamplingProfiler&) = delete;
    SamplingProfiler& operator=(const SamplingProfiler&) = delete;

    // Начать снимать стеки потока, который вызвал start.
    void start();
    void stop();
    // top — сколько функций показать в каждом списке.
    void report(std::FILE* out, int top = 40) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace lw
