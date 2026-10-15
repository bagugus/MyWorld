#include "render/WorldRenderer.h"

#include "sim/Components.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include "sim/Hash.h"

#include <algorithm>
#include <cmath>

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
// Животные — по виду, бегущие — светлее; птицы — чёрные точки.
constexpr Color kDeerColor = {150, 98, 52, 255};
constexpr Color kBoarColor = {86, 70, 60, 255};
constexpr Color kHareColor = {176, 166, 150, 255};
constexpr Color kBirdColor = {24, 24, 28, 255};
// Тела — тёмные квадратики: человек — бурый, зомби — тёмно-болотный, туша — тёмно-коричневая.
constexpr Color kHumanBodyColor = {110, 40, 36, 255};
constexpr Color kZombieBodyColor = {52, 62, 36, 255};
constexpr Color kCarcassColor = {84, 56, 36, 255};
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
    if (const auto* animal = registry.try_get<Animal>(entity)) {
        Color color = animal->kind == AnimalKind::Deer   ? kDeerColor
                      : animal->kind == AnimalKind::Boar ? kBoarColor
                                                         : kHareColor;
        if (animal->state == AnimalState::Flee) {
            color = ColorBrightness(color, 0.35f);
        }
        return color;
    }
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

// Стая — россыпь точек вокруг её позиции: место каждой птицы — из хеша id
// стаи, в полёте стая растягивается, а птицы машут крыльями (чисто визуально).
void drawFlock(const Flock& flock, EntityId id, Vector2 at, float zoom) {
    const int shown = std::min<int>(flock.birds, 16);
    const float spread = flock.flying ? 7.0f : 3.0f;
    const float dot = std::max(0.25f, 1.5f / zoom);
    const float flap = flock.flying ? static_cast<float>(GetTime()) * 9.0f : 0.0f;
    for (int i = 0; i < shown; ++i) {
        Hasher hasher;
        hasher.add(id);
        hasher.add(i);
        // У FNV младшие биты соседних i похожи: перемешать, иначе птицы встают в линию.
        std::uint64_t bits = hasher.value();
        bits ^= bits >> 33;
        bits *= 0xFF51AFD7ED558CCDULL;
        bits ^= bits >> 33;
        const float ox = (static_cast<float>(bits & 0xFFFu) / 4095.0f * 2.0f - 1.0f) * spread;
        const float oy = (static_cast<float>((bits >> 12) & 0xFFFu) / 4095.0f * 2.0f - 1.0f) * spread;
        const float bob = flock.flying ? std::sin(flap + static_cast<float>(i)) * 0.6f : 0.0f;
        DrawCircleV({at.x + ox, at.y + oy + bob}, dot, kBirdColor);
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
        if (const auto* flock = registry.try_get<Flock>(entity)) {
            drawFlock(*flock, registry.get<Id>(entity).value, at, zoom);
            continue;
        }
        if (const auto* corpse = registry.try_get<Corpse>(entity)) {
            const Color color = corpse->kind == CorpseKind::Human    ? kHumanBodyColor
                                : corpse->kind == CorpseKind::Zombie ? kZombieBodyColor
                                                                     : kCarcassColor;
            const float half = radius * 0.7f;
            DrawRectangleV({at.x - half, at.y - half}, {2.0f * half, 2.0f * half}, color);
            continue;
        }
        const float size = registry.all_of<Animal>(entity) ? radius * 0.8f : radius;
        DrawCircleV(at, size, entityColor(registry, entity));
    }
}

} // namespace lw
