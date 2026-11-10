#include "render/RoadRenderer.h"

#include "sim/World.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Общий план: линии видны в полную силу мельче этого зума (пикселей на метр)
// и гаснут к kOverviewTo — дальше полотно на текстуре карты читается само.
constexpr float kOverviewFull = 0.25f;
constexpr float kOverviewTo = 0.6f;
// Разметка появляется с kMarkingsFrom и видна в полную силу с kMarkingsFull.
constexpr float kMarkingsFrom = 1.5f;
constexpr float kMarkingsFull = 2.5f;

// Толщина линий общего плана, пиксели.
constexpr float kHighwayPx = 3.2f;
constexpr float kRegionalPx = 2.2f;
constexpr float kTrackPx = 1.2f;
constexpr Color kHighwayLine = {238, 190, 84, 255};
constexpr Color kRegionalLine = {228, 224, 208, 255};
constexpr Color kTrackLine = {168, 136, 98, 255};

// Разметка, метры: ширина линии, пунктир по оси (штрих и шаг), край полотна
// шоссе от оси, шаг, которым линия проверяет, что под ней.
constexpr float kMarkWidth = 0.15f;
constexpr float kDash = 3.0f;
constexpr float kDashPeriod = 9.0f;
constexpr float kEdgeOffset = 5.4f;
constexpr float kSample = 1.0f;
constexpr Color kMarkColor = {236, 236, 228, 255};

float ramp(float value, float from, float to) {
    return std::clamp((value - from) / (to - from), 0.0f, 1.0f);
}

Color faded(Color color, float strength) {
    color.a = static_cast<unsigned char>(static_cast<float>(color.a) * strength);
    return color;
}

Road roadAt(const Map& map, Vector2 point) {
    const Cell cell = Map::cellAt(point.x, point.y);
    return map.contains(cell) ? map.road.at(cell.x, cell.y) : Road::None;
}

} // namespace

RoadRenderer::RoadRenderer(const World& world) {
    for (const RoadLink& link : world.roads().links) {
        if (link.path.size() < 2) {
            continue;
        }
        Line line;
        line.kind = link.kind;
        const auto axis = [&](Cell cell) { return Vector2{link.axisX(cell), link.axisY(cell)}; };
        line.points.push_back(axis(link.path.front()));
        line.along.push_back(0.0f);
        float length = 0.0f;
        for (std::size_t k = 1; k < link.path.size(); ++k) {
            const Cell a = link.path[k - 1];
            const Cell b = link.path[k];
            length += (a.x != b.x && a.y != b.y) ? Map::kCellSize * 1.41421356f : Map::kCellSize;
            const bool last = k + 1 == link.path.size();
            // Оставляем только изломы: дальше шаг в ту же сторону.
            const bool turns = !last && (link.path[k + 1].x - b.x != b.x - a.x || link.path[k + 1].y - b.y != b.y - a.y);
            if (last || turns) {
                line.points.push_back(axis(b));
                line.along.push_back(length);
            }
        }
        float left = line.points.front().x;
        float top = line.points.front().y;
        float right = left;
        float bottom = top;
        for (const Vector2& p : line.points) {
            left = std::min(left, p.x);
            top = std::min(top, p.y);
            right = std::max(right, p.x);
            bottom = std::max(bottom, p.y);
        }
        const float margin = 2.0f * Map::kCellSize;
        line.bounds = {left - margin, top - margin, right - left + 2.0f * margin, bottom - top + 2.0f * margin};
        lines_.push_back(std::move(line));
    }
    // Снизу вверх по иерархии: шоссе рисуется поверх влившихся в него дорог.
    std::stable_sort(lines_.begin(), lines_.end(), [](const Line& a, const Line& b) { return a.kind < b.kind; });
}

void RoadRenderer::draw(const Map& map, const Rectangle& visible, float zoom) const {
    const float overview = 1.0f - ramp(zoom, kOverviewFull, kOverviewTo);
    if (overview > 0.0f) {
        drawOverview(visible, zoom, overview);
    }
    const float markings = ramp(zoom, kMarkingsFrom, kMarkingsFull);
    if (markings > 0.0f) {
        drawMarkings(map, visible, zoom, markings);
    }
}

void RoadRenderer::drawOverview(const Rectangle& visible, float zoom, float strength) const {
    for (const Line& line : lines_) {
        if (!CheckCollisionRecs(line.bounds, visible)) {
            continue;
        }
        const float px = line.kind == RoadClass::Highway    ? kHighwayPx
                         : line.kind == RoadClass::Regional ? kRegionalPx
                                                            : kTrackPx;
        const Color color = faded(line.kind == RoadClass::Highway    ? kHighwayLine
                                  : line.kind == RoadClass::Regional ? kRegionalLine
                                                                     : kTrackLine,
                                  strength);
        const float thick = px / zoom;
        for (std::size_t k = 1; k < line.points.size(); ++k) {
            DrawLineEx(line.points[k - 1], line.points[k], thick, color);
            // Стык отрезков — без щели на изломе.
            if (px >= 2.0f && k + 1 < line.points.size()) {
                DrawCircleV(line.points[k], thick * 0.5f, color);
            }
        }
    }
}

void RoadRenderer::drawMarkings(const Map& map, const Rectangle& visible, float zoom, float strength) const {
    const float width = std::max(kMarkWidth, 1.0f / zoom);
    const Color color = faded(kMarkColor, strength * 0.85f);
    for (const Line& line : lines_) {
        if (line.kind == RoadClass::Track || !CheckCollisionRecs(line.bounds, visible)) {
            continue;
        }
        const Road own = surfaceOf(line.kind);
        for (std::size_t k = 1; k < line.points.size(); ++k) {
            const Vector2 a = line.points[k - 1];
            const Vector2 b = line.points[k];
            const float length = line.along[k] - line.along[k - 1];
            if (length <= 0.0f) {
                continue;
            }
            const Rectangle span = {std::min(a.x, b.x) - kEdgeOffset, std::min(a.y, b.y) - kEdgeOffset,
                                    std::abs(b.x - a.x) + 2.0f * kEdgeOffset,
                                    std::abs(b.y - a.y) + 2.0f * kEdgeOffset};
            if (!CheckCollisionRecs(span, visible)) {
                continue;
            }
            const Vector2 dir = {(b.x - a.x) / length, (b.y - a.y) / length};
            const Vector2 normal = {-dir.y, dir.x};
            for (float t = 0.0f; t < length; t += kSample) {
                const float piece = std::min(kSample, length - t);
                const Vector2 from = {a.x + dir.x * t, a.y + dir.y * t};
                const Vector2 to = {from.x + dir.x * piece, from.y + dir.y * piece};
                // Трасса, влившаяся в шоссе, своей разметки там не рисует.
                if (roadAt(map, from) != own) {
                    continue;
                }
                const float distance = line.along[k - 1] + t;
                if (std::fmod(distance, kDashPeriod) < kDash) {
                    DrawLineEx(from, to, width, color);
                }
                if (line.kind != RoadClass::Highway) {
                    continue;
                }
                // Края шоссе; разрыв там, где сбоку примыкает другая дорога.
                for (const float side : {-1.0f, 1.0f}) {
                    const float off = side * kEdgeOffset;
                    const Vector2 outside = {from.x + normal.x * (off + side * 2.5f),
                                             from.y + normal.y * (off + side * 2.5f)};
                    const Road beyond = roadAt(map, outside);
                    if (beyond != Road::None && beyond != Road::Highway) {
                        continue;
                    }
                    DrawLineEx({from.x + normal.x * off, from.y + normal.y * off},
                               {to.x + normal.x * off, to.y + normal.y * off}, width, color);
                }
            }
        }
    }
}

} // namespace lw
