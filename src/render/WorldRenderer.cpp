#include "render/WorldRenderer.h"

#include "sim/Components.h"
#include "sim/World.h"

#include <algorithm>

namespace lw {

namespace {

constexpr Color kBorderColor = {190, 190, 190, 255};
constexpr Color kEntityColor = {236, 200, 92, 255};
constexpr Color kBuildingOutline = {40, 34, 30, 255};

constexpr float kEntityRadiusMeters = 0.4f;
constexpr float kEntityMinRadiusPx = 3.0f;
constexpr float kOutlineMinZoom = 1.5f; // пикселей на метр: мельче контур здания не виден

Color buildingColor(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return {198, 164, 124, 255};
    case BuildingKind::Barn:
        return {142, 104, 74, 255};
    case BuildingKind::Apartment:
        return {176, 178, 186, 255};
    case BuildingKind::Shop:
        return {204, 132, 92, 255};
    }
    return {198, 164, 124, 255};
}

} // namespace

WorldRenderer::WorldRenderer(const World& world) : map_(world.map()) {}

void WorldRenderer::draw(const World& world, const Camera2D& camera) {
    const float zoom = camera.zoom;
    const Rectangle bounds = {0.0f, 0.0f, world.width(), world.height()};

    // Видимая часть мира: всё остальное не рисуем.
    const Vector2 topLeft = GetScreenToWorld2D({0.0f, 0.0f}, camera);
    const Vector2 bottomRight = GetScreenToWorld2D(
        {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())}, camera);
    const Rectangle visible = {topLeft.x, topLeft.y, bottomRight.x - topLeft.x,
                               bottomRight.y - topLeft.y};

    map_.draw(visible, zoom);
    DrawRectangleLinesEx(bounds, 2.0f / zoom, kBorderColor);

    for (const auto [entity, building] : world.registry().view<const Building>().each()) {
        const Rectangle rect = {static_cast<float>(building.origin.x) * Map::kCellSize,
                                static_cast<float>(building.origin.y) * Map::kCellSize,
                                static_cast<float>(building.width) * Map::kCellSize,
                                static_cast<float>(building.height) * Map::kCellSize};
        if (!CheckCollisionRecs(rect, visible)) {
            continue;
        }
        DrawRectangleRec(rect, buildingColor(building.kind));
        if (zoom >= kOutlineMinZoom) {
            DrawRectangleLinesEx(rect, 1.0f / zoom, kBuildingOutline);
        }
    }

    // Радиус в метрах, но не меньше нескольких пикселей — чтобы существа
    // было видно и на общем плане.
    const float radius = std::max(kEntityRadiusMeters, kEntityMinRadiusPx / zoom);
    for (const auto [entity, position] : world.registry().view<const Position>().each()) {
        if (position.x < topLeft.x - radius || position.x > bottomRight.x + radius ||
            position.y < topLeft.y - radius || position.y > bottomRight.y + radius) {
            continue;
        }
        DrawCircleV({position.x, position.y}, radius, kEntityColor);
    }
}

} // namespace lw
