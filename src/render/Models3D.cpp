#include "render/Models3D.h"

#include "render/Palette.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

Color shaded(Color color, float factor) {
    const auto channel = [factor](unsigned char value) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(value) * factor, 0.0f, 255.0f));
    };
    return {channel(color.r), channel(color.g), channel(color.b), color.a};
}

Color mix(Color a, Color b, float t) {
    const auto channel = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), a.a};
}

// Сустав: поворот части вокруг точки pivot (в осях модели) — roll вокруг
// продольной оси Z (> 0 — конец по +X вверх), pitch вокруг поперечной оси X
// (> 0 — нижний конец вперёд, нос вверх), yaw вокруг вертикали (> 0 — перёд
// к +X); затем — поворот родителя.
struct Joint {
    Vector3 pivot{};
    float pitch = 0.0f;
    float roll = 0.0f;
    const Joint* parent = nullptr;
    float yaw = 0.0f;
};

Vector3 rotatePitch(Vector3 p, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {p.x, p.y * c + p.z * s, -p.y * s + p.z * c};
}

Vector3 rotateRoll(Vector3 p, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {p.x * c - p.y * s, p.x * s + p.y * c, p.z};
}

Vector3 rotateYaw(Vector3 p, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
}

Vector3 applyJoint(const Joint* joint, Vector3 p) {
    for (; joint != nullptr; joint = joint->parent) {
        Vector3 q = Vector3Subtract(p, joint->pivot);
        if (joint->roll != 0.0f) {
            q = rotateRoll(q, joint->roll);
        }
        if (joint->pitch != 0.0f) {
            q = rotatePitch(q, joint->pitch);
        }
        if (joint->yaw != 0.0f) {
            q = rotateYaw(q, joint->yaw);
        }
        p = Vector3Add(q, joint->pivot);
    }
    return p;
}

// Доля растворения граней (1 − видимость): texcoord.y для шейдера моделей.
float gDissolve = 0.0f;
// Все следующие грани светятся сами (значки эмоций).
bool gGlow = false;
// Все следующие грани — внутри помещения (12а.14).
bool gIndoor = false;

// Грани коробки.
enum Faces : unsigned { kTop = 1u, kBottom = 2u, kSides = 4u, kAll = 7u };

// Вывод граней модели в открытый rlBegin(RL_QUADS): точки в осях модели
// переводятся в мир через суставы и ModelFrame. Все повороты собственные
// (без отражений), поэтому обход против часовой стрелки сохраняется.
class Emitter {
public:
    explicit Emitter(const ModelFrame& frame)
        : frame_(frame), cosYaw_(std::cos(frame.yaw)), sinYaw_(std::sin(frame.yaw)) {}

    Vector3 toWorld(Vector3 p, const Joint* joint) const {
        p = applyJoint(joint, p);
        if (frame_.roll != 0.0f) {
            p = rotateRoll(p, frame_.roll);
        }
        if (frame_.pitch != 0.0f) {
            p = rotatePitch(p, frame_.pitch);
        }
        p = Vector3Scale(p, frame_.scale);
        return {frame_.origin.x + p.x * cosYaw_ + p.z * sinYaw_, frame_.origin.y + p.y,
                frame_.origin.z - p.x * sinYaw_ + p.z * cosYaw_};
    }

    // Четырёхугольник a-b-c-d против часовой стрелки снаружи; треугольник — d = c.
    // glow — светится сам, свет на него не влияет (окно ночью).
    void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color color, const Joint* joint = nullptr,
              bool glow = false) const {
        const Vector3 wa = toWorld(a, joint);
        const Vector3 wb = toWorld(b, joint);
        const Vector3 wc = toWorld(c, joint);
        const Vector3 wd = toWorld(d, joint);
        const Vector3 normal =
            Vector3Normalize(Vector3CrossProduct(Vector3Subtract(wb, wa), Vector3Subtract(wc, wa)));
        // Освещает шейдер (render/Lighting3D) по нормали грани.
        rlNormal3f(normal.x, normal.y, normal.z);
        rlTexCoord2f(glow || gGlow ? 1.0f : gIndoor ? 2.0f : 0.0f, gDissolve);
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlVertex3f(wa.x, wa.y, wa.z);
        rlVertex3f(wb.x, wb.y, wb.z);
        rlVertex3f(wc.x, wc.y, wc.z);
        rlVertex3f(wd.x, wd.y, wd.z);
    }

    void box(Vector3 min, Vector3 max, Color color, const Joint* joint = nullptr, unsigned faces = kAll,
             Color top = {}) const {
        const float x0 = min.x;
        const float y0 = min.y;
        const float z0 = min.z;
        const float x1 = max.x;
        const float y1 = max.y;
        const float z1 = max.z;
        if ((faces & kTop) != 0u) {
            quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, top.a != 0 ? top : color, joint);
        }
        if ((faces & kSides) != 0u) {
            quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, color, joint);
            quad({x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, color, joint);
            quad({x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, color, joint);
            quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, color, joint);
        }
        if ((faces & kBottom) != 0u) {
            quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, color, joint);
        }
    }

    // Коробка, симметричная по X: правая половина [x0, x1] и её отражение.
    void pair(Vector3 min, Vector3 max, Color color, const Joint* left, const Joint* right) const {
        box(min, max, color, left);
        box({-max.x, min.y, min.z}, {-min.x, max.y, max.z}, color, right);
    }

private:
    const ModelFrame& frame_;
    float cosYaw_;
    float sinYaw_;
};

constexpr Color kGunColor = {44, 42, 40, 255};
constexpr Color kSteelColor = {150, 154, 160, 255};
constexpr Color kWoodColor = {112, 80, 52, 255};

} // namespace

float headingOf(float dx, float dy) {
    return std::atan2(dx, dy);
}

namespace {

constexpr Color kShoeColor = {52, 40, 32, 255};
constexpr Color kBottleColor = {74, 132, 96, 255};
constexpr Color kBreadColor = {196, 150, 90, 255};
constexpr Color kClothColor = {156, 124, 84, 255};
constexpr Color kBandageColor = {236, 236, 228, 255};
constexpr Color kPaperColor = {232, 230, 220, 255};
constexpr Color kBarkColor = {96, 72, 50, 255};
constexpr Color kStumpTop = {168, 134, 92, 255};
constexpr Color kBagColor = {118, 92, 58, 255};

// Кисть правой руки в осях предплечья.
constexpr float kHandX = -0.255f;
constexpr float kHandY = 0.86f;

bool inRightHand(Prop prop) {
    switch (prop) {
    case Prop::Food:
    case Prop::Bottle:
    case Prop::Shovel:
    case Prop::Scythe:
    case Prop::Bundle:
    case Prop::Bandage:
    case Prop::Clipboard:
        return true;
    default:
        return false;
    }
}

// Что в правой руке: forearm — сустав предплечья, toolPitch — наклон черенка.
void emitHandProp(const Emitter& e, Prop prop, const Joint* forearm, float toolPitch) {
    const float hx = kHandX;
    const float hy = kHandY;
    switch (prop) {
    case Prop::Food:
        e.box({hx - 0.035f, hy - 0.06f, 0.0f}, {hx + 0.035f, hy + 0.02f, 0.09f}, kBreadColor, forearm);
        break;
    case Prop::Bottle:
        e.box({hx - 0.03f, hy - 0.12f, 0.02f}, {hx + 0.03f, hy + 0.08f, 0.08f}, kBottleColor, forearm);
        e.box({hx - 0.015f, hy + 0.08f, 0.035f}, {hx + 0.015f, hy + 0.12f, 0.065f}, kPaperColor, forearm);
        break;
    case Prop::Bundle:
        e.box({hx - 0.11f, hy - 0.12f, 0.02f}, {hx + 0.11f, hy + 0.04f, 0.22f}, kClothColor, forearm);
        break;
    case Prop::Bandage:
        e.box({hx - 0.035f, hy - 0.06f, 0.0f}, {hx + 0.035f, hy + 0.01f, 0.07f}, kBandageColor, forearm);
        break;
    case Prop::Clipboard:
        e.box({hx - 0.12f, hy - 0.01f, -0.02f}, {hx + 0.12f, hy + 0.01f, 0.3f}, kWoodColor, forearm);
        e.box({hx - 0.1f, hy + 0.01f, 0.0f}, {hx + 0.1f, hy + 0.016f, 0.27f}, kPaperColor, forearm);
        break;
    case Prop::Shovel:
    case Prop::Scythe: {
        const Joint tool{{hx, hy, 0.0f}, toolPitch, 0.0f, forearm};
        if (prop == Prop::Shovel) {
            e.box({hx - 0.02f, hy - 1.05f, -0.02f}, {hx + 0.02f, hy + 0.3f, 0.02f}, kWoodColor, &tool);
            e.box({hx - 0.12f, hy - 1.32f, -0.015f}, {hx + 0.12f, hy - 1.03f, 0.015f}, kSteelColor, &tool);
        } else {
            e.box({hx - 0.02f, hy - 1.3f, -0.02f}, {hx + 0.02f, hy + 0.25f, 0.02f}, kWoodColor, &tool);
            e.box({hx - 0.75f, hy - 1.33f, 0.0f}, {hx, hy - 1.27f, 0.07f}, kSteelColor, &tool);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

void emitHuman(const ModelFrame& frame, const HumanLook& look, const HumanPose& pose) {
    // Таз — на своей высоте: сидит, присел, на коленях, лежит.
    ModelFrame lowered = frame;
    lowered.origin.y += (pelvisHeight(pose) - kHipHeight) * frame.scale;
    const Emitter e(lowered);
    const Emitter ground(frame);
    // Всё тело поворачивается вокруг таза: на спину, ничком, на бок.
    const Joint body{{0.0f, kHipHeight, 0.0f}, pose.bodyPitch, pose.bodyRoll};
    const Joint torso{{0.0f, kHipHeight, 0.0f}, -pose.lean, -pose.side, &body, pose.twist};
    const Joint head{{0.0f, 1.5f, 0.0f}, -pose.headPitch, -pose.headTilt, &torso, pose.headYaw};

    // Что стоит рядом: прилавок перед продавцом, чурбак под сидящим.
    if (look.prop == Prop::Counter && !look.simple) {
        ground.box({-0.65f, 0.0f, 0.5f}, {0.65f, 0.9f, 0.95f}, kWoodColor, nullptr, kTop | kSides);
        ground.box({-0.7f, 0.9f, 0.45f}, {0.7f, 0.96f, 1.0f}, shaded(kWoodColor, 1.25f), nullptr, kAll);
        ground.box({-0.5f, 0.96f, 0.6f}, {-0.25f, 1.12f, 0.8f}, {196, 70, 52, 255}, nullptr, kTop | kSides);
        ground.box({0.1f, 0.96f, 0.58f}, {0.4f, 1.06f, 0.85f}, {220, 190, 90, 255}, nullptr, kTop | kSides);
    } else if (look.prop == Prop::Stump) {
        ground.box({-0.21f, 0.0f, -0.22f}, {0.21f, 0.43f, 0.2f}, kBarkColor, nullptr, kTop | kSides, kStumpTop);
    }

    if (look.simple) {
        // Издали в несколько пикселей: силуэт и цвет одежды.
        e.box({-0.17f, 0.0f, -0.08f}, {0.17f, 0.92f, 0.08f}, look.trousers, &body);
        e.box({-0.2f, 0.88f, -0.11f}, {0.2f, 1.48f, 0.11f}, look.shirt, &torso);
        e.box({-0.11f, 1.5f, -0.11f}, {0.11f, 1.74f, 0.12f}, look.skin, &head);
        return;
    }

    // Ноги: бедро, голень, ботинок. Левая сторона — +X (модель смотрит вдоль +Z).
    const Joint leftHip{{0.095f, kHipHeight, 0.0f}, pose.leftLeg.hip, pose.leftLeg.spread, &body};
    const Joint rightHip{{-0.095f, kHipHeight, 0.0f}, pose.rightLeg.hip, -pose.rightLeg.spread, &body};
    const Joint leftKnee{{0.095f, kHipHeight - kThigh, 0.0f}, -pose.leftLeg.knee, 0.0f, &leftHip};
    const Joint rightKnee{{-0.095f, kHipHeight - kThigh, 0.0f}, -pose.rightLeg.knee, 0.0f, &rightHip};
    e.pair({0.02f, 0.44f, -0.08f}, {0.17f, 0.92f, 0.08f}, look.trousers, &leftHip, &rightHip);
    e.pair({0.025f, 0.07f, -0.07f}, {0.165f, 0.47f, 0.07f}, look.trousers, &leftKnee, &rightKnee);
    e.pair({0.03f, 0.0f, -0.08f}, {0.16f, 0.08f, 0.15f}, kShoeColor, &leftKnee, &rightKnee);

    // Таз, корпус, голова: нос и волосы показывают, куда смотрит.
    e.box({-0.18f, 0.82f, -0.1f}, {0.18f, 0.96f, 0.1f}, look.trousers, &body);
    e.box({-0.2f, 0.94f, -0.11f}, {0.2f, 1.48f, 0.11f}, look.shirt, &torso);
    e.box({-0.11f, 1.5f, -0.11f}, {0.11f, 1.74f, 0.12f}, look.skin, &head);
    e.box({-0.022f, 1.57f, 0.12f}, {0.022f, 1.63f, 0.15f}, shaded(look.skin, 0.85f), &head);
    e.box({-0.118f, 1.67f, -0.12f}, {0.118f, 1.775f, 0.1f}, look.hair, &head);
    e.box({-0.118f, 1.53f, -0.125f}, {0.118f, 1.67f, -0.07f}, look.hair, &head);
    if (look.bag) {
        e.box({0.19f, 0.76f, -0.1f}, {0.27f, 0.98f, 0.08f}, kBagColor, &torso);
    }

    // Руки: плечо, предплечье, кисть.
    const Joint leftShoulder{{0.255f, kShoulderHeight, 0.0f}, pose.leftArm.raise, pose.leftArm.out, &torso};
    const Joint rightShoulder{{-0.255f, kShoulderHeight, 0.0f}, pose.rightArm.raise, -pose.rightArm.out, &torso};
    const Joint leftElbow{{0.255f, kShoulderHeight - kUpperArm, 0.0f}, pose.leftArm.elbow, 0.0f, &leftShoulder};
    const Joint rightElbow{{-0.255f, kShoulderHeight - kUpperArm, 0.0f}, pose.rightArm.elbow, 0.0f, &rightShoulder};
    e.pair({0.2f, 1.15f, -0.055f}, {0.31f, 1.46f, 0.055f}, look.shirt, &leftShoulder, &rightShoulder);
    e.pair({0.205f, 0.99f, -0.05f}, {0.305f, 1.17f, 0.05f}, look.shirt, &leftElbow, &rightElbow);
    e.pair({0.21f, 0.83f, -0.045f}, {0.3f, 0.99f, 0.045f}, look.skin, &leftElbow, &rightElbow);

    // Оружие: длинное — за спиной или в руках, пистолет — в кобуре или в руке,
    // нож и топор — в правой руке, а если она занята — на поясе и за спиной.
    const bool handBusy = inRightHand(look.prop);
    const Joint sling{{0.0f, 1.2f, -0.15f}, 0.0f, 0.6f, &torso};
    switch (look.weapon) {
    case WeaponKind::Rifle:
    case WeaponKind::Shotgun: {
        const float length = look.weapon == WeaponKind::Rifle ? 1.05f : 0.9f;
        if (look.aiming && !handBusy) {
            e.box({-0.28f, 0.82f - 0.7f * length, 0.02f}, {-0.23f, 0.82f + 0.3f * length, 0.09f}, kGunColor,
                  &rightElbow);
        } else {
            e.box({-0.025f, 1.2f - 0.5f * length, -0.18f}, {0.025f, 1.2f + 0.5f * length, -0.12f}, kGunColor,
                  &sling);
        }
        break;
    }
    case WeaponKind::Pistol:
        if (look.aiming && !handBusy) {
            e.box({-0.275f, 0.66f, 0.0f}, {-0.235f, 0.84f, 0.1f}, kGunColor, &rightElbow);
        } else {
            e.box({-0.235f, 0.78f, -0.04f}, {-0.2f, 0.92f, 0.04f}, kGunColor, &torso);
        }
        break;
    case WeaponKind::Knife:
        if (!handBusy) {
            e.box({-0.27f, 0.6f, -0.015f}, {-0.245f, 0.8f, 0.015f}, kSteelColor, &rightElbow);
        } else {
            e.box({-0.225f, 0.72f, 0.0f}, {-0.2f, 0.9f, 0.03f}, kSteelColor, &torso);
        }
        break;
    case WeaponKind::Axe:
        if (!handBusy) {
            e.box({-0.275f, 0.42f, 0.05f}, {-0.24f, 0.86f, 0.085f}, kWoodColor, &rightElbow);
            e.box({-0.28f, 0.42f, 0.085f}, {-0.235f, 0.58f, 0.22f}, kSteelColor, &rightElbow);
        } else {
            e.box({-0.02f, 0.95f, -0.16f}, {0.02f, 1.4f, -0.12f}, kWoodColor, &sling);
            e.box({-0.02f, 1.28f, -0.2f}, {0.02f, 1.42f, -0.16f}, kSteelColor, &sling);
        }
        break;
    default:
        break;
    }
    emitHandProp(e, look.prop, &rightElbow, pose.toolPitch);
}

void emitAnimal(const ModelFrame& frame, AnimalKind kind, Color color, Gait gait, const AnimalPose& pose,
                bool antlers) {
    const float step = std::sin(gait.phase) * 0.5f * gait.swing;
    const Color dark = shaded(color, 0.7f);
    const float scratchWave = pose.scratch * std::sin(pose.scratchPhase);
    switch (kind) {
    case AnimalKind::Deer: {
        const Emitter e(frame);
        // Ноги по диагонали: передняя левая с задней правой.
        const Joint frontLeft{{0.0f, 0.8f, 0.45f}, step};
        const Joint frontRight{{0.0f, 0.8f, 0.45f}, -step};
        const Joint backLeft{{0.0f, 0.8f, -0.45f}, -step};
        const Joint backRight{{0.0f, 0.8f, -0.45f}, step};
        e.pair({0.08f, 0.0f, 0.41f}, {0.16f, 0.82f, 0.49f}, dark, &frontLeft, &frontRight);
        e.pair({0.08f, 0.0f, -0.49f}, {0.16f, 0.82f, -0.41f}, dark, &backLeft, &backRight);
        e.box({-0.19f, 0.75f, -0.6f}, {0.19f, 1.2f, 0.58f}, color);
        e.box({-0.06f, 1.02f, -0.66f}, {0.06f, 1.16f, -0.6f}, {226, 220, 206, 255});
        // Шея: пасётся — к земле, прислушивается — вверх, чешется — к боку.
        const Joint neck{{0.0f, 1.05f, 0.5f}, -0.35f - 1.95f * pose.graze + 0.3f * pose.lookUp + 0.12f * scratchWave,
                         0.0f, nullptr, pose.headYaw + 1.9f * pose.scratch};
        e.box({-0.08f, 1.0f, 0.42f}, {0.08f, 1.6f, 0.62f}, color, &neck);
        e.box({-0.09f, 1.5f, 0.5f}, {0.09f, 1.66f, 0.84f}, color, &neck);
        e.pair({0.05f, 1.62f, 0.52f}, {0.1f, 1.72f + 0.05f * pose.ears, 0.56f}, color, &neck, &neck);
        if (antlers) {
            const Joint leftAntler{{0.045f, 1.66f, 0.54f}, 0.0f, -0.45f, &neck};
            const Joint rightAntler{{-0.045f, 1.66f, 0.54f}, 0.0f, 0.45f, &neck};
            e.pair({0.03f, 1.64f, 0.52f}, {0.06f, 1.98f, 0.56f}, {196, 180, 150, 255}, &leftAntler, &rightAntler);
        }
        break;
    }
    case AnimalKind::Boar: {
        // Чешется — трётся боком.
        ModelFrame rub = frame;
        rub.roll += 0.1f * scratchWave;
        const Emitter e(rub);
        const Joint frontLeft{{0.0f, 0.34f, 0.32f}, step};
        const Joint frontRight{{0.0f, 0.34f, 0.32f}, -step};
        const Joint backLeft{{0.0f, 0.34f, -0.4f}, -step};
        const Joint backRight{{0.0f, 0.34f, -0.4f}, step};
        e.pair({0.08f, 0.0f, 0.27f}, {0.17f, 0.36f, 0.37f}, dark, &frontLeft, &frontRight);
        e.pair({0.08f, 0.0f, -0.45f}, {0.17f, 0.36f, -0.35f}, dark, &backLeft, &backRight);
        e.box({-0.22f, 0.3f, -0.55f}, {0.22f, 0.82f, 0.45f}, color);
        // Загривок выше крупа.
        e.box({-0.18f, 0.82f, -0.05f}, {0.18f, 0.9f, 0.4f}, dark);
        const Joint head{{0.0f, 0.62f, 0.42f}, -0.45f * pose.graze + 0.25f * pose.lookUp - 0.35f * pose.charge, 0.0f,
                         nullptr, pose.headYaw};
        e.box({-0.15f, 0.36f, 0.4f}, {0.15f, 0.72f, 0.78f}, color, &head);
        e.box({-0.07f, 0.4f, 0.78f}, {0.07f, 0.54f, 0.9f}, {112, 92, 88, 255}, &head);
        e.pair({0.08f, 0.7f, 0.46f}, {0.13f, 0.79f + 0.03f * pose.ears, 0.5f}, dark, &head, &head);
        break;
    }
    default: {
        // Заяц скачет: всё тело подпрыгивает; садится столбиком, чешется задней лапой.
        ModelFrame hop = frame;
        hop.origin.y += std::abs(std::sin(gait.phase)) * 0.12f * gait.swing * frame.scale;
        const Emitter e(hop);
        const Joint body{{0.0f, 0.04f, -0.14f}, 0.9f * pose.sitUp};
        const Joint hind{{0.0f, 0.08f, -0.1f}, 0.6f * pose.scratch * (0.5f + 0.5f * scratchWave), 0.0f, &body};
        e.box({0.04f, 0.0f, -0.15f}, {0.08f, 0.1f, -0.04f}, dark, &hind);
        e.box({-0.08f, 0.0f, -0.15f}, {-0.04f, 0.1f, -0.04f}, dark);
        e.pair({0.03f, 0.0f, 0.05f}, {0.06f, 0.08f, 0.1f}, dark, &body, &body);
        e.box({-0.08f, 0.06f, -0.17f}, {0.08f, 0.26f, 0.13f}, color, &body);
        const Joint head{{0.0f, 0.2f, 0.12f}, -0.4f * pose.graze + 0.2f * pose.lookUp, 0.0f, &body, pose.headYaw};
        e.box({-0.06f, 0.18f, 0.1f}, {0.06f, 0.31f, 0.25f}, color, &head);
        e.box({-0.03f, 0.14f, -0.21f}, {0.03f, 0.2f, -0.17f}, {232, 228, 220, 255}, &body);
        const Joint ears{{0.0f, 0.3f, 0.15f}, 0.35f - 0.35f * pose.ears, 0.0f, &head};
        e.pair({0.01f, 0.29f, 0.13f}, {0.035f, 0.46f, 0.17f}, color, &ears, &ears);
        break;
    }
    }
}

void emitBird(const ModelFrame& frame, bool flying, float flap, float peck) {
    constexpr Color kBird = {24, 24, 28, 255};
    const Emitter e(frame);
    e.box({-0.05f, 0.0f, -0.14f}, {0.05f, 0.09f, 0.12f}, kBird);
    const Joint head{{0.0f, 0.08f, 0.1f}, flying ? 0.0f : -0.9f * peck};
    e.box({-0.04f, 0.05f, 0.1f}, {0.04f, 0.13f, 0.18f}, kBird, &head);
    e.box({-0.012f, 0.07f, 0.18f}, {0.012f, 0.1f, 0.22f}, {60, 58, 56, 255}, &head);
    e.box({-0.04f, 0.03f, -0.22f}, {0.04f, 0.06f, -0.14f}, kBird);
    if (flying) {
        const float angle = std::sin(flap) * 0.7f;
        const Joint left{{0.05f, 0.07f, 0.0f}, 0.0f, angle};
        const Joint right{{-0.05f, 0.07f, 0.0f}, 0.0f, -angle};
        e.pair({0.05f, 0.06f, -0.07f}, {0.32f, 0.08f, 0.08f}, kBird, &left, &right);
    }
}

void emitEmote(Vector3 base, float yaw, Emote emote, float size) {
    if (emote == Emote::None || size <= 0.0f) {
        return;
    }
    ModelFrame frame;
    frame.origin = base;
    frame.yaw = yaw;
    frame.scale = size / 0.5f; // значок рисуется высотой 0,5
    const Emitter e(frame);
    Color color = {240, 64, 48, 255};
    switch (emote) {
    case Emote::Discovery:
        color = {250, 210, 70, 255};
        break;
    case Emote::Question:
        color = {120, 190, 250, 255};
        break;
    case Emote::Heart:
        color = {236, 80, 120, 255};
        break;
    case Emote::Anger:
        color = {230, 40, 40, 255};
        break;
    case Emote::Tear:
        color = {90, 160, 240, 255};
        break;
    default:
        break;
    }
    constexpr float d = 0.03f;
    const auto bar = [&](float x0, float y0, float x1, float y1) {
        e.box({x0, y0, -d}, {x1, y1, d}, color);
    };
    gGlow = true; // видно и ночью
    switch (emote) {
    case Emote::Alarm:
    case Emote::Discovery:
        bar(-0.05f, 0.16f, 0.05f, 0.5f);
        bar(-0.05f, 0.0f, 0.05f, 0.1f);
        break;
    case Emote::Question:
        bar(-0.13f, 0.42f, 0.13f, 0.5f);
        bar(-0.15f, 0.36f, -0.07f, 0.46f);
        bar(0.07f, 0.3f, 0.15f, 0.46f);
        bar(-0.04f, 0.24f, 0.11f, 0.31f);
        bar(-0.04f, 0.15f, 0.04f, 0.25f);
        bar(-0.04f, 0.0f, 0.04f, 0.08f);
        break;
    case Emote::Heart:
        bar(-0.17f, 0.28f, -0.01f, 0.42f);
        bar(0.01f, 0.28f, 0.17f, 0.42f);
        bar(-0.19f, 0.2f, 0.19f, 0.32f);
        bar(-0.13f, 0.1f, 0.13f, 0.2f);
        bar(-0.06f, 0.02f, 0.06f, 0.1f);
        break;
    case Emote::Anger:
        // Четыре уголка вокруг середины.
        for (const float sx : {-1.0f, 1.0f}) {
            for (const float sy : {-1.0f, 1.0f}) {
                const float cx = sx * 0.06f;
                const float cy = 0.25f + sy * 0.06f;
                bar(std::min(cx, cx + sx * 0.14f), cy - 0.02f, std::max(cx, cx + sx * 0.14f), cy + 0.02f);
                bar(cx - 0.02f, std::min(cy, cy + sy * 0.14f), cx + 0.02f, std::max(cy, cy + sy * 0.14f));
            }
        }
        break;
    case Emote::Tear:
        bar(-0.025f, 0.36f, 0.025f, 0.44f);
        bar(-0.06f, 0.24f, 0.06f, 0.36f);
        bar(-0.09f, 0.08f, 0.09f, 0.24f);
        bar(-0.06f, 0.02f, 0.06f, 0.08f);
        break;
    default:
        break;
    }
    gGlow = false;
}

void emitCrate(const ModelFrame& frame, Color color) {
    const Emitter e(frame);
    const Color dark = shaded(color, 0.75f);
    e.box({-0.6f, 0.0f, -0.6f}, {0.6f, 0.85f, 0.6f}, color, nullptr, kSides);
    e.box({-0.63f, 0.85f, -0.63f}, {0.63f, 0.96f, 0.63f}, dark, nullptr, kTop | kSides | kBottom);
    // Ремни крест-накрест — чуть шире ящика.
    e.box({-0.61f, 0.0f, -0.07f}, {0.61f, 0.85f, 0.07f}, dark, nullptr, kSides);
    e.box({-0.07f, 0.0f, -0.61f}, {0.07f, 0.85f, 0.61f}, dark, nullptr, kSides);
}

// --- Здания -------------------------------------------------------------------

namespace {

constexpr float kFoundation = 0.5f; // уходит в землю: на склоне здание не висит
constexpr float kPlinth = 0.25f;    // пол над верхним углом земли
constexpr float kEaves = 0.35f;     // свес крыши
constexpr Color kGlassColor = {62, 80, 102, 255};
constexpr Color kWindowLight = {255, 206, 128, 255};
constexpr Color kDoorColor = {86, 58, 40, 255};
constexpr Color kStoneColor = {112, 108, 102, 255};
constexpr Color kFlatRoofColor = {92, 92, 96, 255};
constexpr Color kSnowColor = {236, 240, 246, 255};

enum class Roof : std::uint8_t { Gable, Flat };

// Облик здания по виду и seed.
struct Shape {
    Roof roof = Roof::Flat;
    int storeys = 1;
    float storey = 3.0f;    // высота этажа, м
    float pitch = 0.6f;     // подъём ската на метр полупролёта
    float doorWidth = 1.0f;
    float doorHeight = 2.1f;
    float windowWidth = 1.1f;
    bool windows = true;
    Color roofColor = kFlatRoofColor;
    Color doorColor = kDoorColor;
};

Shape shapeOf(const BuildingModel& model) {
    static constexpr Color kHouseRoofs[] = {
        {128, 52, 42, 255}, {98, 70, 54, 255}, {80, 88, 100, 255}, {74, 94, 72, 255}, {140, 84, 50, 255}};
    Shape shape;
    const std::uint64_t seed = model.seed;
    switch (model.kind) {
    case BuildingKind::House:
        shape.roof = Roof::Gable;
        shape.storeys = seed % 10 < 4 ? 2 : 1;
        shape.roofColor = kHouseRoofs[(seed >> 8) % std::size(kHouseRoofs)];
        break;
    case BuildingKind::Barn:
        shape.roof = Roof::Gable;
        shape.storey = 4.5f;
        shape.pitch = 0.75f;
        shape.doorWidth = 3.0f;
        shape.doorHeight = 3.2f;
        shape.windows = false;
        shape.roofColor = {118, 122, 126, 255};
        shape.doorColor = {96, 66, 46, 255};
        break;
    case BuildingKind::Apartment:
        shape.storeys = 4 + static_cast<int>(seed % 3);
        shape.doorWidth = 1.4f;
        break;
    case BuildingKind::Shop:
        shape.storey = 4.0f;
        shape.doorWidth = 1.6f;
        shape.windowWidth = 2.4f;
        shape.doorColor = kGlassColor;
        break;
    case BuildingKind::Hospital:
        shape.storeys = 4;
        shape.doorWidth = 2.4f;
        shape.doorColor = kGlassColor;
        break;
    case BuildingKind::PoliceStation:
        shape.storeys = 2;
        shape.storey = 3.2f;
        shape.doorWidth = 1.6f;
        break;
    case BuildingKind::School:
        shape.storeys = 3;
        shape.doorWidth = 2.0f;
        shape.windowWidth = 1.6f;
        break;
    }
    return shape;
}

// Здание в своих осях: длинная сторона — вдоль X, конёк крыши — тоже.
struct Layout {
    ModelFrame frame;
    float hx = 0.0f; // полуширина и полуглубина
    float hz = 0.0f;
    float floor = 0.0f;
    float wallTop = 0.0f;
    float ridge = 0.0f; // высота конька (у плоской крыши — верх стен)
    float decalOut = 0.04f; // окна и двери — на столько снаружи стены
    // По сторонам (Side): во сколько раз дальше отходят окна и насколько они
    // видны — вскользь окно мельче пикселя по ширине и спорит со стеной в глубине.
    float sideOut[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float sideDetail[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

Layout layoutOf(const BuildingModel& model, const Shape& shape) {
    Layout layout;
    const float width = model.x1 - model.x0;
    const float depth = model.z1 - model.z0;
    layout.frame.origin = {(model.x0 + model.x1) * 0.5f, 0.0f, (model.z0 + model.z1) * 0.5f};
    if (width >= depth) {
        layout.hx = width * 0.5f;
        layout.hz = depth * 0.5f;
    } else {
        // Повёрнуто на 90°: X модели — вдоль −Z мира, Z модели — вдоль +X.
        layout.frame.yaw = PI * 0.5f;
        layout.hx = depth * 0.5f;
        layout.hz = width * 0.5f;
    }
    layout.floor = model.high + kPlinth;
    layout.wallTop = layout.floor + static_cast<float>(shape.storeys) * shape.storey;
    layout.ridge = shape.roof == Roof::Gable ? layout.wallTop + shape.pitch * layout.hz : layout.wallTop;
    return layout;
}

// Сторона стены в осях модели.
enum class Side : std::uint8_t { PlusX, MinusX, PlusZ, MinusZ };

// Прямоугольник на стене side: вдоль стены [a0, a1], по высоте [y0, y1],
// чуть снаружи (layout.decalOut), чтобы не сливаться со стеной.
void emitOnWall(const Emitter& e, const Layout& layout, Side side, float a0, float a1, float y0, float y1,
                Color color, bool glow = false) {
    const float kOut = layout.decalOut * layout.sideOut[static_cast<int>(side)];
    switch (side) {
    case Side::PlusZ: {
        const float z = layout.hz + kOut;
        e.quad({a0, y0, z}, {a1, y0, z}, {a1, y1, z}, {a0, y1, z}, color, nullptr, glow);
        break;
    }
    case Side::MinusZ: {
        const float z = -layout.hz - kOut;
        e.quad({a1, y0, z}, {a0, y0, z}, {a0, y1, z}, {a1, y1, z}, color, nullptr, glow);
        break;
    }
    case Side::PlusX: {
        const float x = layout.hx + kOut;
        e.quad({x, y0, a1}, {x, y0, a0}, {x, y1, a0}, {x, y1, a1}, color, nullptr, glow);
        break;
    }
    case Side::MinusX: {
        const float x = -layout.hx - kOut;
        e.quad({x, y0, a0}, {x, y0, a1}, {x, y1, a1}, {x, y1, a0}, color, nullptr, glow);
        break;
    }
    }
}

// Коробка у стены side снаружи: вдоль стены [a0, a1], от стены на out, по высоте [y0, y1].
void emitAtWall(const Emitter& e, const Layout& layout, Side side, float a0, float a1, float out, float y0,
                float y1, Color color) {
    switch (side) {
    case Side::PlusZ:
        e.box({a0, y0, layout.hz}, {a1, y1, layout.hz + out}, color, nullptr, kTop | kSides);
        break;
    case Side::MinusZ:
        e.box({a0, y0, -layout.hz - out}, {a1, y1, -layout.hz}, color, nullptr, kTop | kSides);
        break;
    case Side::PlusX:
        e.box({layout.hx, y0, a0}, {layout.hx + out, y1, a1}, color, nullptr, kTop | kSides);
        break;
    case Side::MinusX:
        e.box({-layout.hx - out, y0, a0}, {-layout.hx, y1, a1}, color, nullptr, kTop | kSides);
        break;
    }
}

void emitGableRoof(const Emitter& e, const Layout& layout, Color wall, Color roof) {
    const float slope = layout.ridge - layout.wallTop;
    const float eave = layout.wallTop - kEaves * slope / layout.hz;
    const float xa = -layout.hx - kEaves;
    const float xb = layout.hx + kEaves;
    const float ze = layout.hz + kEaves;
    const float yr = layout.ridge;
    e.quad({xa, yr, 0.0f}, {xa, eave, ze}, {xb, eave, ze}, {xb, yr, 0.0f}, roof);
    e.quad({xa, eave, -ze}, {xa, yr, 0.0f}, {xb, yr, 0.0f}, {xb, eave, -ze}, roof);
    // Изнанка свеса — видна снизу.
    const Color under = shaded(roof, 0.6f);
    e.quad({xb, yr, 0.0f}, {xb, eave, ze}, {xa, eave, ze}, {xa, yr, 0.0f}, under);
    e.quad({xb, eave, -ze}, {xb, yr, 0.0f}, {xa, yr, 0.0f}, {xa, eave, -ze}, under);
    // Фронтоны.
    const float hx = layout.hx;
    const float hz = layout.hz;
    const float top = layout.wallTop;
    e.quad({hx, top, hz}, {hx, top, -hz}, {hx, yr, 0.0f}, {hx, yr, 0.0f}, wall);
    e.quad({-hx, top, -hz}, {-hx, top, hz}, {-hx, yr, 0.0f}, {-hx, yr, 0.0f}, wall);
}

// Проём в осях модели: дверь или окно на стене side.
struct LocalOpening {
    Side side = Side::PlusZ;
    float a0 = 0.0f;
    float a1 = 0.0f;
    float y0 = 0.0f;
    float y1 = 0.0f;
    int storey = 0;
    bool door = false;
    std::uint64_t bits = 0; // хеш окна: светится ли ночью
};

// Дверь и окна по этажам: равными промежутками, на первом этаже — не над дверью.
std::vector<LocalOpening> localOpenings(const BuildingModel& model, const Shape& shape, const Layout& layout) {
    std::vector<LocalOpening> result;
    const float hx = layout.hx;
    const float hz = layout.hz;
    // Дверь: в какой стене и где вдоль неё (в осях модели).
    const float dx = model.door.x - layout.frame.origin.x;
    const float dz = model.door.y - layout.frame.origin.z;
    const float lx = layout.frame.yaw == 0.0f ? dx : -dz;
    const float lz = layout.frame.yaw == 0.0f ? dz : dx;
    Side doorSide = Side::PlusZ;
    float doorAlong = lx;
    if (std::abs(lz) >= hz - 0.05f) {
        doorSide = lz > 0.0f ? Side::PlusZ : Side::MinusZ;
    } else {
        doorSide = lx > 0.0f ? Side::PlusX : Side::MinusX;
        doorAlong = lz;
    }
    const float doorHalf = shape.doorWidth * 0.5f;
    result.push_back({doorSide, doorAlong - doorHalf, doorAlong + doorHalf, layout.floor,
                      layout.floor + shape.doorHeight, 0, true, 0});
    if (!shape.windows) {
        return result;
    }
    const float windowHalf = shape.windowWidth * 0.5f;
    const bool shop = model.kind == BuildingKind::Shop;
    for (const Side side : {Side::PlusZ, Side::MinusZ, Side::PlusX, Side::MinusX}) {
        const float length = side == Side::PlusZ || side == Side::MinusZ ? 2.0f * hx : 2.0f * hz;
        const float spacing = shape.windowWidth + (shop ? 1.2f : 2.0f);
        const int count = static_cast<int>((length - 1.0f) / spacing);
        if (count < 1) {
            continue;
        }
        const float gap = length / static_cast<float>(count);
        for (int storey = 0; storey < shape.storeys; ++storey) {
            const float base = layout.floor + static_cast<float>(storey) * shape.storey;
            const float y0 = base + (shop ? 0.6f : 0.9f);
            const float y1 = shop ? base + 2.8f : y0 + 1.3f;
            for (int i = 0; i < count; ++i) {
                const float along = -length * 0.5f + gap * (static_cast<float>(i) + 0.5f);
                if (storey == 0 && side == doorSide && std::abs(along - doorAlong) < doorHalf + windowHalf + 0.2f) {
                    continue;
                }
                const std::uint64_t bits = (model.seed >> 8) ^ (static_cast<std::uint64_t>(side) * 0x9E3779B1u) ^
                                           (static_cast<std::uint64_t>(storey) * 0x85EBCA77u) ^
                                           (static_cast<std::uint64_t>(i) * 0xC2B2AE3Du);
                result.push_back({side, along - windowHalf, along + windowHalf, y0, y1, storey, false, bits});
            }
        }
    }
    return result;
}

} // namespace

BuildingShell buildingShell(const BuildingModel& model) {
    const Shape shape = shapeOf(model);
    const Layout layout = layoutOf(model, shape);
    BuildingShell shell;
    shell.floor = layout.floor;
    shell.storey = shape.storey;
    shell.storeys = shape.storeys;
    shell.wallTop = layout.wallTop;
    return shell;
}

std::vector<WallOpening> buildingOpenings(const BuildingModel& model) {
    const Shape shape = shapeOf(model);
    const Layout layout = layoutOf(model, shape);
    const float ox = layout.frame.origin.x;
    const float oz = layout.frame.origin.z;
    const bool turned = layout.frame.yaw != 0.0f;
    std::vector<WallOpening> result;
    for (const LocalOpening& local : localOpenings(model, shape, layout)) {
        WallOpening o;
        o.door = local.door;
        o.y0 = local.y0;
        o.y1 = local.y1;
        o.storey = local.storey;
        // Оси модели → мира: без поворота — те же; повёрнутое на 90° — X модели
        // идёт по −Z мира, Z модели — по +X мира.
        switch (local.side) {
        case Side::PlusZ:
        case Side::MinusZ: {
            const float sign = local.side == Side::PlusZ ? 1.0f : -1.0f;
            if (!turned) {
                o.alongX = true;
                o.plane = oz + sign * layout.hz;
                o.outward = sign;
                o.a0 = ox + local.a0;
                o.a1 = ox + local.a1;
            } else {
                o.alongX = false;
                o.plane = ox + sign * layout.hz;
                o.outward = sign;
                o.a0 = oz - local.a1;
                o.a1 = oz - local.a0;
            }
            break;
        }
        case Side::PlusX:
        case Side::MinusX: {
            const float sign = local.side == Side::PlusX ? 1.0f : -1.0f;
            if (!turned) {
                o.alongX = false;
                o.plane = ox + sign * layout.hx;
                o.outward = sign;
                o.a0 = oz + local.a0;
                o.a1 = oz + local.a1;
            } else {
                o.alongX = true;
                o.plane = oz - sign * layout.hx;
                o.outward = -sign;
                o.a0 = ox + local.a0;
                o.a1 = ox + local.a1;
            }
            break;
        }
        }
        result.push_back(o);
    }
    return result;
}

float buildingTop(const BuildingModel& model) {
    const Shape shape = shapeOf(model);
    return layoutOf(model, shape).ridge + 1.0f;
}

void emitBuilding(const BuildingModel& model) {
    const Shape shape = shapeOf(model);
    Layout layout = layoutOf(model, shape);
    layout.decalOut = model.decalOut;
    if (model.eye) {
        // Глаз — в осях модели (обратный поворот ModelFrame).
        const float c = std::cos(layout.frame.yaw);
        const float s = std::sin(layout.frame.yaw);
        const float dx = model.eye->x - layout.frame.origin.x;
        const float dz = model.eye->z - layout.frame.origin.z;
        const Vector3 eye = {dx * c - dz * s, model.eye->y, dx * s + dz * c};
        const float midY = 0.5f * (layout.floor + layout.wallTop);
        const Vector3 centers[4] = {{layout.hx, midY, 0.0f}, {-layout.hx, midY, 0.0f}, {0.0f, midY, layout.hz},
                                    {0.0f, midY, -layout.hz}};
        const Vector3 normals[4] = {{1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
        for (int side = 0; side < 4; ++side) {
            const Vector3 toEye = Vector3Subtract(eye, centers[side]);
            const float facing = Vector3DotProduct(normals[side], toEye) / std::max(Vector3Length(toEye), 1e-3f);
            layout.sideOut[side] = 1.0f / std::max(facing, 0.2f);
            const float k = std::clamp((facing - 0.08f) / 0.2f, 0.0f, 1.0f);
            layout.sideDetail[side] = k * k * (3.0f - 2.0f * k);
        }
    }
    const Emitter e(layout.frame);
    const Color wall = buildingColor(model.kind);
    const float hx = layout.hx;
    const float hz = layout.hz;

    // Цоколь от фундамента до пола, над ним стены.
    e.box({-hx - 0.06f, model.low - kFoundation, -hz - 0.06f}, {hx + 0.06f, layout.floor, hz + 0.06f},
          mix(kStoneColor, wall, 0.25f), nullptr, kTop | kSides);
    Color roof = shape.roofColor;
    if (model.snow > 0.0f) {
        roof = mix(roof, kSnowColor, std::min(model.snow, 1.0f) * 0.85f);
    }
    if (shape.roof == Roof::Gable) {
        e.box({-hx, layout.floor, -hz}, {hx, layout.wallTop, hz}, wall, nullptr, kSides);
        emitGableRoof(e, layout, wall, roof);
        if (model.kind == BuildingKind::House) {
            const float x = hx * 0.45f;
            const float z = hz * 0.35f;
            e.box({x - 0.3f, layout.wallTop, z - 0.3f}, {x + 0.3f, layout.ridge + 0.6f, z + 0.3f},
                  {124, 64, 50, 255}, nullptr, kTop | kSides);
        }
    } else {
        e.box({-hx, layout.floor, -hz}, {hx, layout.wallTop, hz}, wall, nullptr, kTop | kSides, roof);
        // Парапет по краю крыши.
        const float y0 = layout.wallTop;
        const float y1 = y0 + 0.35f;
        constexpr float t = 0.25f;
        e.box({-hx, y0, hz - t}, {hx, y1, hz}, wall, nullptr, kTop | kSides);
        e.box({-hx, y0, -hz}, {hx, y1, -hz + t}, wall, nullptr, kTop | kSides);
        e.box({hx - t, y0, -hz + t}, {hx, y1, hz - t}, wall, nullptr, kTop | kSides);
        e.box({-hx, y0, -hz + t}, {-hx + t, y1, hz - t}, wall, nullptr, kTop | kSides);
        if (model.kind == BuildingKind::Apartment || model.kind == BuildingKind::School) {
            // Выход на крышу.
            const float x = hx * 0.4f;
            e.box({x - 1.5f, y0, -1.5f}, {x + 1.5f, y0 + 2.4f, 1.5f}, shaded(wall, 0.9f), nullptr,
                  kTop | kSides, roof);
        }
        if (model.kind == BuildingKind::Hospital) {
            constexpr Color kCross = {200, 40, 40, 255};
            e.box({-3.0f, y0, -0.8f}, {3.0f, y0 + 0.12f, 0.8f}, kCross, nullptr, kTop | kSides);
            e.box({-0.8f, y0, -3.0f}, {0.8f, y0 + 0.12f, 3.0f}, kCross, nullptr, kTop | kSides);
        }
    }
    if (!model.detailed) {
        return;
    }

    // Дверь и окна — тем же списком, что и комнаты внутри (buildingOpenings).
    const std::vector<LocalOpening> openings = localOpenings(model, shape, layout);
    const LocalOpening& door = openings.front();
    const Side doorSide = door.side;
    const float doorAlong = (door.a0 + door.a1) * 0.5f;
    const float doorHalf = shape.doorWidth * 0.5f;
    const auto detailOn = [&](Side side) { return model.detail * layout.sideDetail[static_cast<int>(side)]; };
    emitOnWall(e, layout, doorSide, door.a0, door.a1, door.y0, door.y1,
               mix(wall, shape.doorColor, detailOn(doorSide)));
    if (model.doorGround < layout.floor - 0.15f) {
        // Крыльцо до земли у двери.
        emitAtWall(e, layout, doorSide, doorAlong - doorHalf - 0.3f, doorAlong + doorHalf + 0.3f, 1.0f,
                   model.doorGround - 0.3f, layout.floor, kStoneColor);
    }
    if (model.kind == BuildingKind::Shop) {
        static constexpr Color kSigns[] = {{196, 58, 48, 255}, {52, 110, 170, 255}, {60, 140, 80, 255}};
        const Color sign = kSigns[(model.seed >> 16) % std::size(kSigns)];
        const float y = layout.floor + shape.doorHeight + 0.3f;
        emitAtWall(e, layout, doorSide, doorAlong - 2.0f, doorAlong + 2.0f, 0.15f, y, y + 0.6f, sign);
    }

    // Окна по этажам. Ночью светится часть окон — по хешу окна, одни и те же.
    for (std::size_t i = 1; i < openings.size(); ++i) {
        const LocalOpening& window = openings[i];
        const bool glow = model.lights > 0.0f &&
                          static_cast<float>((window.bits * 0x2545F4914F6CDD1DULL) >> 40 & 0xFFFFu) / 65535.0f <
                              model.lights;
        const float detail = detailOn(window.side);
        if (detail <= 0.0f) {
            continue; // окно цвета стены (ночью — тёмное, как стена) не нужно
        }
        emitOnWall(e, layout, window.side, window.a0, window.a1, window.y0, window.y1,
                   glow ? shaded(kWindowLight, detail) : mix(wall, kGlassColor, detail), glow);
    }
}

void emitWorldBox(Vector3 min, Vector3 max, Color color, bool bottom) {
    const ModelFrame frame;
    const Emitter e(frame);
    e.box(min, max, color, nullptr, bottom ? kAll : (kTop | kSides));
}

void emitTurnedBox(Vector3 center, Vector3 half, float yaw, Color color, bool bottom) {
    ModelFrame frame;
    frame.origin = center;
    frame.yaw = yaw;
    const Emitter e(frame);
    e.box(Vector3Negate(half), half, color, nullptr, bottom ? kAll : (kTop | kSides));
}

void emitBeacon(Vector3 tip, float size, Color color) {
    ModelFrame frame;
    frame.origin = tip;
    frame.scale = size;
    const Emitter e(frame);
    constexpr float h = 0.4f;
    const Vector3 t = {0.0f, 0.0f, 0.0f};
    const Vector3 p0 = {-h, 1.0f, -h};
    const Vector3 p1 = {h, 1.0f, -h};
    const Vector3 p2 = {h, 1.0f, h};
    const Vector3 p3 = {-h, 1.0f, h};
    // Грани светятся сами: метку видно и ночью; боковые чуть темнее верха.
    const Color side = shaded(color, 0.8f);
    e.quad(t, p2, p3, p3, side, nullptr, true);
    e.quad(t, p1, p2, p2, color, nullptr, true);
    e.quad(t, p0, p1, p1, side, nullptr, true);
    e.quad(t, p3, p0, p0, color, nullptr, true);
    e.quad(p0, p3, p2, p1, color, nullptr, true);
}

void setModelFade(float visible) {
    gDissolve = 1.0f - std::clamp(visible, 0.0f, 1.0f);
}

void setModelIndoor(bool indoor) {
    gIndoor = indoor;
}

// --- Комнаты внутри (этап 12а.14) ------------------------------------------------

namespace {

constexpr Color kCeilingColor = {226, 224, 216, 255};
constexpr Color kFrameColor = {228, 228, 220, 255};
constexpr Color kThresholdColor = {118, 98, 76, 255};
constexpr Color kLampOn = {255, 226, 160, 255};
constexpr Color kLampOff = {214, 204, 180, 255};
constexpr Color kDarkColor = {40, 38, 36, 255};
constexpr Color kBrassColor = {186, 156, 90, 255};
constexpr Color kLinenColor = {232, 230, 220, 255};

Color colorOf(Rgb c) {
    return {c.r, c.g, c.b, 255};
}

// Брус стены по осям мира: грани со стороны lo и hi — своими обоями.
void emitWallBox(const Emitter& e, bool alongX, Vector3 min, Vector3 max, Color lo, Color hi) {
    const float x0 = min.x;
    const float y0 = min.y;
    const float z0 = min.z;
    const float x1 = max.x;
    const float y1 = max.y;
    const float z1 = max.z;
    const Color ends = mix(lo, hi, 0.5f);
    const Color south = alongX ? hi : ends; // грань +Z
    const Color north = alongX ? lo : ends; // грань −Z
    const Color east = alongX ? ends : hi;  // грань +X
    const Color west = alongX ? ends : lo;  // грань −X
    e.quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, ends);
    e.quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, ends);
    e.quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, south);
    e.quad({x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, north);
    e.quad({x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, east);
    e.quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, west);
}

// Коробка стены от a0 до a1 вдоль, по высоте от y0 до y1 (мир), поперёк — lo..hi.
void emitWallPiece(const Emitter& e, const InteriorWall& wall, float a0, float a1, float y0, float y1) {
    if (a1 - a0 < 1e-3f || y1 - y0 < 1e-3f) {
        return;
    }
    const Vector3 min = wall.alongX ? Vector3{a0, y0, wall.lo} : Vector3{wall.lo, y0, a0};
    const Vector3 max = wall.alongX ? Vector3{a1, y1, wall.hi} : Vector3{wall.hi, y1, a1};
    emitWallBox(e, wall.alongX, min, max, colorOf(wall.colorLo), colorOf(wall.colorHi));
}

// Рама окна посреди толщины стены: подоконник, перекладины, средник.
void emitWindowFrame(const Emitter& e, const InteriorWall& wall, const WallGap& gap, float floor) {
    const float mid = (wall.lo + wall.hi) * 0.5f;
    constexpr float t = 0.05f;
    const auto bar = [&](float a0, float a1, float y0, float y1, float depth, Color color) {
        const Vector3 min = wall.alongX ? Vector3{a0, floor + y0, mid - depth} : Vector3{mid - depth, floor + y0, a0};
        const Vector3 max = wall.alongX ? Vector3{a1, floor + y1, mid + depth} : Vector3{mid + depth, floor + y1, a1};
        e.box(min, max, color);
    };
    bar(gap.a0, gap.a1, gap.y0, gap.y0 + t, 0.16f, kFrameColor); // подоконник
    bar(gap.a0, gap.a1, gap.y1 - t, gap.y1, 0.04f, kFrameColor);
    bar(gap.a0, gap.a0 + t, gap.y0, gap.y1, 0.04f, kFrameColor);
    bar(gap.a1 - t, gap.a1, gap.y0, gap.y1, 0.04f, kFrameColor);
    const float c = (gap.a0 + gap.a1) * 0.5f;
    bar(c - t * 0.5f, c + t * 0.5f, gap.y0, gap.y1, 0.04f, kFrameColor);
    bar(gap.a0, gap.a1, gap.y0 + (gap.y1 - gap.y0) * 0.62f, gap.y0 + (gap.y1 - gap.y0) * 0.62f + t * 0.8f, 0.04f,
        kFrameColor);
}

void emitWall(const Emitter& e, const InteriorWall& wall, float floor, float height, bool shadowPass) {
    std::vector<WallGap> gaps = wall.gaps;
    std::sort(gaps.begin(), gaps.end(), [](const WallGap& a, const WallGap& b) { return a.a0 < b.a0; });
    const float y0 = floor - 0.02f;
    const float y1 = floor + height + 0.02f;
    float a = wall.a0;
    for (const WallGap& gap : gaps) {
        const float g0 = std::clamp(gap.a0, wall.a0, wall.a1);
        const float g1 = std::clamp(gap.a1, wall.a0, wall.a1);
        if (g1 <= a) {
            continue;
        }
        emitWallPiece(e, wall, a, g0, y0, y1);
        emitWallPiece(e, wall, g0, g1, y0, floor + gap.y0);
        emitWallPiece(e, wall, g0, g1, floor + gap.y1, y1);
        if (gap.window && !shadowPass) {
            emitWindowFrame(e, wall, gap, floor);
        }
        a = g1;
    }
    emitWallPiece(e, wall, a, wall.a1, y0, y1);
}

// Книги на полке: корешки разной высоты и цвета.
void emitBooks(const Emitter& e, float x0, float x1, float y, float depth, std::uint32_t seed) {
    static constexpr Color kSpines[] = {{150, 48, 40, 255}, {50, 84, 130, 255}, {70, 110, 64, 255},
                                        {180, 150, 80, 255}, {96, 60, 90, 255}, {200, 196, 180, 255}};
    float x = x0 + 0.02f;
    std::uint32_t s = seed | 1u;
    while (x < x1 - 0.05f) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const float w = 0.025f + static_cast<float>(s % 40u) / 1000.0f;
        const float h = 0.16f + static_cast<float>((s >> 8) % 90u) / 1000.0f;
        if ((s >> 20) % 7u == 0u) {
            x += 0.1f; // пустое место
            continue;
        }
        e.box({x, y, -depth * 0.45f}, {std::min(x + w, x1 - 0.02f), y + h, depth * 0.35f},
              kSpines[(s >> 16) % std::size(kSpines)]);
        x += w + 0.004f;
    }
}

void emitFurniture(const Furniture& f, float floor, float ceiling, bool lampsOn, bool shadowPass) {
    ModelFrame frame;
    frame.origin = {f.x, floor + f.base, f.z};
    frame.yaw = f.facing;
    const Emitter e(frame);
    const float w = f.width * 0.5f;
    const float d = f.depth * 0.5f;
    const float h = f.height;
    const Color c = colorOf(f.color);
    const Color dark = shaded(c, 0.7f);
    const Color light = shaded(c, 1.15f);
    const auto legs = [&](float x, float z, float top, float size, Color color) {
        for (const float sx : {-1.0f, 1.0f}) {
            for (const float sz : {-1.0f, 1.0f}) {
                const float lx = sx * (x - size);
                const float lz = sz * (z - size);
                e.box({lx - size, 0.0f, lz - size}, {lx + size, top, lz + size}, color);
            }
        }
    };
    switch (f.kind) {
    case FurnitureKind::Bed:
        if (h < 0.6f) { // нары в камере
            e.box({-w, 0.0f, -d}, {w, h, d}, c);
            break;
        }
        e.box({-w, 0.0f, -d}, {w, 0.3f, d}, kWoodColor);
        e.box({-w + 0.04f, 0.3f, -d + 0.06f}, {w - 0.04f, 0.5f, d - 0.04f}, kLinenColor);
        e.box({-w + 0.02f, 0.42f, -d + 0.6f}, {w - 0.02f, 0.55f, d - 0.02f}, c);
        if (!shadowPass) {
            if (f.width > 1.2f) {
                e.box({-w + 0.1f, 0.5f, -d + 0.12f}, {-0.05f, 0.6f, -d + 0.48f}, kLinenColor);
                e.box({0.05f, 0.5f, -d + 0.12f}, {w - 0.1f, 0.6f, -d + 0.48f}, kLinenColor);
            } else {
                e.box({-w + 0.1f, 0.5f, -d + 0.12f}, {w - 0.1f, 0.6f, -d + 0.48f}, kLinenColor);
            }
        }
        e.box({-w, 0.0f, -d}, {w, h, -d + 0.07f}, shaded(kWoodColor, 0.85f));
        break;
    case FurnitureKind::Table:
        e.box({-w, h - 0.04f, -d}, {w, h, d}, c);
        legs(w - 0.04f, d - 0.04f, h - 0.04f, 0.025f, dark);
        break;
    case FurnitureKind::Desk:
        e.box({-w, h - 0.04f, -d}, {w, h, d}, c);
        legs(w - 0.03f, d - 0.03f, h - 0.04f, 0.02f, {80, 80, 84, 255});
        e.box({-w + 0.03f, h - 0.3f, d - 0.04f}, {w - 0.03f, h - 0.04f, d - 0.02f}, dark);
        break;
    case FurnitureKind::Chair:
        e.box({-0.21f, 0.42f, -0.21f}, {0.21f, 0.46f, 0.21f}, c);
        legs(0.2f, 0.2f, 0.42f, 0.02f, dark);
        e.box({-0.21f, 0.46f, -0.22f}, {0.21f, h, -0.18f}, c);
        break;
    case FurnitureKind::Sofa:
    case FurnitureKind::Armchair:
        e.box({-w, 0.08f, -d}, {w, 0.42f, d}, c);
        e.box({-w, 0.08f, -d}, {w, h, -d + 0.22f}, dark);
        e.box({-w, 0.08f, -d}, {-w + 0.18f, 0.62f, d}, dark);
        e.box({w - 0.18f, 0.08f, -d}, {w, 0.62f, d}, dark);
        if (!shadowPass) {
            e.box({-w + 0.18f, 0.42f, -d + 0.22f}, {w - 0.18f, 0.47f, d - 0.03f}, light);
            legs(w - 0.04f, d - 0.04f, 0.08f, 0.03f, kDarkColor);
        }
        break;
    case FurnitureKind::Wardrobe:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        if (!shadowPass) {
            e.box({-0.006f, 0.05f, d}, {0.006f, h - 0.05f, d + 0.01f}, dark);
            e.box({-0.08f, 1.0f, d}, {-0.05f, 1.15f, d + 0.03f}, kBrassColor);
            e.box({0.05f, 1.0f, d}, {0.08f, 1.15f, d + 0.03f}, kBrassColor);
        }
        break;
    case FurnitureKind::Counter:
    case FurnitureKind::Stove:
    case FurnitureKind::Sink: {
        const bool tall = h > 0.95f; // прилавок, стойка
        e.box({-w, 0.08f, -d}, {w, h - 0.04f, d}, c);
        e.box({-w + 0.02f, 0.0f, -d}, {w - 0.02f, 0.08f, d - 0.06f}, kDarkColor);
        const Color top = f.kind == FurnitureKind::Stove ? Color{56, 56, 58, 255}
                          : tall                         ? shaded(c, 0.85f)
                                                         : Color{166, 156, 140, 255};
        e.box({-w, h - 0.04f, -d}, {w, h, d + 0.02f}, top);
        if (shadowPass) {
            break;
        }
        if (f.kind == FurnitureKind::Stove) {
            for (const float bx : {-0.14f, 0.14f}) {
                for (const float bz : {-0.12f, 0.12f}) {
                    e.box({bx - 0.08f, h, bz - 0.08f}, {bx + 0.08f, h + 0.012f, bz + 0.08f}, kDarkColor);
                }
            }
            e.box({-w + 0.06f, 0.2f, d}, {w - 0.06f, 0.65f, d + 0.01f}, {30, 30, 34, 255});
        } else if (f.kind == FurnitureKind::Sink) {
            e.box({-w + 0.08f, h - 0.035f, -d + 0.1f}, {w - 0.08f, h + 0.004f, d - 0.08f}, kSteelColor);
            e.box({-0.02f, h, -d + 0.04f}, {0.02f, h + 0.25f, -d + 0.08f}, kSteelColor);
        } else if (!tall) {
            e.box({-0.004f, 0.12f, d}, {0.004f, h - 0.08f, d + 0.008f}, dark);
        }
        break;
    }
    case FurnitureKind::Fridge:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        if (!shadowPass) {
            e.box({-w + 0.02f, h * 0.62f, d}, {w - 0.02f, h * 0.62f + 0.01f, d + 0.006f}, shaded(c, 0.75f));
            e.box({w - 0.1f, h * 0.68f, d}, {w - 0.07f, h * 0.9f, d + 0.04f}, kSteelColor);
        }
        break;
    case FurnitureKind::Shelf: {
        e.box({-w, 0.0f, -d}, {-w + 0.03f, h, d}, c);
        e.box({w - 0.03f, 0.0f, -d}, {w, h, d}, c);
        e.box({-w, 0.0f, -d}, {w, h, -d + 0.02f}, dark);
        const int boards = std::max(3, static_cast<int>(h / 0.4f));
        for (int i = 0; i <= boards; ++i) {
            const float y = std::min(h - 0.03f, static_cast<float>(i) * h / static_cast<float>(boards));
            e.box({-w, y, -d}, {w, y + 0.03f, d}, c);
            if (!shadowPass && i < boards) {
                Emitter inner(frame);
                emitBooks(inner, -w + 0.03f, w - 0.03f, y + 0.03f, f.depth, f.seed + static_cast<std::uint32_t>(i) * 977u);
            }
        }
        break;
    }
    case FurnitureKind::Nightstand:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        if (!shadowPass) {
            e.box({-w + 0.04f, h - 0.16f, d}, {w - 0.04f, h - 0.15f, d + 0.006f}, dark);
            e.box({-0.06f, h, -0.06f}, {0.06f, h + 0.25f, 0.06f}, kLampOff);
        }
        break;
    case FurnitureKind::TvStand:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        e.box({-0.55f, h, -0.06f}, {0.55f, h + 0.62f, 0.02f}, {22, 22, 24, 255});
        break;
    case FurnitureKind::Rug:
        if (!shadowPass) {
            e.box({-w, 0.0f, -d}, {w, h, d}, c, nullptr, kTop);
            e.box({-w + 0.12f, h, -d + 0.12f}, {w - 0.12f, h + 0.002f, d - 0.12f}, shaded(c, 1.2f), nullptr, kTop);
        }
        break;
    case FurnitureKind::Tub:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        if (!shadowPass) {
            e.box({-w + 0.08f, h - 0.005f, -d + 0.08f}, {w - 0.08f, h + 0.002f, d - 0.08f}, {168, 186, 196, 255},
                  nullptr, kTop);
        }
        break;
    case FurnitureKind::Lamp: {
        if (shadowPass) {
            break;
        }
        // Шнур до потолка и абажур; горит — светится сам.
        const float top = ceiling - frame.origin.y;
        e.box({-0.01f, h, -0.01f}, {0.01f, top, 0.01f}, kDarkColor);
        e.quad({-w, 0.0f, -d}, {w, 0.0f, -d}, {w, 0.0f, d}, {-w, 0.0f, d}, lampsOn ? kLampOn : kLampOff, nullptr,
               lampsOn);
        e.box({-w, 0.0f, -d}, {w, h, d}, lampsOn ? shaded(kLampOn, 0.9f) : kLampOff, nullptr, kSides | kTop);
        break;
    }
    case FurnitureKind::Board:
        e.box({-w - 0.05f, -0.05f, -d}, {w + 0.05f, h + 0.05f, d}, kWoodColor);
        e.box({-w, 0.0f, -d}, {w, h, d + 0.01f}, c);
        break;
    case FurnitureKind::Bars: {
        e.box({-w, 0.0f, -d}, {w, 0.05f, d}, c);
        e.box({-w, h - 0.05f, -d}, {w, h, d}, c);
        for (float x = -w; x <= w + 1e-3f; x += 0.14f) {
            e.box({x - 0.015f, 0.0f, -0.015f}, {x + 0.015f, h, 0.015f}, c);
        }
        break;
    }
    case FurnitureKind::Bales:
        for (float y = 0.0f; y < h - 1e-3f; y += 0.6f) {
            e.box({-w, y, -d}, {w, y + 0.58f, d}, c);
            if (!shadowPass) {
                for (const float x : {-w * 0.5f, w * 0.5f}) {
                    e.box({x - 0.02f, y, -d - 0.005f}, {x + 0.02f, y + 0.585f, d + 0.005f}, shaded(c, 0.7f));
                }
            }
        }
        break;
    case FurnitureKind::Crate:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        if (!shadowPass) {
            e.box({-w - 0.005f, h * 0.45f, -d - 0.005f}, {w + 0.005f, h * 0.55f, d + 0.005f}, dark);
        }
        break;
    case FurnitureKind::Stall:
        e.box({-w, 0.0f, -d}, {w, h, d}, c);
        break;
    case FurnitureKind::Door:
        e.box({-w - 0.06f, 0.0f, -d}, {w + 0.06f, h + 0.06f, d}, shaded(c, 1.25f));
        e.box({-w, 0.0f, -d}, {w, h, d + 0.02f}, c);
        if (!shadowPass) {
            e.box({w - 0.16f, 1.0f, d + 0.02f}, {w - 0.06f, 1.04f, d + 0.07f}, kBrassColor);
        }
        break;
    case FurnitureKind::Steps: {
        constexpr int kSteps = 14;
        for (int i = 0; i < kSteps; ++i) {
            const float z0 = -d + f.depth * static_cast<float>(i) / kSteps;
            e.box({-w, 0.0f, z0}, {w, h * static_cast<float>(i + 1) / kSteps, d}, c);
        }
        e.box({w - 0.04f, 0.0f, -d}, {w, h + 0.9f, -d + 0.04f}, kSteelColor);
        break;
    }
    }
}

} // namespace

void emitInterior(const InteriorLayout& layout, const InteriorLook& look) {
    const bool savedIndoor = gIndoor;
    gIndoor = true;
    const ModelFrame world;
    const Emitter e(world);
    const float floor = layout.floor;
    const float height = layout.ceiling - layout.floor;
    // Пол: подложка под всем этажом (видна в проёмах дверей) и пол каждой комнаты.
    if (!look.shadowPass) {
        e.box({layout.x0, floor - 0.32f, layout.z0}, {layout.x1, floor - 0.005f, layout.z1}, kThresholdColor,
              nullptr, kTop);
        for (const InteriorRoom& room : layout.rooms) {
            e.box({room.x0, floor - 0.3f, room.z0}, {room.x1, floor, room.z1}, colorOf(room.floor), nullptr, kTop);
        }
        // Потолок изнутри — нижняя грань плиты.
        e.box({layout.outerX0, layout.ceiling, layout.outerZ0}, {layout.outerX1, layout.ceiling + 0.25f, layout.outerZ1},
              kCeilingColor, nullptr, kBottom);
    } else {
        // В карту теней — плита потолка целиком: свет заходит только в окна.
        e.box({layout.outerX0, layout.ceiling, layout.outerZ0}, {layout.outerX1, layout.ceiling + 0.25f, layout.outerZ1},
              kCeilingColor);
        e.box({layout.outerX0, floor - 0.3f, layout.outerZ0}, {layout.outerX1, floor - 0.01f, layout.outerZ1},
              kCeilingColor, nullptr, kBottom | kSides);
    }
    for (std::size_t i = 0; i < layout.walls.size(); ++i) {
        if (!look.shadowPass && look.hiddenWalls != nullptr && i < look.hiddenWalls->size() && (*look.hiddenWalls)[i]) {
            continue;
        }
        emitWall(e, layout.walls[i], floor, height, look.shadowPass);
    }
    for (std::size_t i = 0; i < layout.furniture.size(); ++i) {
        if (!look.shadowPass && look.hiddenFurniture != nullptr && i < look.hiddenFurniture->size() &&
            (*look.hiddenFurniture)[i]) {
            continue;
        }
        emitFurniture(layout.furniture[i], floor, layout.ceiling, look.lampsOn, look.shadowPass);
    }
    gIndoor = savedIndoor;
}

} // namespace lw
