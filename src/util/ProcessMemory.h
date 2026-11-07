#pragma once

#include <cstddef>

namespace lw {

// Память процесса по данным ОС (для замеров; на мир не влияет). Ноль — ОС не
// сообщает (не Windows).
struct ProcessMemory {
    std::size_t workingSet = 0;     // сейчас в оперативной памяти, байты
    std::size_t peakWorkingSet = 0; // наибольшее за жизнь процесса
    std::size_t privateBytes = 0;   // выделено процессом (куча, стеки)
};

ProcessMemory processMemory();

} // namespace lw
