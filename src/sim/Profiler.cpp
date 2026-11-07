#include "sim/Profiler.h"

#include <algorithm>
#include <mutex>

namespace lw {

namespace {

struct SectionTable {
    std::mutex mutex;
    std::vector<std::string> names;
    std::vector<int> depths;
};

SectionTable& sections() {
    static SectionTable table;
    return table;
}

int depthOf(const std::string& name) {
    return static_cast<int>(std::count(name.begin(), name.end(), '/'));
}

} // namespace

int profileSection(const char* name) {
    SectionTable& table = sections();
    const std::lock_guard lock(table.mutex);
    for (std::size_t i = 0; i < table.names.size(); ++i) {
        if (table.names[i] == name) {
            return static_cast<int>(i);
        }
    }
    table.names.emplace_back(name);
    table.depths.push_back(depthOf(table.names.back()));
    return static_cast<int>(table.names.size() - 1);
}

const std::string& profileSectionName(int section) {
    return sections().names[static_cast<std::size_t>(section)];
}

int profileSectionDepth(int section) {
    return sections().depths[static_cast<std::size_t>(section)];
}

int profileSectionCount() {
    SectionTable& table = sections();
    const std::lock_guard lock(table.mutex);
    return static_cast<int>(table.names.size());
}

std::vector<int> profileSectionsInTreeOrder() {
    SectionTable& table = sections();
    const std::lock_guard lock(table.mutex);
    const int count = static_cast<int>(table.names.size());
    std::vector<int> order;
    order.reserve(table.names.size());
    std::vector<bool> placed(table.names.size(), false);
    // Родитель секции — самая длинная секция, имя которой — её префикс до «/».
    const auto isChild = [&](int child, int parent) {
        const std::string& c = table.names[static_cast<std::size_t>(child)];
        const std::string& p = table.names[static_cast<std::size_t>(parent)];
        return table.depths[static_cast<std::size_t>(child)] == table.depths[static_cast<std::size_t>(parent)] + 1 &&
               c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/';
    };
    const auto visit = [&](auto& self, int section) -> void {
        placed[static_cast<std::size_t>(section)] = true;
        order.push_back(section);
        for (int child = 0; child < count; ++child) {
            if (!placed[static_cast<std::size_t>(child)] && isChild(child, section)) {
                self(self, child);
            }
        }
    };
    for (int section = 0; section < count; ++section) {
        if (!placed[static_cast<std::size_t>(section)] && table.depths[static_cast<std::size_t>(section)] == 0) {
            visit(visit, section);
        }
    }
    // Вложенные без зарегистрированного родителя — в конец.
    for (int section = 0; section < count; ++section) {
        if (!placed[static_cast<std::size_t>(section)]) {
            visit(visit, section);
        }
    }
    return order;
}

std::size_t Profiler::beginStartup(const char* name) {
    StartupStep step;
    step.name = name;
    step.depth = depthOf(step.name);
    startup_.push_back(std::move(step));
    return startup_.size() - 1;
}

double Profiler::startupSeconds() const {
    double total = 0.0;
    for (const StartupStep& step : startup_) {
        total += step.depth == 0 ? step.seconds : 0.0;
    }
    return total;
}

bool ProfileWindow::sample(const Profiler& profiler, double realDt, double period) {
    elapsed_ += realDt;
    if (elapsed_ < period) {
        return false;
    }
    const int count = profileSectionCount();
    last_.resize(static_cast<std::size_t>(count));
    const std::uint64_t ticks = profiler.ticks() - std::min(profiler.ticks(), lastTicks_);
    const double tickSeconds = profiler.tickSeconds() - lastTickSeconds_;
    rows_.clear();
    for (int section = 0; section < count; ++section) {
        const Profiler::Totals now = profiler.totals(section);
        Profiler::Totals& before = last_[static_cast<std::size_t>(section)];
        const double seconds = std::max(0.0, now.seconds - before.seconds);
        before = now;
        if (ticks == 0) {
            continue;
        }
        Row row;
        row.section = section;
        row.msPerTick = 1000.0 * seconds / static_cast<double>(ticks);
        row.share = tickSeconds > 0.0 ? seconds / tickSeconds : 0.0;
        rows_.push_back(row);
    }
    msPerTick_ = ticks > 0 ? 1000.0 * tickSeconds / static_cast<double>(ticks) : 0.0;
    ticksPerSecond_ = static_cast<double>(ticks) / elapsed_;
    lastTicks_ = profiler.ticks();
    lastTickSeconds_ = profiler.tickSeconds();
    elapsed_ = 0.0;
    return true;
}

} // namespace lw
