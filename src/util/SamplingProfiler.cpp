#include "util/SamplingProfiler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// windows.h — только здесь: его макросы спорят с raylib (CloseWindow, Rectangle).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// dbghelp.h — после windows.h.
#include <dbghelp.h>
#include <timeapi.h>
#endif

namespace lw {

#ifdef _WIN32

struct SamplingProfiler::State {
    static constexpr int kDepth = 64;

    std::atomic<bool> running{false};
    std::thread thread;
    HANDLE target = nullptr;
    // Стеки подряд: адреса от вершины к корню, после каждого стека — 0.
    std::vector<std::uint64_t> frames;
    std::size_t samples = 0;

    void loop() {
        timeBeginPeriod(1);
        std::uint64_t stack[kDepth];
        while (running.load(std::memory_order_relaxed)) {
            Sleep(1);
            if (SuspendThread(target) == static_cast<DWORD>(-1)) {
                continue;
            }
            // Пока поток стоит — ни одного выделения памяти: он мог остановиться
            // внутри кучи с её замком.
            int depth = 0;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(target, &context)) {
                while (depth < kDepth && context.Rip != 0) {
                    stack[depth++] = context.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr) {
                        // Листовая функция без кадра: адрес возврата — на вершине стека.
                        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                        context.Rsp += 8;
                    } else {
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
                                         &handlerData, &establisher, nullptr);
                    }
                }
            }
            ResumeThread(target);
            if (depth > 0) {
                frames.insert(frames.end(), stack, stack + depth);
                frames.push_back(0);
                ++samples;
            }
        }
        timeEndPeriod(1);
    }
};

SamplingProfiler::SamplingProfiler() : state_(std::make_unique<State>()) {}

SamplingProfiler::~SamplingProfiler() {
    stop();
}

void SamplingProfiler::start() {
    if (state_->running) {
        return;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &state_->target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    state_->frames.reserve(1 << 22);
    state_->running = true;
    state_->thread = std::thread([state = state_.get()] { state->loop(); });
}

void SamplingProfiler::stop() {
    if (!state_->running) {
        return;
    }
    state_->running = false;
    state_->thread.join();
    CloseHandle(state_->target);
    state_->target = nullptr;
}

namespace {

// Имена функций по адресу: встроенные (inline) — от внутренней к внешней, затем
// сама функция. Кэш: адресов немного, а символы ищутся долго.
class Symbols {
public:
    Symbols() : process_(GetCurrentProcess()) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process_, nullptr, TRUE);
    }
    ~Symbols() { SymCleanup(process_); }

    const std::vector<std::string>& namesAt(std::uint64_t address) {
        const auto found = cache_.find(address);
        if (found != cache_.end()) {
            return found->second;
        }
        std::vector<std::string> names;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        const auto reset = [&] {
            std::fill(std::begin(buffer), std::end(buffer), '\0');
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
        };
        const DWORD inlineCount = SymAddrIncludeInlineTrace(process_, address);
        if (inlineCount > 0) {
            DWORD inlineContext = 0;
            DWORD frameIndex = 0;
            if (SymQueryInlineTrace(process_, address, 0, address, address, &inlineContext, &frameIndex)) {
                for (DWORD i = 0; i < inlineCount; ++i) {
                    reset();
                    DWORD64 displacement = 0;
                    if (SymFromInlineContext(process_, address, inlineContext + i, &displacement, symbol)) {
                        names.emplace_back(symbol->Name);
                    }
                }
            }
        }
        reset();
        DWORD64 displacement = 0;
        if (SymFromAddr(process_, address, &displacement, symbol)) {
            names.emplace_back(symbol->Name);
        } else {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(address));
            names.emplace_back(hex);
        }
        return cache_.emplace(address, std::move(names)).first->second;
    }

private:
    HANDLE process_;
    std::unordered_map<std::uint64_t, std::vector<std::string>> cache_;
};

void printTop(std::FILE* out, const char* title, const std::unordered_map<std::string, std::size_t>& counts,
              std::size_t samples, int top) {
    std::vector<std::pair<std::string, std::size_t>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::fprintf(out, "%s:\n", title);
    for (int i = 0; i < top && i < static_cast<int>(rows.size()); ++i) {
        std::string name = rows[static_cast<std::size_t>(i)].first;
        if (name.size() > 110) {
            name = name.substr(0, 107) + "...";
        }
        std::fprintf(out, "  %5.1f %%  %s\n",
                     100.0 * static_cast<double>(rows[static_cast<std::size_t>(i)].second) /
                         static_cast<double>(samples),
                     name.c_str());
    }
}

} // namespace

void SamplingProfiler::report(std::FILE* out, int top) const {
    if (state_->samples == 0) {
        std::fprintf(out, "sampling profile: no samples\n");
        return;
    }
    Symbols symbols;
    std::unordered_map<std::string, std::size_t> self;
    std::unordered_map<std::string, std::size_t> total;
    std::unordered_set<std::string> seen;
    bool first = true;
    for (const std::uint64_t address : state_->frames) {
        if (address == 0) {
            seen.clear();
            first = true;
            continue;
        }
        // Адрес возврата указывает за вызов: на байт назад — внутри вызывающей строки.
        const std::vector<std::string>& names = symbols.namesAt(first ? address : address - 1);
        if (first && !names.empty()) {
            ++self[names.front()];
        }
        for (const std::string& name : names) {
            if (seen.insert(name).second) {
                ++total[name];
            }
        }
        first = false;
    }
    std::fprintf(out, "sampling profile: %zu samples (~1 ms each)\n", state_->samples);
    printTop(out, "self (top of stack, inline frames resolved)", self, state_->samples, top);
    printTop(out, "total (anywhere in stack)", total, state_->samples, top);
}

#else

struct SamplingProfiler::State {};
SamplingProfiler::SamplingProfiler() : state_(std::make_unique<State>()) {}
SamplingProfiler::~SamplingProfiler() = default;
void SamplingProfiler::start() {}
void SamplingProfiler::stop() {}
void SamplingProfiler::report(std::FILE* out, int) const {
    std::fprintf(out, "sampling profile: not supported on this platform\n");
}

#endif

} // namespace lw
