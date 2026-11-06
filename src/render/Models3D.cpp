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
        rlTexCoord2f(glow || gGlow ? 1.0f : 0.0f, gDissolve);
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
    const float kOut = layout.decalOut;
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

} // namespace

float buildingTop(const BuildingModel& model) {
    const Shape shape = shapeOf(model);
    return layoutOf(model, shape).ridge + 1.0f;
}

void emitBuilding(const BuildingModel& model) {
    const Shape shape = shapeOf(model);
    Layout layout = layoutOf(model, shape);
    layout.decalOut = model.decalOut;
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
    emitOnWall(e, layout, doorSide, doorAlong - doorHalf, doorAlong + doorHalf, layout.floor,
               layout.floor + shape.doorHeight, mix(wall, shape.doorColor, model.detail));
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

    // Окна по этажам: равными промежутками, на первом этаже — не над дверью.
    if (!shape.windows) {
        return;
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
                // Ночью светится часть окон — по хешу окна, одни и те же.
                const std::uint64_t bits = (model.seed >> 8) ^ (static_cast<std::uint64_t>(side) * 0x9E3779B1u) ^
                                           (static_cast<std::uint64_t>(storey) * 0x85EBCA77u) ^
                                           (static_cast<std::uint64_t>(i) * 0xC2B2AE3Du);
                const bool glow = model.lights > 0.0f &&
                                  static_cast<float>((bits * 0x2545F4914F6CDD1DULL) >> 40 & 0xFFFFu) / 65535.0f <
                                      model.lights;
                emitOnWall(e, layout, side, along - windowHalf, along + windowHalf, y0, y1,
                           glow ? shaded(kWindowLight, model.detail) : mix(wall, kGlassColor, model.detail), glow);
            }
        }
    }
}

void emitWorldBox(Vector3 min, Vector3 max, Color color, bool bottom) {
    const ModelFrame frame;
    const Emitter e(frame);
    e.box(min, max, color, nullptr, bottom ? kAll : (kTop | kSides));
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

} // namespace lw
