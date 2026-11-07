#include "render/WorldRenderer.h"

#include "render/Palette.h"

#include "sim/Components.h"
#include "sim/Family.h"
#include "sim/Infection.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

constexpr Color kBorderColor = {190, 190, 190, 255};

constexpr float kEntityRadiusMeters = 0.4f;
constexpr float kEntityMinRadiusPx = 3.0f;
constexpr float kOutlineMinZoom = 1.5f; // пикселей на метр: мельче контур здания не виден

// Знак ивента — столб дыма: клубы сносит по ветру, они растут и тают. На общем
// плане — ещё и кольцо в несколько пикселей, чтобы место было видно издалека.
void drawSmoke(const World& world, const WorldEvent& event, float zoom) {
    const float x = Map::cellCenter(event.site.x);
    const float y = Map::cellCenter(event.site.y);
    const Weather& weather = world.weather();
    // Откуда дует (0 — север, 90 — восток): дым идёт в обратную сторону, ось y — на юг.
    const float radians = weather.windFrom * DEG2RAD;
    const float toX = -std::sin(radians);
    const float toY = std::cos(radians);
    const float drift = 4.0f + 2.5f * weather.windSpeed;
    const float rise = static_cast<float>(std::fmod(GetTime() * 0.4, 1.0));
    constexpr int kPuffs = 7;
    for (int i = kPuffs - 1; i >= 0; --i) {
        const float t = (static_cast<float>(i) + rise) / static_cast<float>(kPuffs);
        const float along = t * drift * static_cast<float>(kPuffs);
        Color color = kSmokeColor;
        color.a = static_cast<unsigned char>(170.0f * (1.0f - t));
        DrawCircleV({x + toX * along, y + toY * along}, 3.0f + 9.0f * t, color);
    }
    const float ring = std::max(6.0f, 9.0f / zoom);
    DrawRing({x, y}, ring, ring + 2.0f / zoom, 0.0f, 360.0f, 32, kSmokeColor);
}

// Стая — россыпь точек вокруг её позиции (birdOffset), в полёте стая
// растягивается, а птицы машут крыльями (чисто визуально).
void drawFlock(const Flock& flock, EntityId id, Vector2 at, float zoom) {
    const int shown = std::min<int>(flock.birds, 16);
    const float spread = flock.flying ? 7.0f : 3.0f;
    const float dot = std::max(0.25f, 1.5f / zoom);
    const float flap = flock.flying ? static_cast<float>(GetTime()) * 9.0f : 0.0f;
    for (int i = 0; i < shown; ++i) {
        const Vector2 offset = birdOffset(id, i);
        const float bob = flock.flying ? std::sin(flap + static_cast<float>(i)) * 0.6f : 0.0f;
        DrawCircleV({at.x + offset.x * spread, at.y + offset.y * spread + bob}, dot, kBirdColor);
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

    for (const auto [entity, event] : world.registry().view<const WorldEvent>().each()) {
        if (world.tick() < event.signalUntil) {
            drawSmoke(world, event, zoom);
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
        if (const auto* flock = registry.try_get<Flock>(entity)) {
            drawFlock(*flock, registry.get<Id>(entity).value, at, zoom);
            continue;
        }
        if (registry.all_of<Cache>(entity)) {
            const auto* stock = registry.try_get<Inventory>(entity);
            const bool empty = stock == nullptr || stock->empty();
            const float half = radius * 1.1f;
            const Rectangle crate = {at.x - half, at.y - half, 2.0f * half, 2.0f * half};
            DrawRectangleRec(crate, empty ? kEmptyCacheColor : kCacheColor);
            DrawRectangleLinesEx(crate, std::max(0.1f, 1.0f / zoom), kCacheOutline);
            continue;
        }
        if (const auto* corpse = registry.try_get<Corpse>(entity)) {
            const Color color = corpseColor(corpse->kind);
            const float half = radius * 0.7f;
            DrawRectangleV({at.x - half, at.y - half}, {2.0f * half, 2.0f * half}, color);
            continue;
        }
        // Звери и дети (этап 7.3) — кружком поменьше.
        const float size = registry.all_of<Animal>(entity)                               ? radius * 0.8f
                           : registry.all_of<Age>(entity) && isMinor(registry, entity) ? radius * 0.7f
                                                                                       : radius;
        DrawCircleV(at, size, creatureColor(registry, entity));
        // Больной в лихорадке (этап 7.5) — оранжевым кольцом: это видно и
        // соседям. Скрытый период на карте не виден — его показывает инспектор
        // сценария.
        if (const auto* wounds = registry.try_get<Wounds>(entity);
            wounds != nullptr && registry.all_of<Person>(entity) && isFeverish(*wounds)) {
            const float ring = std::max(0.25f, 1.5f / zoom);
            DrawRing(at, size + 0.5f * ring, size + 1.5f * ring, 0.0f, 360.0f, 20, kFeverRingColor);
        }
    }
}

} // namespace lw
