#include "sim/Roads.h"

#include <algorithm>

namespace lw {

namespace {

// Шаги по асфальту слышны дальше: твёрдое покрытие звенит, грунт и трава глушат.
constexpr float kPavedFootsteps = 1.25f;
// Раскисшая грунтовка и пашня в ливень: на столько доля скорости меньше.
constexpr float kMudDirtRoad = 0.3f;
constexpr float kMudField = 0.25f;
// Морось почти не размывает: мокро — с этой силы осадков, сильнее — грязнее.
constexpr float kWetFrom = 0.1f;

} // namespace

Road surfaceOf(RoadClass kind) {
    switch (kind) {
    case RoadClass::Highway:
        return Road::Highway;
    case RoadClass::Regional:
        return Road::Asphalt;
    case RoadClass::Track:
        break;
    }
    return Road::Dirt;
}

int widthOf(RoadClass kind) {
    switch (kind) {
    case RoadClass::Highway:
        return 3;
    case RoadClass::Regional:
        return 2;
    case RoadClass::Track:
        break;
    }
    return 1;
}

const char* nameOf(RoadClass kind) {
    switch (kind) {
    case RoadClass::Highway:
        return "шоссе";
    case RoadClass::Regional:
        return "трасса";
    case RoadClass::Track:
        break;
    }
    return "просёлок";
}

const char* surfaceName(Road road) {
    switch (road) {
    case Road::Highway:
        return "шоссе";
    case Road::Asphalt:
        return "асфальт";
    case Road::Dirt:
        return "грунтовая";
    case Road::None:
        break;
    }
    return "нет";
}

float footstepLoudness(Road road) {
    return road == Road::Asphalt || road == Road::Highway ? kPavedFootsteps : 1.0f;
}

float mudWetness(float precipitation, bool snowing) {
    if (snowing || precipitation <= kWetFrom) {
        return 0.0f;
    }
    return std::min(1.0f, (precipitation - kWetFrom) / (1.0f - kWetFrom));
}

float mudSpeedFactor(Road road, Ground ground, float wetness) {
    if (wetness <= 0.0f) {
        return 1.0f;
    }
    if (road == Road::Dirt) {
        return 1.0f - kMudDirtRoad * wetness;
    }
    if (road == Road::None && ground == Ground::Field) {
        return 1.0f - kMudField * wetness;
    }
    return 1.0f;
}

float RoadLink::axisX(Cell cell) const {
    return (static_cast<float>(cell.x) + 0.5f * static_cast<float>(widthOf(kind))) * Map::kCellSize;
}

float RoadLink::axisY(Cell cell) const {
    return (static_cast<float>(cell.y) + 0.5f * static_cast<float>(widthOf(kind))) * Map::kCellSize;
}

float RoadNetwork::kilometers(RoadClass kind) const {
    float meters = 0.0f;
    for (const RoadLink& link : links) {
        if (link.kind == kind) {
            meters += link.lengthMeters;
        }
    }
    return meters / 1000.0f;
}

int RoadNetwork::count(RoadClass kind) const {
    return static_cast<int>(std::count_if(links.begin(), links.end(),
                                          [kind](const RoadLink& link) { return link.kind == kind; }));
}

std::optional<RoadClass> roadClassAt(const RoadNetwork& network, Cell cell) {
    std::optional<RoadClass> best;
    for (const RoadLink& link : network.links) {
        if (best && *best >= link.kind) {
            continue;
        }
        const int width = widthOf(link.kind);
        for (const Cell& c : link.path) {
            if (cell.x >= c.x && cell.x < c.x + width && cell.y >= c.y && cell.y < c.y + width) {
                best = link.kind;
                break;
            }
        }
    }
    return best;
}

} // namespace lw
