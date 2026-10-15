#include "render/WorldRenderer.h"

#include "sim/Components.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <algorithm>

namespace lw {

namespace {

constexpr Color kBorderColor = {190, 190, 190, 255};
constexpr Color kEntityColor = {236, 200, 92, 255};   // в пути
constexpr Color kRestingColor = {150, 205, 225, 255};  // на месте
constexpr Color kSleepingColor = {70, 84, 140, 255};
constexpr Color kEatingColor = {120, 210, 110, 255};   // ест, пьёт
constexpr Color kSearchingColor = {232, 132, 64, 255}; // ищет припасы, собирает в лесу
constexpr Color kSocialColor = {226, 128, 196, 255};
constexpr Color kFleeingColor = {255, 255, 255, 255};
// Зомби: спокойные — болотные, идущие на шум — жёлто-зелёные, в погоне — красные.
constexpr Color kZombieColor = {92, 128, 64, 255};
constexpr Color kZombieAlertColor = {170, 190, 60, 255};
constexpr Color kZombieChaseColor = {214, 58, 46, 255};
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

// Люди в пути — жёлтые (бегущие — белые), на месте — по занятию; зомби — по
// состоянию.
Color entityColor(const entt::registry& registry, entt::entity entity) {
    if (const auto* zombie = registry.try_get<Zombie>(entity)) {
        switch (zombie->state) {
        case ZombieState::Chase:
            return kZombieChaseColor;
        case ZombieState::Investigate:
            return kZombieAlertColor;
        default:
            return kZombieColor;
        }
    }
    const auto* mind = registry.try_get<Mind>(entity);
    if (mind != nullptr && mind->action == Action::Flee) {
        return kFleeingColor;
    }
    const auto* walker = registry.try_get<Walker>(entity);
    if (walker == nullptr || walker->moving()) {
        return kEntityColor;
    }
    if (mind == nullptr || mind->phase == ActionPhase::Travel) {
        return kRestingColor;
    }
    switch (mind->action) {
    case Action::Sleep:
        return kSleepingColor;
    case Action::Eat:
    case Action::Drink:
        return kEatingColor;
    case Action::Scavenge:
    case Action::Forage:
        return kSearchingColor;
    case Action::Socialize:
        return kSocialColor;
    default:
        return kRestingColor;
    }
}

} // namespace

WorldRenderer::WorldRenderer(const World& world) : map_(world.map()) {}

void WorldRenderer::draw(const World& world, const Camera2D& camera, float alpha) {
    const float zoom = camera.zoom;
    const Rectangle bounds = {0.0f, 0.0f, world.width(), world.height()};

    // Видимая часть мира: всё остальное не рисуем.
    const Vector2 topLeft = GetScreenToWorld2D({0.0f, 0.0f}, camera);
    const Vector2 bottomRight = GetScreenToWorld2D(
        {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())}, camera);
    const Rectangle visible = {topLeft.x, topLeft.y, bottomRight.x - topLeft.x,
                               bottomRight.y - topLeft.y};

    map_.draw(visible, zoom);
    // Даже полный покров оставляет карту читаемой.
    map_.drawSnow(visible, world.weather().snowCover * 0.85f);
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
    // Позиция на начало тика — текущая минус смещение за тик (Velocity · kTickDt).
    const float back = (1.0f - std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
    const entt::registry& registry = world.registry();
    for (const auto [entity, position] : registry.view<const Position>().each()) {
        Vector2 at = {position.x, position.y};
        if (const auto* velocity = registry.try_get<Velocity>(entity)) {
            at.x -= velocity->x * back;
            at.y -= velocity->y * back;
        }
        if (at.x < topLeft.x - radius || at.x > bottomRight.x + radius ||
            at.y < topLeft.y - radius || at.y > bottomRight.y + radius) {
            continue;
        }
        DrawCircleV(at, radius, entityColor(registry, entity));
    }
}

} // namespace lw
