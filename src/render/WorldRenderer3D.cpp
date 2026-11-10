#include "render/WorldRenderer3D.h"

#include "render/CameraController.h"
#include "render/Lighting.h"
#include "render/Models3D.h"
#include "render/Palette.h"
#include "render/Terrain3D.h"
#include "render/TreeLayout.h"
#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Family.h"
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
// Раз в столько кадров аниматор забывает тех, кого давно нет в кадре.
constexpr std::uint32_t kForgetFrames = 600;
// Значки эмоций — не дальше этого, метры; высота — не меньше kEmotePixels на экране.
constexpr float kEmoteRange = 150.0f;
constexpr float kEmoteSize = 0.42f;
constexpr float kEmotePixels = 14.0f;
// Тени — в квадрате вокруг точки взгляда: полусторона растёт с расстоянием
// камеры ступенями (чтобы при зуме тени не дрожали), дальше kShadowsOff — теней нет.
constexpr float kShadowMinRadius = 40.0f;
constexpr float kShadowMaxRadius = 1600.0f;
constexpr float kShadowStep = 1.2f;
constexpr float kShadowsOff = 4000.0f;
// Внутри помещения рассеянный свет неба — эта доля от уличного (12а.14).
constexpr float kIndoorAmbient = 0.95f;
// Дымка — слой у земли: с высоты H метров её видно в 1 + H / kHazeHeight раз меньше.
constexpr float kHazeHeight = 1500.0f;

constexpr Color kTrousersColor = {58, 62, 78, 255};
constexpr Color kZombieSkin = {138, 156, 112, 255};
constexpr Color kZombieTrousers = {52, 56, 48, 255};
constexpr Color kDeadSkin = {176, 164, 150, 255};
constexpr Color kSkinTones[] = {{232, 196, 164, 255}, {198, 150, 112, 255}, {140, 98, 70, 255}};
constexpr Color kHairTones[] = {{40, 30, 24, 255},    {92, 62, 38, 255},  {150, 110, 62, 255},
                                {204, 172, 112, 255}, {62, 62, 64, 255},  {168, 168, 168, 255}};
constexpr Color kZombieHair = {64, 66, 58, 255};

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

// Витрина поз: ряды по 8 в точке center, лицом на юг (к камере с юга).
void emitPoseGallery(Vector3 center, const Terrain3D& terrain, float t, int only) {
    using namespace pose;
    const Gait gait{t * 4.0f, 1.0f};
    struct Entry {
        HumanPose pose;
        Prop prop = Prop::None;
        bool zombie = false;
        WeaponKind weapon = WeaponKind::Fists;
        bool aiming = false;
        bool bag = false;
        Emote emote = Emote::None;
    };
    const float hit = std::fmod(t, 1.5f);
    const Entry entries[] = {
        {stand(t, 1)}, {lookAround(t, 2, 0.0f)}, {stretch(std::fmod(t, 5.0f))}, {armsCrossed(t, 3)},
        {handsOnHips(t, 4)}, {scratchHead(t)}, {walk(gait, 5)}, {run(gait)},
        {sneak(gait), Prop::None, false, WeaponKind::Rifle, true}, {patrol(gait)}, {limp(gait)}, {backAway(gait)},
        {pocketsWalk(gait)}, {sitGround(t, 6)}, {sitKnees(t, 7)}, {sitStump(t, 8), Prop::Stump},
        {lieBack(t, 9)}, {leanWall(t, 10)}, {eat(t, false), Prop::Food}, {eat(t, true), Prop::Food},
        {drinkBottle(t), Prop::Bottle}, {scoopWater(t)}, {sleepBack(t)}, {sleepCurled(t)},
        {dig(t), Prop::Shovel}, {reap(t), Prop::Scythe}, {counter(t, 11), Prop::Counter}, {examine(t)},
        {clipboard(t), Prop::Clipboard}, {lecture(t, 12)}, {patrolStand(t, 13)}, {rummage(t)},
        {reachHigh(t)}, {pick(t, false), Prop::None, false, WeaponKind::Fists, false, true},
        {pick(t, true), Prop::None, false, WeaponKind::Fists, false, true}, {butcher(t), Prop::None, false, WeaponKind::Knife},
        {talk(t, 14, 0.5f)}, {nod(t)}, {laugh(std::fmod(t, 4.0f))}, {listen(t, 15)},
        {argue(t, 16), Prop::None, false, WeaponKind::Fists, false, false, Emote::Anger}, {hug(t), Prop::None, false,
        WeaponKind::Fists, false, false, Emote::Heart}, {offer(std::fmod(t, 3.0f)), Prop::Bundle},
        {attack(guard(t, 0), hit, 0)}, {attack(guard(t, 1), hit, 1), Prop::None, false, WeaponKind::Knife},
        {attack(guard(t, 2), hit, 2), Prop::None, false, WeaponKind::Axe},
        {attack(aim(t, 3), hit, 3), Prop::None, false, WeaponKind::Pistol, true},
        {attack(aim(t, 4), hit, 4), Prop::None, false, WeaponKind::Rifle, true},
        {reload(t, 4), Prop::None, false, WeaponKind::Shotgun, true}, {bandage(t), Prop::Bandage},
        {holdWound(t)}, {kneelGrief(t, true), Prop::None, false, WeaponKind::Fists, false, false, Emote::Tear},
        {standGrief(t)}, {cower(t), Prop::None, false, WeaponKind::Fists, false, false, Emote::Alarm},
        {tremble(stand(t, 17), t, 1.0f)}, {corpse(18)}, {twitch(corpse(19), t, 1.0f)},
        {zombieWalk(gait, 0, false), Prop::None, true}, {zombieWalk(gait, 1, false), Prop::None, true},
        {zombieWalk(gait, 2, false), Prop::None, true}, {zombieWalk(gait, 3, true), Prop::None, true},
        {zombieStand(t, 1), Prop::None, true}, {zombieFeed(t), Prop::None, true}, {zombieBang(t), Prop::None, true},
        {zombieAttack(zombieReach(t, 0), hit), Prop::None, true},
        {zombieRise(std::fmod(t, kRiseSeconds + 1.0f) / kRiseSeconds, 20, 0), Prop::None, true},
    };
    constexpr int kColumns = 8;
    constexpr float kSpacing = 2.4f;
    const int count = static_cast<int>(std::size(entries));
    const int rows = (count + kColumns - 1) / kColumns;
    for (int i = 0; i < count; ++i) {
        const Entry& entry = entries[i];
        if (only >= 0 && i / kColumns != only) {
            continue;
        }
        const float x = center.x + (static_cast<float>(i % kColumns) - 0.5f * (kColumns - 1)) * kSpacing;
        const float row = only >= 0 ? 0.0f : static_cast<float>(i / kColumns) - 0.5f * static_cast<float>(rows);
        const float z = center.z + row * kSpacing;
        ModelFrame frame;
        frame.origin = {x, terrain.standAt(x, z), z};
        HumanLook look;
        look.shirt = entry.zombie ? Color{110, 120, 96, 255} : Color{70, 110, 170, 255};
        look.trousers = entry.zombie ? kZombieTrousers : kTrousersColor;
        look.skin = entry.zombie ? kZombieSkin : kSkinTones[i % 3];
        look.hair = entry.zombie ? kZombieHair : kHairTones[i % std::size(kHairTones)];
        look.zombie = entry.zombie;
        look.weapon = entry.weapon;
        look.aiming = entry.aiming;
        look.prop = entry.prop;
        look.bag = entry.bag;
        emitHuman(frame, look, entry.pose);
        if (entry.emote != Emote::None) {
            const float head = pelvisHeight(entry.pose) - kHipHeight + 2.1f;
            emitEmote({x, frame.origin.y + head, z}, 0.0f, entry.emote, 0.42f);
        }
    }
    // Звери: пасётся, прислушивается, чешется, заяц столбиком.
    if (only >= 0 && only != rows) {
        return;
    }
    const AnimalPose animals[] = {graze(t, 1), listenAnimal(t, 0.7f), scratchAnimal(t), sitUpHare(t)};
    const float z = center.z + (only >= 0 ? 0.0f : 0.5f * static_cast<float>(rows) * kSpacing + 2.0f);
    int column = 0;
    for (const AnimalKind kind : {AnimalKind::Deer, AnimalKind::Boar, AnimalKind::Hare}) {
        for (const AnimalPose& pose : animals) {
            const float x = center.x + (static_cast<float>(column) - 5.5f) * 2.6f;
            ModelFrame frame;
            frame.origin = {x, terrain.standAt(x, z), z};
            frame.yaw = PI * 0.5f;
            emitAnimal(frame, kind, {150, 110, 70, 255}, {}, pose, kind == AnimalKind::Deer);
            ++column;
        }
    }
}

} // namespace

WorldRenderer3D::WorldRenderer3D(MapRenderer& textures, const Map& map)
    : land_(map), terrain_(map, textures, land_), trees_(map, terrain_, land_), props_(map, terrain_, land_),
      interior_(terrain_) {
    // Прямой пролёт — плита вдоль дороги от берега до берега, перила по обеим
    // сторонам, опоры до дна. Пролёт не по прямой — настил по клеткам, перила
    // по краям над открытой водой, опоры через клетку.
    const auto part = [this](Vector3 center, Vector3 half, float yaw, Color color, bool bottom = false) {
        bridgeParts_.push_back({center, half, yaw, color, bottom});
    };
    const auto box = [&part](Vector3 min, Vector3 max, Color color, bool bottom = false) {
        part(Vector3Scale(Vector3Add(min, max), 0.5f), Vector3Scale(Vector3Subtract(max, min), 0.5f), 0.0f, color,
             bottom);
    };
    for (const Terrain3D::Span& span : terrain_.spans()) {
        if (!span.straight) {
            continue;
        }
        const bool asphalt = span.road >= Road::Asphalt;
        const Color rail = asphalt ? kConcreteColor : kTimberColor;
        const float deck = span.deck;
        const Vector2 delta = Vector2Subtract(span.b, span.a);
        const float length = Vector2Length(delta);
        const Vector2 along = Vector2Scale(delta, 1.0f / length);
        const Vector2 across = {along.y, -along.x};
        // Ось Z коробки — вдоль моста (как у ModelFrame: yaw 0 — на +Z мира).
        const float yaw = std::atan2(along.x, along.y);
        const float halfWidth = span.halfWidth;
        const Vector2 mid = Vector2Scale(Vector2Add(span.a, span.b), 0.5f);
        part({mid.x, deck - 0.5f * kBridgeThickness, mid.y}, {halfWidth, 0.5f * kBridgeThickness, 0.5f * length}, yaw,
             asphalt ? kBridgeAsphalt : kBridgeWood, true);
        for (const float side : {-1.0f, 1.0f}) {
            const float t = asphalt ? 0.25f : 0.12f;
            const Vector2 edge = Vector2Add(mid, Vector2Scale(across, side * (halfWidth - 0.5f * t)));
            if (asphalt) {
                part({edge.x, deck + 0.4f, edge.y}, {0.5f * t, 0.4f, 0.5f * length}, yaw, rail);
                continue;
            }
            // Деревянные перила: верхний брус и столбики через 2 м.
            part({edge.x, deck + 0.875f, edge.y}, {0.5f * t, 0.075f, 0.5f * length}, yaw, rail);
            const int posts = std::max(1, static_cast<int>(length / 2.0f));
            for (int i = 0; i <= posts; ++i) {
                const float s = -0.5f * length + length * static_cast<float>(i) / static_cast<float>(posts);
                const Vector2 at = Vector2Add(edge, Vector2Scale(along, s));
                part({at.x, deck + 0.475f, at.y}, {0.075f, 0.475f, 0.075f}, yaw, rail);
            }
        }
        // Опоры там, где под настилом не земля.
        const float step = asphalt ? 8.0f : 4.0f;
        const int piers = static_cast<int>(length / step);
        for (int i = 1; i < piers; ++i) {
            const float s = -0.5f * length + length * static_cast<float>(i) / static_cast<float>(piers);
            const Vector2 at = Vector2Add(mid, Vector2Scale(along, s));
            const float bottom = terrain_.groundAt(at.x, at.y) - 0.5f;
            const float top = deck - kBridgeThickness;
            if (top - bottom < 0.8f) {
                continue;
            }
            const float y = 0.5f * (top + bottom);
            const float h = 0.5f * (top - bottom);
            if (asphalt) {
                part({at.x, y, at.y}, {0.8f * halfWidth, h, 0.5f}, yaw, kConcreteColor);
            } else {
                for (const float side : {-1.0f, 1.0f}) {
                    const Vector2 leg = Vector2Add(at, Vector2Scale(across, side * (halfWidth - 0.4f)));
                    part({leg.x, y, leg.y}, {0.15f, h, 0.15f}, yaw, kTimberColor);
                }
            }
        }
    }

    const auto key = [&map](Cell cell) {
        return static_cast<std::int64_t>(cell.y) * map.width() + cell.x;
    };
    std::unordered_set<std::int64_t> bridged;
    for (const Terrain3D::Bridge& bridge : terrain_.bridges()) {
        bridged.insert(key(bridge.cell));
    }
    for (const Terrain3D::Bridge& bridge : terrain_.bridges()) {
        if (terrain_.spans()[static_cast<std::size_t>(bridge.span)].straight) {
            continue;
        }
        const bool asphalt = bridge.road >= Road::Asphalt;
        const float x0 = static_cast<float>(bridge.cell.x) * Map::kCellSize;
        const float z0 = static_cast<float>(bridge.cell.y) * Map::kCellSize;
        const float x1 = x0 + Map::kCellSize;
        const float z1 = z0 + Map::kCellSize;
        const float deck = bridge.deck;
        box({x0, deck - kBridgeThickness, z0}, {x1, deck, z1}, asphalt ? kBridgeAsphalt : kBridgeWood, true);
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
            box(min, max, rail);
            if (!asphalt) {
                // Столбики перил у начала каждого пролёта.
                box({min.x, deck, min.z}, {std::min(max.x, min.x + 0.15f), y1, std::min(max.z, min.z + 0.15f)}, rail);
            }
        }
        if ((bridge.cell.x + bridge.cell.y) % (asphalt ? 3 : 2) == 0) {
            const float cx = (x0 + x1) * 0.5f;
            const float cz = (z0 + z1) * 0.5f;
            const float half = asphalt ? 0.6f : 0.2f;
            const float bottom = terrain_.groundAt(cx, cz) - 0.5f;
            const float top = deck - kBridgeThickness;
            if (top > bottom) {
                box({cx - half, bottom, cz - half}, {cx + half, top, cz + half}, rail);
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

bool WorldRenderer3D::interiorView(const CameraController& camera) const {
    const InteriorLayout* layout = interior_.layout();
    if (layout == nullptr || !camera.is3d()) {
        return false;
    }
    const Vector3 eye = camera.camera3d().position;
    return eye.x > layout->outerX0 && eye.x < layout->outerX1 && eye.z > layout->outerZ0 && eye.z < layout->outerZ1 &&
           eye.y > layout->floor - 0.5f && eye.y < layout->ceiling + 0.5f;
}

void WorldRenderer3D::prepareInterior(const InteriorLayout& layout, Vector3 eye, const SceneLight& light) {
    // Кого камера должна видеть: того, за кем смотрим, и тех, кто с ним в комнате.
    std::vector<Vector2> subjects;
    const Placement* focus = interior_.placementOf(interior_.focus());
    if (focus != nullptr) {
        subjects.push_back({focus->x, focus->z});
        for (const Placement& other : interior_.placements()) {
            if (&other != focus && other.room == focus->room) {
                subjects.push_back({other.x, other.z});
            }
        }
    } else {
        subjects.push_back({(layout.x0 + layout.x1) * 0.5f, (layout.z0 + layout.z1) * 0.5f});
    }
    // Взгляд на target проходит сквозь дверной проём стены (кадр «через проём»,
    // 12а.12): такую стену не прятать — она и есть рамка кадра.
    const auto throughDoor = [&](const InteriorWall& wall, Vector2 target) {
        const float origin[2] = {eye.x, eye.z};
        const float dir[2] = {target.x - eye.x, target.y - eye.z};
        const int across = wall.alongX ? 1 : 0;
        const int along = 1 - across;
        if (std::abs(dir[across]) < 1e-5f) {
            return false;
        }
        const float t = ((wall.lo + wall.hi) * 0.5f - origin[across]) / dir[across];
        if (t <= 0.0f || t >= 1.0f) {
            return false;
        }
        const float at = origin[along] + t * dir[along];
        for (const WallGap& gap : wall.gaps) {
            if (!gap.window && gap.y0 < 0.05f && at > gap.a0 + 0.12f && at < gap.a1 - 0.12f) {
                return true;
            }
        }
        return false;
    };
    // Отрезок от глаза до кого-то из них пересекает прямоугольник (с запасом margin)?
    const auto crosses = [&](float x0, float z0, float x1, float z1, float margin,
                             const InteriorWall* wall = nullptr) {
        x0 -= margin;
        z0 -= margin;
        x1 += margin;
        z1 += margin;
        if (eye.x > x0 && eye.x < x1 && eye.z > z0 && eye.z < z1) {
            return true; // глаз внутри — заслонит всё
        }
        for (const Vector2 target : subjects) {
            if (wall != nullptr && throughDoor(*wall, target)) {
                continue;
            }
            const float origin[2] = {eye.x, eye.z};
            const float dir[2] = {target.x - eye.x, target.y - eye.z};
            const float lo[2] = {x0, z0};
            const float hi[2] = {x1, z1};
            float enter = 0.0f;
            float leave = 1.0f;
            bool miss = false;
            for (int axis = 0; axis < 2 && !miss; ++axis) {
                if (std::abs(dir[axis]) < 1e-6f) {
                    miss = origin[axis] < lo[axis] || origin[axis] > hi[axis];
                    continue;
                }
                float t1 = (lo[axis] - origin[axis]) / dir[axis];
                float t2 = (hi[axis] - origin[axis]) / dir[axis];
                if (t1 > t2) {
                    std::swap(t1, t2);
                }
                enter = std::max(enter, t1);
                leave = std::min(leave, t2);
            }
            // Не у самой цели: сидит у стены — стену не прятать.
            if (!miss && enter < leave && enter < 0.92f) {
                return true;
            }
        }
        return false;
    };
    hiddenWalls_.assign(layout.walls.size(), false);
    for (std::size_t i = 0; i < layout.walls.size(); ++i) {
        const InteriorWall& wall = layout.walls[i];
        const float x0 = wall.alongX ? wall.a0 : wall.lo;
        const float x1 = wall.alongX ? wall.a1 : wall.hi;
        const float z0 = wall.alongX ? wall.lo : wall.a0;
        const float z1 = wall.alongX ? wall.hi : wall.a1;
        hiddenWalls_[i] = crosses(x0, z0, x1, z1, 0.3f, &wall);
    }
    hiddenFurniture_.assign(layout.furniture.size(), false);
    for (std::size_t i = 0; i < layout.furniture.size(); ++i) {
        const Furniture& f = layout.furniture[i];
        const bool tall = f.height + f.base > 1.15f && f.kind != FurnitureKind::Door && f.kind != FurnitureKind::Board;
        if (f.kind == FurnitureKind::Lamp) {
            hiddenFurniture_[i] = Vector2Distance({eye.x, eye.z}, {f.x, f.z}) < 0.6f; // перед объективом
        } else if (tall) {
            hiddenFurniture_[i] = crosses(f.minX(), f.minZ(), f.maxX(), f.maxZ(), 0.1f);
        }
    }
    // Лампы — вечером и ночью, если кто-то дома не спит.
    const bool awake = std::any_of(interior_.placements().begin(), interior_.placements().end(),
                                   [](const Placement& p) { return p.what != Occupation::Sleep; });
    const float dusk = std::clamp((light.night - 0.05f) / 0.5f, 0.0f, 1.0f);
    lampsOn_ = awake && dusk > 0.15f;
    std::vector<SceneShading::Lamp> lamps;
    if (lampsOn_) {
        std::vector<const InteriorLamp*> order;
        for (const InteriorLamp& lamp : layout.lamps) {
            order.push_back(&lamp);
        }
        const Vector2 center = subjects.front();
        const int room = focus != nullptr ? focus->room : -1;
        std::sort(order.begin(), order.end(), [&](const InteriorLamp* a, const InteriorLamp* b) {
            const float da = Vector2Distance({a->x, a->z}, center) - (a->room == room ? 100.0f : 0.0f);
            const float db = Vector2Distance({b->x, b->z}, center) - (b->room == room ? 100.0f : 0.0f);
            return da < db;
        });
        const float strength = 0.4f + 0.45f * dusk;
        for (std::size_t i = 0; i < order.size() && i < static_cast<std::size_t>(SceneShading::kMaxLamps); ++i) {
            lamps.push_back({{order[i]->x, order[i]->y, order[i]->z}, 6.5f,
                             {1.0f * strength, 0.76f * strength, 0.48f * strength}});
        }
    }
    shading_.setLamps(lamps, kIndoorAmbient);
}

void WorldRenderer3D::updateBuildingBounds(const World& world) {
    if (!land_.hasSettlements()) {
        // Дворы и столбы — по зданиям: раскладка вне леса меняется.
        land_.addSettlements(world);
        trees_.reset();
        props_.reset();
    }
    // Здания не меняются: модели и сетки — один раз.
    buildings_.build(world, terrain_);
}

void WorldRenderer3D::draw(const World& world, const CameraController& camera, float alpha) {
    const Camera3D& view = camera.camera3d();
    const Vector3 eye = view.position;
    ++frame_;
    updateBuildingBounds(world);
    interior_.update(world);
    dt_ = std::min(GetFrameTime(), 0.1f);
    animator_.beginFrame(world, dt_, alpha);
    const float snow = snowCover(world);
    const Environment environment = environmentAt(world);

    // Свет кадра: солнце или луна, небо, дымка (сверху её видно меньше).
    const SceneLight light = sceneLightAt(environment, world.weather());
    const float height = std::max(0.0f, eye.y - terrain_.surfaceAt(eye.x, eye.z));
    shading_.setLight(light, eye, 1.0f / (1.0f + height / kHazeHeight));
    if (const InteriorLayout* layout = interior_.layout()) {
        prepareInterior(*layout, eye, light);
    } else {
        lampsOn_ = false;
        shading_.setLamps({}, kIndoorAmbient);
    }

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

    // Коробки зданий — сетками по чанкам; открытое изнутри — комнатами.
    buildings_.update(snow, interior_.layout() != nullptr ? interior_.building() : kInvalidId);

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
            buildings_.drawShadows(shading_);
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

    // Всё непрозрачное — одним набором четырёхугольников без текстуры; коробки
    // зданий — готовыми сетками тем же шейдером.
    shading_.beginModels();
    buildingsDrawn_ = buildings_.drawShells(shading_);
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    emitScene(world, camera, alpha, false);
    rlEnd();
    rlSetTexture(0);
    rlDrawRenderBatchActive();
    shading_.endModels();

    // Забыть облик тех, кого давно нет в кадре.
    if (frame_ % kForgetFrames == 0) {
        animator_.forgetStale();
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
    const float snow = snowCover(world);

    const entt::registry& registry = world.registry();
    if (!shadowPass) {
        creaturesDrawn_ = 0;
    }
    const InteriorLayout* interior = interior_.layout();
    if (interior != nullptr) {
        // Открытое здание — комнатами изнутри (в карту теней — стены с окнами и потолок).
        InteriorLook look;
        look.hiddenWalls = &hiddenWalls_;
        look.hiddenFurniture = &hiddenFurniture_;
        look.lampsOn = lampsOn_;
        look.shadowPass = shadowPass;
        emitInterior(*interior, look);
    }
    // Коробки зданий — в сетках (Buildings3D); здесь только окна, двери и крыльца
    // ближних: мелочь на стенах тени не отбрасывает.
    if (!shadowPass) {
        buildings_.forEachNear({eye.x, eye.z}, drawDistance_, [&](const Buildings3D::Entry& entry) {
            if (interior != nullptr && entry.id == interior_.building()) {
                return;
            }
            const BuildingModel& base = entry.model;
            const Vector2 center = {(base.x0 + base.x1) * 0.5f, (base.z0 + base.z1) * 0.5f};
            const float top = entry.bounds.max.y;
            if (skip({center.x, (base.low + top) * 0.5f, center.y},
                     std::max(base.x1 - base.x0, base.z1 - base.z0) + top - base.low)) {
                return;
            }
            // Окно меньше нескольких пикселей мелькало бы при движении камеры: у
            // предела детали тают в цвет стены.
            const float pixels = camera.zoomAt(center);
            const float distance = Vector3Distance(eye, {center.x, base.low, center.y});
            if (pixels < kDetailPixels || distance >= drawDistance_) {
                return;
            }
            BuildingModel model = base;
            model.snow = snow;
            model.detailed = true;
            model.detail = std::clamp((pixels - kDetailPixels) / (2.0f * kDetailPixels), 0.0f, 1.0f);
            // Окно — плоскость у самой стены. Вдали буфер глубины различает
            // только слои в d² / (near · 2²⁴) друг от друга; окно отходит от стены
            // на втрое больше, иначе стена и окно мерцают, перекрывая друг друга.
            model.decalOut = std::max(0.04f, 3.0f * distance * distance / (nearPlane * 16777216.0f));
            model.eye = eye;
            model.lights = litBuildings_.contains(entry.id) ? lights_ : 0.0f;
            emitBuildingDetails(model);
        });
    }

    // Мосты: настил над водой по клеткам дороги, перила и опоры; вдали тают.
    const float bridgeRange = kBridgeRange * drawDistance_;
    const auto bridgeFade = [&](Vector3 point) {
        return fadeAt(Vector3Distance(eye, point), bridgeRange);
    };
    for (const Part& part : bridgeParts_) {
        const float fade = bridgeFade(part.center);
        if (fade > 0.0f && !skip(part.center, Vector3Length(part.half))) {
            setModelFade(fade);
            emitTurnedBox(part.center, part.half, part.yaw, part.color, part.bottom);
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
        const EntityId id = registry.get<Id>(entity).value;
        // Дальше дальности прорисовки — не рисуется (кроме выбранного), у края тает.
        // По плоскости — до высоты: высоты вдали считаются блоками по требованию
        // (Terrain3D), и спрашивать их у всех существ карты дорого.
        const bool highlighted = id == highlight_;
        if (!highlighted && Vector2Distance({eye.x, eye.z}, at) >= drawDistance_) {
            continue;
        }
        const float stand = terrain_.standAt(at.x, at.y);
        if (skip({at.x, stand, at.y}, 50.0f)) {
            continue;
        }
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
                // Сидящие клюют — каждая в свой черёд.
                const float peck =
                    std::pow(std::max(0.0f, std::sin(wingTime * 0.35f + 1.7f * static_cast<float>(i))), 4.0f);
                emitBird(frame, flock->flying, flap, peck);
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
        const auto* person = registry.try_get<Person>(entity);
        const Placement* placed = nullptr;
        if (person != nullptr && person->indoors) {
            // В доме снаружи не видно; в открытом здании — на своём месте в комнате.
            placed = interior != nullptr ? interior_.placementOf(id) : nullptr;
            if (placed == nullptr) {
                continue;
            }
            at = {placed->x, placed->z};
            velocity = {placed->vx, placed->vz}; // идёт к новому месту — шаг, иначе стоит
        }
        // Поза и движение — по занятию (render/Animator); тела падают, зомби встают.
        const Figure& figure = animator_.animate(world, entity, id, at.x, at.y, velocity.x, velocity.y, placed);
        const Vector2 drawn = {at.x + figure.offsetX, at.y + figure.offsetY};
        ModelFrame frame;
        if (placed != nullptr) {
            // Лёжа — на матрасе или диване; сидя — сиденье уже в позе.
            frame.origin = {drawn.x, interior->floor + placed->height * lyingness(figure.human), drawn.y};
            // На боку тело поворачивается вокруг продольной оси: голова — к изголовью.
            frame.yaw = figure.heading - figure.human.bodyRoll;
        } else {
            frame.origin = {drawn.x, terrain_.standAt(drawn.x, drawn.y), drawn.y};
            frame.yaw = figure.heading;
        }
        creaturesDrawn_ += shadowPass ? 0 : 1;

        if (figure.body == Figure::Body::AnimalCorpse || figure.body == Figure::Body::Animal) {
            if (const auto* corpse = registry.try_get<Corpse>(entity)) {
                frame.roll = PI * 0.5f * figure.fall; // валится на бок
                frame.origin.y += carcassLift(corpse->animal) * figure.fall * frame.scale;
                emitAnimal(frame, corpse->animal, corpseColor(corpse->kind), {}, figure.animal, false);
            } else {
                const auto& animal = registry.get<Animal>(entity);
                emitAnimal(frame, animal.kind, creatureColor(registry, entity), figure.gait, figure.animal,
                           (bits & 1u) != 0u);
            }
            continue;
        }

        HumanLook look;
        look.simple = 1.75f * camera.zoomAt(at) < kSimplePixels;
        look.hair = kHairTones[(bits >> 16) % std::size(kHairTones)];
        look.skin = kSkinTones[(bits >> 8) % std::size(kSkinTones)];
        look.trousers = kTrousersColor;
        look.prop = figure.prop;
        look.bag = figure.bag;
        look.aiming = figure.aiming;
        if (const auto* corpse = registry.try_get<Corpse>(entity)) {
            look.shirt = corpseColor(corpse->kind);
            if (corpse->kind == CorpseKind::Zombie) {
                look.trousers = kZombieTrousers;
                look.skin = kZombieSkin;
                look.hair = kZombieHair;
            } else {
                look.skin = kDeadSkin;
            }
        } else {
            look.shirt = creatureColor(registry, entity);
            if (registry.all_of<Zombie>(entity)) {
                look.zombie = true;
                look.trousers = kZombieTrousers;
                look.skin = kZombieSkin;
                look.hair = kZombieHair;
            }
        }
        if (const auto* weapon = registry.try_get<Weapon>(entity)) {
            look.weapon = weapon->kind;
        }
        // Дети (этап 7.3) ниже ростом: та же модель в масштабе — от метра в 4
        // года до взрослого роста к 18.
        if (const int years = ageOf(registry, entity); registry.all_of<Age>(entity) && years < kAdultAge) {
            frame.scale = std::clamp((1.0f + 0.055f * static_cast<float>(years - 4)) / 1.75f, 0.55f, 1.0f);
        }
        setModelIndoor(placed != nullptr);
        emitHuman(frame, look, figure.human);
        setModelIndoor(false);

        // Значок эмоции над головой — в важный момент: всплывает, качается и тает.
        if (!shadowPass && figure.emote != Emote::None && distance < kEmoteRange) {
            const float age = figure.emoteAge;
            const float pop = pose::smooth(age / 0.2f);
            const float size = std::max(kEmoteSize, kEmotePixels / camera.zoomAt(at)) * pop;
            const float head = pelvisHeight(figure.human) - kHipHeight + 1.95f;
            const float bob = 0.05f * std::sin(age * 4.0f);
            setModelFade(fade * std::clamp((Animator::kEmoteSeconds - age) / 0.6f, 0.0f, 1.0f));
            emitEmote({drawn.x, frame.origin.y + head + 0.15f + bob, drawn.y},
                      headingOf(eye.x - drawn.x, eye.z - drawn.y), figure.emote, size);
            setModelFade(fade);
        }
    }
    setModelFade(1.0f);
    if (beacon) {
        emitBeacon({beacon->x, beacon->y, beacon->z}, beacon->w, kBeaconColor);
    }
    if (poseGallery_) {
        emitPoseGallery(view.target, terrain_, static_cast<float>(GetTime()), *poseGallery_);
    }
}

} // namespace lw
