#include "util/ProcessMemory.h"

// windows.h — только здесь: его макросы спорят с raylib (CloseWindow, Rectangle).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace lw {

ProcessMemory processMemory() {
    ProcessMemory result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        result.workingSet = counters.WorkingSetSize;
        result.peakWorkingSet = counters.PeakWorkingSetSize;
        result.privateBytes = counters.PrivateUsage;
    }
#endif
    return result;
}

} // namespace lw
