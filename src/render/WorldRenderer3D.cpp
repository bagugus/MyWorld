#include "render/WorldRenderer3D.h"

#include "render/CameraController.h"
#include "render/Palette.h"
#include "render/Terrain3D.h"
#include "sim/Components.h"
#include "sim/TimeScale.h"
#include "sim/World.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

// Освещение пока постоянное (свет по времени суток — этап 11.4): солнце с
// северо-запада, как в отмывке рельефа карты. Яркость граней — от их нормали.
constexpr float kTopShade = 1.0f;
constexpr float kSideShadeBase = 0.72f;
constexpr float kSideShadeRange = 0.16f;
// Свет по земле: (−1, −1) — с запада и севера.
constexpr float kLightX = -0.7071f;
constexpr float kLightZ = -0.7071f;

// Существо мельче этого на экране растёт, чтобы его было видно на общем плане.
constexpr float kMinCreaturePixels = 4.0f;
constexpr int kCreatureSides = 6; // гранёный столбик: чётное число граней

// Высота этажа и этажность по виду здания.
constexpr float kStoreyMeters = 3.0f;
// Фундамент уходит в землю: на склоне здание не висит над ней.
constexpr float kFoundation = 0.5f;
// Стая в полёте — над деревьями.
constexpr float kFlightHeight = 14.0f;
// Настил моста — толщиной в полметра, клетка дороги над водой.
constexpr float kBridgeThickness = 0.5f;
constexpr Color kBridgeAsphalt = {84, 84, 90, 255};
constexpr Color kBridgeWood = {122, 96, 66, 255};

float storeys(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return 2.0f;
    case BuildingKind::Barn:
        return 1.7f;
    case BuildingKind::Apartment:
        return 5.0f;
    case BuildingKind::Shop:
        return 1.5f;
    case BuildingKind::Hospital:
        return 4.0f;
    case BuildingKind::PoliceStation:
        return 2.5f;
    case BuildingKind::School:
        return 3.0f;
    }
    return 2.0f;
}

// Облик существа: столбик радиуса radius и высоты height, метры.
struct Shape {
    float radius = 0.28f;
    float height = 1.75f;
};

Shape shapeOf(const entt::registry& registry, entt::entity entity) {
    if (const auto* animal = registry.try_get<Animal>(entity)) {
        switch (animal->kind) {
        case AnimalKind::Deer:
            return {0.45f, 1.3f};
        case AnimalKind::Boar:
            return {0.4f, 0.85f};
        default:
            return {0.18f, 0.35f};
        }
    }
    return {};
}

Color shaded(Color color, float factor) {
    const auto channel = [factor](unsigned char value) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(value) * factor, 0.0f, 255.0f));
    };
    return {channel(color.r), channel(color.g), channel(color.b), color.a};
}

// Яркость вертикальной грани с внешней нормалью (nx, 0, nz).
float sideShade(float nx, float nz) {
    return kSideShadeBase + kSideShadeRange * (nx * kLightX + nz * kLightZ);
}

void vertex(float x, float y, float z) {
    rlVertex3f(x, y, z);
}

void color(Color c) {
    rlColor4ub(c.r, c.g, c.b, c.a);
}

// Грани ниже выводятся в открытый rlBegin(RL_QUADS), против часовой стрелки
// при взгляде снаружи: обратные стороны отсекаются. Ось Y — вверх, Z — на юг.

// Коробка от min до max без дна.
void emitBox(Vector3 min, Vector3 max, Color base) {
    const float x0 = min.x;
    const float y0 = min.y;
    const float z0 = min.z;
    const float x1 = max.x;
    const float y1 = max.y;
    const float z1 = max.z;
    color(shaded(base, kTopShade));
    vertex(x0, y1, z0);
    vertex(x0, y1, z1);
    vertex(x1, y1, z1);
    vertex(x1, y1, z0);
    color(shaded(base, sideShade(0.0f, 1.0f))); // юг
    vertex(x0, y0, z1);
    vertex(x1, y0, z1);
    vertex(x1, y1, z1);
    vertex(x0, y1, z1);
    color(shaded(base, sideShade(0.0f, -1.0f))); // север
    vertex(x1, y0, z0);
    vertex(x0, y0, z0);
    vertex(x0, y1, z0);
    vertex(x1, y1, z0);
    color(shaded(base, sideShade(1.0f, 0.0f))); // восток
    vertex(x1, y0, z1);
    vertex(x1, y0, z0);
    vertex(x1, y1, z0);
    vertex(x1, y1, z1);
    color(shaded(base, sideShade(-1.0f, 0.0f))); // запад
    vertex(x0, y0, z0);
    vertex(x0, y0, z1);
    vertex(x0, y1, z1);
    vertex(x0, y1, z0);
}

// Гранёный столбик без дна: основание в base, kCreatureSides граней.
void emitPrism(Vector3 base, float radius, float height, Color c) {
    constexpr int n = kCreatureSides;
    float xs[n];
    float zs[n];
    for (int i = 0; i < n; ++i) {
        const float angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(n);
        xs[i] = base.x + radius * std::cos(angle);
        zs[i] = base.z + radius * std::sin(angle);
    }
    const float top = base.y + height;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float middle = 2.0f * PI * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        color(shaded(c, sideShade(std::cos(middle), std::sin(middle))));
        // Снаружи грань идёт от большего угла к меньшему.
        vertex(xs[j], base.y, zs[j]);
        vertex(xs[i], base.y, zs[i]);
        vertex(xs[i], top, zs[i]);
        vertex(xs[j], top, zs[j]);
    }
    // Крышка — веер четырёхугольников, обход по убыванию угла (сверху — против часовой).
    color(shaded(c, kTopShade));
    for (int k = n - 1; k >= 3; k -= 2) {
        vertex(xs[0], top, zs[0]);
        vertex(xs[k], top, zs[k]);
        vertex(xs[k - 1], top, zs[k - 1]);
        vertex(xs[k - 2], top, zs[k - 2]);
    }
}

// Множитель размера: мелкое вдали — не меньше kMinCreaturePixels на экране.
float visibleScale(const CameraController& camera, Vector2 at, float size) {
    return std::max(1.0f, kMinCreaturePixels / (camera.zoomAt(at) * size));
}

// Столб дыма над местом ивента: клубы поднимаются, их сносит по ветру, они
// растут и тают. Рисуется после непрозрачного.
void drawSmoke(const World& world, const WorldEvent& event, const CameraController& camera,
               const Terrain3D& terrain) {
    const Vector2 site = {Map::cellCenter(event.site.x), Map::cellCenter(event.site.y)};
    const float ground = terrain.surfaceAt(site.x, site.y);
    const Weather& weather = world.weather();
    // Откуда дует (0 — север, 90 — восток): дым идёт в обратную сторону, ось Z — на юг.
    const float radians = weather.windFrom * DEG2RAD;
    const float toX = -std::sin(radians);
    const float toZ = std::cos(radians);
    const float drift = 2.0f + 1.5f * weather.windSpeed;
    const float rise = static_cast<float>(std::fmod(GetTime() * 0.4, 1.0));
    const float scale = visibleScale(camera, site, 6.0f);
    constexpr int kPuffs = 9;
    for (int i = kPuffs - 1; i >= 0; --i) {
        const float t = (static_cast<float>(i) + rise) / static_cast<float>(kPuffs);
        const float along = t * drift * static_cast<float>(kPuffs);
        Color c = kSmokeColor;
        c.a = static_cast<unsigned char>(190.0f * (1.0f - t));
        const Vector3 center = {site.x + toX * along * scale, ground + (2.0f + 40.0f * t) * scale,
                                site.y + toZ * along * scale};
        DrawSphereEx(center, (2.0f + 7.0f * t) * scale, 6, 8, c);
    }
}

} // namespace

void WorldRenderer3D::draw(const World& world, const CameraController& camera, float alpha) {
    const Camera3D& view = camera.camera3d();
    const Vector3 eye = view.position;
    const Vector3 forward = Vector3Normalize(Vector3Subtract(view.target, eye));
    // Грубое отсечение: что целиком позади камеры, не рисуется.
    const auto behind = [&](Vector3 point, float size) {
        return Vector3DotProduct(Vector3Subtract(point, eye), forward) < -size;
    };

    BeginMode3D(view);
    // Даже полный покров оставляет карту читаемой.
    terrain_.drawGround(view, world.weather().snowCover * 0.85f);

    // Всё непрозрачное — одним набором четырёхугольников без текстуры.
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    rlNormal3f(0.0f, 1.0f, 0.0f);

    const entt::registry& registry = world.registry();
    buildingsDrawn_ = 0;
    for (const auto [entity, building] : registry.view<const Building>().each()) {
        const float x0 = static_cast<float>(building.origin.x) * Map::kCellSize;
        const float z0 = static_cast<float>(building.origin.y) * Map::kCellSize;
        const float x1 = x0 + static_cast<float>(building.width) * Map::kCellSize;
        const float z1 = z0 + static_cast<float>(building.height) * Map::kCellSize;
        // Стоит на нижнем углу участка, высота отсчитывается от верхнего.
        float low = terrain_.groundAt(x0, z0);
        float high = low;
        for (const Vector2 corner : {Vector2{x1, z0}, Vector2{x0, z1}, Vector2{x1, z1}}) {
            const float ground = terrain_.groundAt(corner.x, corner.y);
            low = std::min(low, ground);
            high = std::max(high, ground);
        }
        const float top = high + storeys(building.kind) * kStoreyMeters;
        const Vector3 center = {(x0 + x1) * 0.5f, (low + top) * 0.5f, (z0 + z1) * 0.5f};
        if (behind(center, std::max(x1 - x0, z1 - z0) + top - low)) {
            continue;
        }
        emitBox({x0, low - kFoundation, z0}, {x1, top, z1}, buildingColor(building.kind));
        ++buildingsDrawn_;
    }

    // Мосты — настил над водой по клеткам дороги (модели мостов — этап 11.3).
    for (const Terrain3D::Bridge& bridge : terrain_.bridges()) {
        const float x0 = static_cast<float>(bridge.cell.x) * Map::kCellSize;
        const float z0 = static_cast<float>(bridge.cell.y) * Map::kCellSize;
        if (behind({x0, bridge.deck, z0}, 2.0f * Map::kCellSize)) {
            continue;
        }
        emitBox({x0, bridge.deck - kBridgeThickness, z0},
                {x0 + Map::kCellSize, bridge.deck, z0 + Map::kCellSize},
                bridge.road == Road::Asphalt ? kBridgeAsphalt : kBridgeWood);
    }

    // Позиция на начало тика — текущая минус смещение за тик (Velocity · kTickDt).
    const float back = (1.0f - std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
    const float wingTime = static_cast<float>(GetTime()) * 9.0f;
    for (const auto [entity, position] : registry.view<const Position>().each()) {
        Vector2 at = {position.x, position.y};
        if (const auto* velocity = registry.try_get<Velocity>(entity)) {
            at.x -= velocity->x * back;
            at.y -= velocity->y * back;
        }
        const float stand = terrain_.standAt(at.x, at.y);
        if (behind({at.x, stand, at.y}, 50.0f)) {
            continue;
        }
        if (const auto* flock = registry.try_get<Flock>(entity)) {
            // Сидящие — на земле, летящие — над деревьями, машут крыльями.
            const EntityId id = registry.get<Id>(entity).value;
            const int shown = std::min<int>(flock->birds, 16);
            const float scale = visibleScale(camera, at, 0.3f);
            const float spread = (flock->flying ? 7.0f : 3.0f) * std::sqrt(scale);
            const float bird = 0.3f * scale;
            for (int i = 0; i < shown; ++i) {
                const Vector2 offset = birdOffset(id, i);
                const float height =
                    flock->flying ? terrain_.surfaceAt(at.x, at.y) + kFlightHeight +
                                        std::sin(wingTime + static_cast<float>(i)) * 0.6f
                                  : stand;
                const Vector3 min = {at.x + offset.x * spread - bird * 0.5f, height,
                                     at.y + offset.y * spread - bird * 0.5f};
                emitBox(min, {min.x + bird, height + bird * 0.6f, min.z + bird}, kBirdColor);
            }
            continue;
        }
        if (registry.all_of<Cache>(entity)) {
            const auto* stock = registry.try_get<Inventory>(entity);
            const bool empty = stock == nullptr || stock->empty();
            const float half = 0.6f * visibleScale(camera, at, 1.2f);
            emitBox({at.x - half, stand, at.y - half}, {at.x + half, stand + 1.6f * half, at.y + half},
                    empty ? kEmptyCacheColor : kCacheColor);
            continue;
        }
        if (const auto* corpse = registry.try_get<Corpse>(entity)) {
            // Лежит: низкая плита.
            const float half = 0.45f * visibleScale(camera, at, 0.9f);
            emitBox({at.x - half, stand, at.y - half}, {at.x + half, stand + 0.6f * half, at.y + half},
                    corpseColor(corpse->kind));
            continue;
        }
        const Shape shape = shapeOf(registry, entity);
        const float scale = visibleScale(camera, at, 2.0f * shape.radius);
        emitPrism({at.x, stand, at.y}, shape.radius * scale, shape.height * scale,
                  creatureColor(registry, entity));
    }
    rlEnd();
    rlSetTexture(0);

    terrain_.drawWater(view, static_cast<float>(GetTime()), kSkyColor);

    for (const auto [entity, event] : registry.view<const WorldEvent>().each()) {
        if (world.tick() < event.signalUntil) {
            drawSmoke(world, event, camera, terrain_);
        }
    }
    EndMode3D();
}

} // namespace lw
