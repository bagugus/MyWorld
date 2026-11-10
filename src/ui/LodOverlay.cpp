#include "ui/LodOverlay.h"

#include "render/CameraController.h"
#include "sim/Components.h"
#include "sim/Lod.h"
#include "sim/World.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace lw::ui {

namespace {

bool enabled = false;

constexpr Color kFullZone = {230, 60, 50, 70};
constexpr Color kCalmZone = {230, 200, 60, 32};
constexpr Color kFullRing = {255, 80, 60, 235};
constexpr Color kCalmRing = {240, 210, 80, 210};
constexpr Color kDormantRing = {150, 155, 170, 190};

// Кольца у существ — только вблизи: дальше метров на пиксель они сливаются.
constexpr float kRingMaxMetersPerPixel = 0.8f;

} // namespace

bool lodOverlay() {
    return enabled;
}

void setLodOverlay(bool value) {
    enabled = value;
}

void drawLodOverlay(const World& world, const CameraController& camera) {
    const ActivityMap& map = world.activityMap();
    if (!enabled || camera.is3d() || !world.config().lod || !map.computed()) {
        return;
    }
    // Видимые районы — по углам экрана.
    const std::optional<Vector2> topLeft = camera.screenToWorld({0.0f, 0.0f});
    const std::optional<Vector2> bottomRight = camera.screenToWorld(
        {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())});
    if (!topLeft || !bottomRight) {
        return;
    }
    const int x0 = map.regionX(std::min(topLeft->x, bottomRight->x));
    const int x1 = map.regionX(std::max(topLeft->x, bottomRight->x));
    const int y0 = map.regionY(std::min(topLeft->y, bottomRight->y));
    const int y1 = map.regionY(std::max(topLeft->y, bottomRight->y));
    constexpr float size = ActivityMap::kRegionSize;
    for (int ry = y0; ry <= y1; ++ry) {
        for (int rx = x0; rx <= x1; ++rx) {
            // Полная — там, где для людей или зомби есть встреча, и вокруг боя.
            const bool full = (map.testRegion(ActivityMap::PeopleNear, rx, ry) &&
                               map.testRegion(ActivityMap::UndeadNear, rx, ry)) ||
                              map.testRegion(ActivityMap::ActiveNear, rx, ry);
            const bool calm = map.testRegion(ActivityMap::PeopleFar, rx, ry);
            if (!full && !calm) {
                continue;
            }
            DrawRectangleRec({static_cast<float>(rx) * size, static_cast<float>(ry) * size, size, size},
                             full ? kFullZone : kCalmZone);
        }
    }

    const float metersPerPixel = 1.0f / std::max(camera.zoom(), 1e-4f);
    if (metersPerPixel > kRingMaxMetersPerPixel) {
        return;
    }
    const float minX = static_cast<float>(x0) * size;
    const float maxX = static_cast<float>(x1 + 1) * size;
    const float minY = static_cast<float>(y0) * size;
    const float maxY = static_cast<float>(y1 + 1) * size;
    const float radius = std::max(2.2f, 7.0f * metersPerPixel);
    const float width = std::max(0.35f, 1.6f * metersPerPixel);
    for (const auto [entity, lod, position] : world.registry().view<const Lod, const Position>().each()) {
        if (position.x < minX || position.x > maxX || position.y < minY || position.y > maxY) {
            continue;
        }
        const Color color = lod.level == Detail::Full ? kFullRing
                            : lod.level == Detail::Calm ? kCalmRing
                                                        : kDormantRing;
        DrawRing({position.x, position.y}, radius, radius + width, 0.0f, 360.0f, 16, color);
    }
}

} // namespace lw::ui
