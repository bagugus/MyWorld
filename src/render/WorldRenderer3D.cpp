#include "render/WorldRenderer3D.h"

#include "render/CameraController.h"
#include "render/Lighting.h"
#include "render/Models3D.h"
#include "render/Palette.h"
#include "render/Terrain3D.h"
#include "render/TreeLayout.h"
#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Scenario.h"
#include "sim/TimeScale.h"
#include "sim/Weapons.h"
#include "sim/World.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>

namespace lw {

namespace {

// Всё — в настоящую величину. Человек ниже этого на экране, пикселей, рисуется
// упрощённым силуэтом.
constexpr float kSimplePixels = 12.0f;
// Доля дальности прорисовки, в которой модели растворяются.
constexpr float kFadeShare = 0.25f;
// Мосты видны дальше существ во столько раз.
constexpr float kBridgeRange = 3.0f;
// Метка выбранного: высота на экране, пикселей, и не меньше, метров.
constexpr float kBeaconPixels = 16.0f;
constexpr float kBeaconMinSize = 0.35f;
constexpr Color kBeaconColor = {255, 236, 120, 255};
// Окна, двери и крыльцо — когда метр на экране не меньше этого, пикселей.
constexpr float kDetailPixels = 3.0f;
// Стая в полёте — над деревьями (самые высокие — около 30 м).
constexpr float kFlightHeight = 32.0f;
// Настил моста — толщиной в полметра, клетка дороги над водой.
constexpr float kBridgeThickness = 0.5f;
constexpr Color kBridgeAsphalt = {84, 84, 90, 255};
constexpr Color kBridgeWood = {122, 96, 66, 255};
constexpr Color kConcreteColor = {150, 150, 146, 255};
constexpr Color kTimberColor = {92, 70, 50, 255};
// Поворот к цели движения, радиан в секунду.
constexpr float kTurnRate = 8.0f;
// Сколько кадров помнить облик существа, которое не рисуется.
constexpr std::uint32_t kForgetFrames = 600;
// Тени — в квадрате вокруг точки взгляда: полусторона растёт с расстоянием
// камеры ступенями (чтобы при зуме тени не дрожали), дальше kShadowsOff — теней нет.
constexpr float kShadowMinRadius = 40.0f;
constexpr float kShadowMaxRadius = 1600.0f;
constexpr float kShadowStep = 1.2f;
constexpr float kShadowsOff = 4000.0f;
// Дымка — слой у земли: с высоты H метров её видно в 1 + H / kHazeHeight раз меньше.
constexpr float kHazeHeight = 1500.0f;

constexpr Color kTrousersColor = {58, 62, 78, 255};
constexpr Color kZombieSkin = {138, 156, 112, 255};
constexpr Color kZombieTrousers = {52, 56, 48, 255};
constexpr Color kDeadSkin = {176, 164, 150, 255};
constexpr Color kSkinTones[] = {{232, 196, 164, 255}, {198, 150, 112, 255}, {140, 98, 70, 255}};

// Разброс облика по id: одинаков от кадра к кадру.
std::uint64_t mixBits(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ULL;
    value ^= value >> 33;
    return value;
}

float angleOf(std::uint64_t bits) {
    return static_cast<float>(bits % 6283u) / 1000.0f;
}

// Видимость на расстоянии distance при дальности range: 1 — целиком, 0 — нет.
float fadeAt(float distance, float range) {
    return std::clamp((range - distance) / (range * kFadeShare), 0.0f, 1.0f);
}

// Длина шага зверя, метры.
float animalStride(AnimalKind kind) {
    switch (kind) {
    case AnimalKind::Deer:
        return 2.2f;
    case AnimalKind::Boar:
        return 1.4f;
    default:
        return 0.9f;
    }
}

// Полувысота лежащей на боку туши: на столько её поднять над землёй.
float carcassLift(AnimalKind kind) {
    switch (kind) {
    case AnimalKind::Deer:
        return 0.19f;
    case AnimalKind::Boar:
        return 0.22f;
    default:
        return 0.08f;
    }
}

// Столб дыма над местом ивента: клубы поднимаются, их сносит по ветру, они
// растут и тают. Рисуется после непрозрачного.
void drawSmoke(const World& world, const WorldEvent& event, const Terrain3D& terrain, const SceneLight& light) {
    const Vector2 site = {Map::cellCenter(event.site.x), Map::cellCenter(event.site.y)};
    const float ground = terrain.surfaceAt(site.x, site.y);
    const Weather& weather = world.weather();
    // Откуда дует (0 — север, 90 — восток): дым идёт в обратную сторону, ось Z — на юг.
    const float radians = weather.windFrom * DEG2RAD;
    const float toX = -std::sin(radians);
    const float toZ = std::cos(radians);
    const float drift = 2.0f + 1.5f * weather.windSpeed;
    const float rise = static_cast<float>(std::fmod(GetTime() * 0.4, 1.0));
    constexpr float scale = 1.0f;
    constexpr int kPuffs = 9;
    for (int i = kPuffs - 1; i >= 0; --i) {
        const float t = (static_cast<float>(i) + rise) / static_cast<float>(kPuffs);
        const float along = t * drift * static_cast<float>(kPuffs);
        // Дым освещён, как всё вокруг: ночью тёмный.
        const Vector3 lit = Vector3Add(light.ambientSky, Vector3Scale(light.lightColor, 0.7f));
        Color c = {static_cast<unsigned char>(std::min(255.0f, kSmokeColor.r * lit.x)),
                   static_cast<unsigned char>(std::min(255.0f, kSmokeColor.g * lit.y)),
                   static_cast<unsigned char>(std::min(255.0f, kSmokeColor.b * lit.z)),
                   static_cast<unsigned char>(190.0f * (1.0f - t))};
        const Vector3 center = {site.x + toX * along * scale, ground + (2.0f + 40.0f * t) * scale,
                                site.y + toZ * along * scale};
        DrawSphereEx(center, (2.0f + 7.0f * t) * scale, 6, 8, c);
    }
}

} // namespace

WorldRenderer3D::WorldRenderer3D(MapRenderer& textures, const Map& map)
    : land_(map), terrain_(map, textures, land_), trees_(map, terrain_, land_), props_(map, terrain_, land_) {
    // Перила — по краям настила над открытой водой, опоры — через клетку до дна.
    const auto key = [&map](Cell cell) {
        return static_cast<std::int64_t>(cell.y) * map.width() + cell.x;
    };
    std::unordered_set<std::int64_t> bridged;
    for (const Terrain3D::Bridge& bridge : terrain_.bridges()) {
        bridged.insert(key(bridge.cell));
    }
    for (const Terrain3D::Bridge& bridge : terrain_.bridges()) {
        const bool asphalt = bridge.road == Road::Asphalt;
        const float x0 = static_cast<float>(bridge.cell.x) * Map::kCellSize;
        const float z0 = static_cast<float>(bridge.cell.y) * Map::kCellSize;
        const float x1 = x0 + Map::kCellSize;
        const float z1 = z0 + Map::kCellSize;
        const float deck = bridge.deck;
        const float t = asphalt ? 0.25f : 0.12f;
        const float y0 = asphalt ? deck : deck + 0.8f;
        const float y1 = asphalt ? deck + 0.8f : deck + 0.95f;
        const Color rail = asphalt ? kConcreteColor : kTimberColor;
        constexpr int kDx[] = {0, 0, -1, 1};
        constexpr int kDy[] = {-1, 1, 0, 0};
        for (int d = 0; d < 4; ++d) {
            const Cell next = {bridge.cell.x + kDx[d], bridge.cell.y + kDy[d]};
            if (!map.contains(next) || map.water.at(next.x, next.y) == Water::None ||
                bridged.contains(key(next))) {
                continue;
            }
            Vector3 min = {x0, y0, z0};
            Vector3 max = {x1, y1, z1};
            switch (d) {
            case 0:
                max.z = z0 + t;
                break;
            case 1:
                min.z = z1 - t;
                break;
            case 2:
                max.x = x0 + t;
                break;
            default:
                min.x = x1 - t;
                break;
            }
            bridgeParts_.push_back({min, max, rail});
            if (!asphalt) {
                // Столбики перил у начала каждого пролёта.
                const Vector3 postMin = {min.x, deck, min.z};
                const Vector3 postMax = {std::min(max.x, min.x + 0.15f), y1, std::min(max.z, min.z + 0.15f)};
                bridgeParts_.push_back({postMin, postMax, rail});
            }
        }
        if ((bridge.cell.x + bridge.cell.y) % (asphalt ? 3 : 2) == 0) {
            const float cx = (x0 + x1) * 0.5f;
            const float cz = (z0 + z1) * 0.5f;
            const float half = asphalt ? 0.6f : 0.2f;
            const float bottom = terrain_.groundAt(cx, cz) - 0.5f;
            const float top = deck - kBridgeThickness;
            if (top > bottom) {
                bridgeParts_.push_back({{cx - half, bottom, cz - half}, {cx + half, top, cz + half},
                                        asphalt ? kConcreteColor : kTimberColor});
            }
        }
    }
}

float WorldRenderer3D::snowCover(const World& world) const {
    if (!foliageYear_) {
        return world.weather().snowCover;
    }
    // Сезон вручную: зимой — снег, как обычно бывает к середине зимы.
    const float year = *foliageYear_;
    return 0.85f * std::clamp((year - 0.76f) / 0.04f, 0.0f, 1.0f) * std::clamp((1.0f - year) / 0.04f, 0.0f, 1.0f);
}

void WorldRenderer3D::setDrawDistance(float meters) {
    drawDistance_ = std::clamp(meters, kMinDrawDistance, kMaxDrawDistance);
}

BuildingModel WorldRenderer3D::buildingModel(const Building& building, EntityId id) const {
    BuildingModel model;
    model.kind = building.kind;
    model.x0 = static_cast<float>(building.origin.x) * Map::kCellSize;
    model.z0 = static_cast<float>(building.origin.y) * Map::kCellSize;
    model.x1 = model.x0 + static_cast<float>(building.width) * Map::kCellSize;
    model.z1 = model.z0 + static_cast<float>(building.height) * Map::kCellSize;
    // Стоит на нижнем углу участка, высота отсчитывается от верхнего.
    model.low = terrain_.groundAt(model.x0, model.z0);
    model.high = model.low;
    for (const Vector2 corner : {Vector2{model.x1, model.z0}, Vector2{model.x0, model.z1},
                                 Vector2{model.x1, model.z1}}) {
        const float ground = terrain_.groundAt(corner.x, corner.y);
        model.low = std::min(model.low, ground);
        model.high = std::max(model.high, ground);
    }
    // Дверь — на стене напротив клетки входа.
    const Vector2 doorCell = {Map::cellCenter(building.door.x), Map::cellCenter(building.door.y)};
    model.door = {std::clamp(doorCell.x, model.x0, model.x1), std::clamp(doorCell.y, model.z0, model.z1)};
    model.doorGround = terrain_.groundAt(doorCell.x, doorCell.y);
    model.seed = mixBits(id);
    return model;
}

void WorldRenderer3D::updateBuildingBounds(const World& world) {
    if (!land_.hasSettlements()) {
        // Дворы и столбы — по зданиям: раскладка вне леса меняется.
        land_.addSettlements(world);
        trees_.reset();
        props_.reset();
    }
    const auto buildings = world.registry().view<const Building>();
    if (buildingBounds_.size() == buildings.size()) {
        return; // здания не меняются: посчитано раньше
    }
    buildingBounds_.clear();
    buildingBounds_.reserve(buildings.size());
    for (const auto [entity, building] : buildings.each()) {
        const BuildingModel model = buildingModel(building, world.registry().get<Id>(entity).value);
        buildingBounds_.push_back({{model.x0, model.low, model.z0}, {model.x1, buildingTop(model), model.z1}});
    }
}

std::optional<float> WorldRenderer3D::drawnHeading(EntityId id) const {
    const auto it = motion_.find(id);
    if (it == motion_.end() || frame_ - it->second.seen > 1) {
        return std::nullopt;
    }
    return it->second.heading;
}

WorldRenderer3D::Motion& WorldRenderer3D::motionOf(EntityId id, float dx, float dy, float stride, float dt) {
    const auto [it, inserted] = motion_.try_emplace(id);
    Motion& motion = it->second;
    const float speed = std::sqrt(dx * dx + dy * dy);
    if (inserted) {
        const std::uint64_t bits = mixBits(id);
        motion.heading = speed > 0.05f ? headingOf(dx, dy) : angleOf(bits);
        motion.phase = angleOf(bits >> 20);
    }
    // Облик меняется раз за кадр, сколько бы проходов (тень, кадр) его ни рисовали.
    if (motion.seen == frame_) {
        return motion;
    }
    if (speed > 0.05f) {
        const float turn = std::remainder(headingOf(dx, dy) - motion.heading, 2.0f * PI);
        motion.heading += std::clamp(turn, -kTurnRate * dt, kTurnRate * dt);
        // Фаза растёт с пройденным путём: ноги не скользят. Период — 4π (зомби качается вдвое реже).
        motion.phase = std::fmod(motion.phase + 2.0f * PI * speed * dt / stride, 4.0f * PI);
    }
    motion.seen = frame_;
    return motion;
}

void WorldRenderer3D::draw(const World& world, const CameraController& camera, float alpha) {
    const Camera3D& view = camera.camera3d();
    const Vector3 eye = view.position;
    ++frame_;
    updateBuildingBounds(world);
    dt_ = std::min(GetFrameTime(), 0.1f);
    const float snow = snowCover(world);
    const Environment environment = environmentAt(world);

    // Свет кадра: солнце или луна, небо, дымка (сверху её видно меньше).
    const SceneLight light = sceneLightAt(environment, world.weather());
    const float height = std::max(0.0f, eye.y - terrain_.surfaceAt(eye.x, eye.z));
    shading_.setLight(light, eye, 1.0f / (1.0f + height / kHazeHeight));

    // Где ночью горит свет: дома, где кто-то не спит, и открытая работа.
    litBuildings_.clear();
    lights_ = 0.6f * std::clamp((light.night - 0.05f) / 0.5f, 0.0f, 1.0f);
    const entt::registry& registry = world.registry();
    if (lights_ > 0.0f) {
        for (const auto [entity, person] : registry.view<const Person>().each()) {
            if (person.indoors && !person.asleep && person.home != kInvalidId) {
                litBuildings_.insert(person.home);
            }
        }
        for (const auto [entity, workplace, id] : registry.view<const Workplace, const Id>().each()) {
            if (workplace.onDuty > 0) {
                litBuildings_.insert(id.value);
            }
        }
    }

    // Деревья: какие сетки нужны с этой точки (и для теней тоже).
    Trees3D::Look trees;
    trees.year = foliageYear_.value_or(static_cast<float>(environment.time.yearFraction));
    trees.snow = snow;
    trees.wind = {light.windX, light.windZ};
    trees.time = static_cast<float>(GetTime());
    if (treesEnabled_) {
        trees_.update(view, kNearTreeRange, std::min(drawDistance_, kMaxTreeRange));
    }
    // Мелочь вне леса; мусор — по запустению мира (с паники, больше после коллапса).
    Props3D::Look props;
    props.year = trees.year;
    props.snow = snow;
    props.wind = trees.wind;
    props.time = trees.time;
    switch (worldPhase(world)) {
    case WorldPhase::Panic:
        props.decay = 0.25f;
        break;
    case WorldPhase::Collapse:
        props.decay = 0.6f;
        break;
    case WorldPhase::Survival:
        props.decay = 1.0f;
        break;
    default:
        props.decay = 0.0f;
        break;
    }
    if (propsEnabled_) {
        props_.update(view, Props3D::kSmallRange, std::min(drawDistance_, kMaxPropsRange));
    }

    // Тени: квадрат вокруг точки взгляда, чуть вперёд по взгляду.
    const float distance = Vector3Distance(eye, view.target);
    if (shadowsEnabled_ && distance < kShadowsOff) {
        const float wanted = std::clamp(distance * 1.1f, kShadowMinRadius, kShadowMaxRadius);
        const float steps = std::ceil(std::log(wanted / kShadowMinRadius) / std::log(kShadowStep));
        const float radius = std::min(kShadowMinRadius * std::pow(kShadowStep, steps), kShadowMaxRadius);
        Vector3 ahead = {view.target.x - eye.x, 0.0f, view.target.z - eye.z};
        ahead = Vector3Length(ahead) > 1e-3f ? Vector3Normalize(ahead) : Vector3{};
        const Vector3 center = Vector3Add(view.target, Vector3Scale(ahead, 0.35f * radius));
        if (shading_.beginShadowPass(center, radius)) {
            terrain_.drawShadowCasters(view, shading_.shadowCenter(), shading_.shadowRadius());
            if (treesEnabled_) {
                trees_.drawShadows(shading_, trees);
            }
            if (propsEnabled_) {
                props_.drawShadows(shading_, props);
            }
            rlSetTexture(rlGetTextureIdDefault());
            rlBegin(RL_QUADS);
            emitScene(world, camera, alpha, true);
            rlEnd();
            rlSetTexture(0);
            shading_.endShadowPass();
        }
    }

    shading_.bindTextures();
    const float time = static_cast<float>(GetTime());
    shading_.drawSky(view, time);

    BeginMode3D(view);
    // Даже полный покров оставляет карту читаемой.
    const Foliage foliage = averageFoliageAt(trees.year);
    terrain_.drawGround(view, snow * 0.85f, {foliage.leaves, foliage.turn, foliage.fresh}, trees.year, shading_);
    if (treesEnabled_) {
        trees_.draw(shading_, trees);
    }
    if (propsEnabled_) {
        props_.draw(shading_, props);
    }

    // Всё непрозрачное — одним набором четырёхугольников без текстуры.
    shading_.beginModels();
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    emitScene(world, camera, alpha, false);
    rlEnd();
    rlSetTexture(0);
    rlDrawRenderBatchActive();
    shading_.endModels();

    // Забыть облик тех, кого давно нет в кадре.
    if (frame_ % kForgetFrames == 0) {
        std::erase_if(motion_, [this](const auto& entry) { return frame_ - entry.second.seen > kForgetFrames; });
    }

    terrain_.drawWater(time, trees.wind, shading_);

    for (const auto [entity, event] : registry.view<const WorldEvent>().each()) {
        if (world.tick() < event.signalUntil) {
            drawSmoke(world, event, terrain_, light);
        }
    }
    EndMode3D();
    shading_.unbindTextures();
}

void WorldRenderer3D::emitScene(const World& world, const CameraController& camera, float alpha, bool shadowPass) {
    const Camera3D& view = camera.camera3d();
    const Vector3 eye = view.position;
    const Vector3 forward = Vector3Normalize(Vector3Subtract(view.target, eye));
    const float nearPlane = static_cast<float>(rlGetCullDistanceNear());
    // Что не нужно этому проходу: в кадре — целиком позади камеры, в тени — вне
    // квадрата карты теней.
    const auto skip = [&](Vector3 point, float size) {
        if (shadowPass) {
            return !shading_.inShadowArea(point, size);
        }
        return Vector3DotProduct(Vector3Subtract(point, eye), forward) < -size;
    };
    const float dt = dt_;
    const float snow = snowCover(world);

    const entt::registry& registry = world.registry();
    if (!shadowPass) {
        buildingsDrawn_ = 0;
        creaturesDrawn_ = 0;
    }
    for (const auto [entity, building] : registry.view<const Building>().each()) {
        const EntityId id = registry.get<Id>(entity).value;
        BuildingModel model = buildingModel(building, id);
        const Vector2 center = {(model.x0 + model.x1) * 0.5f, (model.z0 + model.z1) * 0.5f};
        const float top = buildingTop(model);
        if (skip({center.x, (model.low + top) * 0.5f, center.y},
                   std::max(model.x1 - model.x0, model.z1 - model.z0) + top - model.low)) {
            continue;
        }
        model.snow = snow;
        // Мелочь на стенах тени не отбрасывает. Окно меньше нескольких пикселей
        // мелькало бы при движении камеры: у предела детали тают в цвет стены.
        const float pixels = camera.zoomAt(center);
        const float distance = Vector3Distance(eye, {center.x, model.low, center.y});
        model.detailed = !shadowPass && pixels >= kDetailPixels && distance < drawDistance_;
        model.detail = std::clamp((pixels - kDetailPixels) / kDetailPixels, 0.0f, 1.0f);
        // Окно — плоскость у самой стены. Вдали буфер глубины различает
        // только слои в d² / (near · 2²⁴) друг от друга; окно отходит от стены
        // на втрое больше, иначе стена и окно мерцают, перекрывая друг друга.
        model.decalOut = std::max(0.04f, 3.0f * distance * distance / (nearPlane * 16777216.0f));
        model.lights = !shadowPass && litBuildings_.contains(id) ? lights_ : 0.0f;
        emitBuilding(model);
        buildingsDrawn_ += shadowPass ? 0 : 1;
    }

    // Мосты: настил над водой по клеткам дороги, перила и опоры; вдали тают.
    const float bridgeRange = kBridgeRange * drawDistance_;
    const auto bridgeFade = [&](Vector3 point) {
        return fadeAt(Vector3Distance(eye, point), bridgeRange);
    };
    for (const Terrain3D::Bridge& bridge : terrain_.bridges()) {
        const float x0 = static_cast<float>(bridge.cell.x) * Map::kCellSize;
        const float z0 = static_cast<float>(bridge.cell.y) * Map::kCellSize;
        const float fade = bridgeFade({x0, bridge.deck, z0});
        if (fade <= 0.0f || skip({x0, bridge.deck, z0}, 2.0f * Map::kCellSize)) {
            continue;
        }
        setModelFade(fade);
        emitWorldBox({x0, bridge.deck - kBridgeThickness, z0},
                     {x0 + Map::kCellSize, bridge.deck, z0 + Map::kCellSize},
                     bridge.road == Road::Asphalt ? kBridgeAsphalt : kBridgeWood, true);
    }
    for (const Part& part : bridgeParts_) {
        const float fade = bridgeFade(part.min);
        if (fade > 0.0f && !skip(part.min, 2.0f * Map::kCellSize)) {
            setModelFade(fade);
            emitWorldBox(part.min, part.max, part.color);
        }
    }
    setModelFade(1.0f);

    // Позиция на начало тика — текущая минус смещение за тик (Velocity · kTickDt).
    const float back = (1.0f - std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
    const float wingTime = static_cast<float>(GetTime()) * 9.0f;
    std::optional<Vector4> beacon; // метка выбранного: x, y, z острия и высота
    for (const auto [entity, position] : registry.view<const Position>().each()) {
        Vector2 at = {position.x, position.y};
        Vector2 velocity = {0.0f, 0.0f};
        if (const auto* v = registry.try_get<Velocity>(entity)) {
            velocity = {v->x, v->y};
            at.x -= v->x * back;
            at.y -= v->y * back;
        }
        const float stand = terrain_.standAt(at.x, at.y);
        if (skip({at.x, stand, at.y}, 50.0f)) {
            continue;
        }
        const EntityId id = registry.get<Id>(entity).value;
        // Дальше дальности прорисовки — не рисуется (кроме выбранного), у края тает.
        const bool highlighted = id == highlight_;
        const float distance = Vector3Distance(eye, {at.x, stand, at.y});
        const float fade = highlighted ? 1.0f : fadeAt(distance, drawDistance_);
        if (fade <= 0.0f) {
            continue;
        }
        setModelFade(fade);
        if (highlighted && !shadowPass) {
            const float size = std::max(kBeaconPixels / camera.zoomAt(at), kBeaconMinSize);
            const float bob = 0.15f * size * std::sin(static_cast<float>(GetTime()) * 3.0f);
            beacon = Vector4{at.x, stand + 2.2f + 0.4f * size + bob, at.y, size};
        }
        const std::uint64_t bits = mixBits(id);
        if (const auto* flock = registry.try_get<Flock>(entity)) {
            // Сидящие — на земле, летящие — над деревьями, машут крыльями.
            const int shown = std::min<int>(flock->birds, 16);
            const float spread = flock->flying ? 7.0f : 3.0f;
            const float course = headingOf(flock->toX - at.x, flock->toY - at.y);
            for (int i = 0; i < shown; ++i) {
                const Vector2 offset = birdOffset(id, i);
                const float flap = wingTime + static_cast<float>(i);
                const float height = flock->flying ? terrain_.surfaceAt(at.x, at.y) + kFlightHeight +
                                                         std::sin(flap) * 0.6f
                                                   : stand;
                ModelFrame frame;
                frame.origin = {at.x + offset.x * spread, height, at.y + offset.y * spread};
                frame.yaw = flock->flying ? course : angleOf(mixBits(bits + static_cast<std::uint64_t>(i)));
                emitBird(frame, flock->flying, flap);
            }
            continue;
        }
        if (registry.all_of<Cache>(entity)) {
            const auto* stock = registry.try_get<Inventory>(entity);
            const bool empty = stock == nullptr || stock->empty();
            ModelFrame frame;
            frame.origin = {at.x, stand, at.y};
            frame.yaw = angleOf(bits);
            emitCrate(frame, empty ? kEmptyCacheColor : kCacheColor);
            continue;
        }
        if (const auto* corpse = registry.try_get<Corpse>(entity)) {
            ModelFrame frame;
            frame.origin = {at.x, stand, at.y};
            frame.yaw = angleOf(bits);
            if (corpse->kind == CorpseKind::Animal) {
                frame.roll = PI * 0.5f; // на боку
                frame.origin.y += carcassLift(corpse->animal) * frame.scale;
                emitAnimal(frame, corpse->animal, corpseColor(corpse->kind), {}, false, false);
            } else {
                frame.pitch = -PI * 0.5f; // ничком
                frame.origin.y += 0.12f * frame.scale;
                HumanLook look;
                look.shirt = corpseColor(corpse->kind);
                look.trousers = corpse->kind == CorpseKind::Zombie ? kZombieTrousers : kTrousersColor;
                look.skin = corpse->kind == CorpseKind::Zombie ? kZombieSkin : kDeadSkin;
                emitHuman(frame, look, {});
            }
            continue;
        }

        const auto* person = registry.try_get<Person>(entity);
        if (person != nullptr && person->indoors) {
            continue; // в доме: снаружи не видно
        }
        const auto* animal = registry.try_get<Animal>(entity);
        const float stride = animal != nullptr ? animalStride(animal->kind) : 1.6f;
        Motion& motion = motionOf(id, velocity.x, velocity.y, stride, dt);
        // Стоящий в бою поворачивается к цели.
        const auto* combat = registry.try_get<Combat>(entity);
        if (combat != nullptr && combat->target != kInvalidId && velocity.x == 0.0f && velocity.y == 0.0f) {
            const entt::entity target = world.find(combat->target);
            if (target != entt::null) {
                if (const auto* aim = registry.try_get<Position>(target)) {
                    motion.heading = headingOf(aim->x - at.x, aim->y - at.y);
                }
            }
        }
        const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
        const Gait gait = {motion.phase, std::clamp(speed / (0.9f * stride), 0.0f, 1.3f)};
        ModelFrame frame;
        frame.origin = {at.x, stand, at.y};
        frame.yaw = motion.heading;
        creaturesDrawn_ += shadowPass ? 0 : 1;

        if (animal != nullptr) {
            const bool grazing = animal->state == AnimalState::Graze && speed < 0.05f;
            emitAnimal(frame, animal->kind, creatureColor(registry, entity), gait, grazing, (bits & 1u) != 0u);
            continue;
        }
        HumanLook look;
        look.simple = 1.75f * camera.zoomAt(at) < kSimplePixels;
        look.shirt = creatureColor(registry, entity);
        if (registry.all_of<Zombie>(entity)) {
            look.zombie = true;
            look.trousers = kZombieTrousers;
            look.skin = kZombieSkin;
        } else {
            look.trousers = kTrousersColor;
            look.skin = kSkinTones[(bits >> 8) % std::size(kSkinTones)];
            const auto* mind = registry.try_get<Mind>(entity);
            const auto* walker = registry.try_get<Walker>(entity);
            look.running = (mind != nullptr && mind->action == Action::Flee) ||
                           (walker != nullptr && walker->pace > 1.3f);
            if (const auto* weapon = registry.try_get<Weapon>(entity)) {
                look.weapon = weapon->kind;
                look.aiming = combat != nullptr && combat->target != kInvalidId && isFirearm(weapon->kind);
            }
            if (person != nullptr && person->asleep) {
                frame.pitch = PI * 0.5f; // спит под открытым небом — на спине
                frame.origin.y += 0.12f * frame.scale;
            }
        }
        emitHuman(frame, look, gait);
    }
    setModelFade(1.0f);
    if (beacon) {
        emitBeacon({beacon->x, beacon->y, beacon->z}, beacon->w, kBeaconColor);
    }
}

} // namespace lw
