#include "render/Poses.h"

#include <algorithm>
#include <cmath>

namespace lw {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTau = 6.28318531f;
constexpr float kHalfPi = 1.57079633f;

float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

LegPose lerpLeg(const LegPose& a, const LegPose& b, float t) {
    return {lerpf(a.hip, b.hip, t), lerpf(a.spread, b.spread, t), lerpf(a.knee, b.knee, t)};
}

ArmPose lerpArm(const ArmPose& a, const ArmPose& b, float t) {
    return {lerpf(a.raise, b.raise, t), lerpf(a.out, b.out, t), lerpf(a.elbow, b.elbow, t)};
}

// Доля 0..1 из seed: у каждого существа своя манера.
float unit(std::uint32_t seed, std::uint32_t salt) {
    std::uint32_t x = seed ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFu) / 65535.0f;
}

// Внезапные рывки 0..1: редкие короткие всплески (дрожь тела, подёргивания).
float spike(float t, float rate, float offset) {
    const float a = std::sin(t * rate + offset);
    const float b = std::sin(t * rate * 0.37f + offset * 2.1f);
    return std::pow(std::max(0.0f, a * b), 6.0f);
}

ArmPose arm(float raise, float out, float elbow) {
    return {raise, out, elbow};
}

LegPose leg(float hip, float spread, float knee) {
    return {hip, spread, knee};
}

// Расслабленная стойка: руки чуть в стороны и согнуты.
HumanPose relaxed() {
    HumanPose p;
    p.leftArm = arm(0.0f, 0.06f, 0.15f);
    p.rightArm = arm(0.0f, 0.06f, 0.15f);
    p.leftLeg = leg(0.0f, 0.04f, 0.0f);
    p.rightLeg = leg(0.0f, 0.04f, 0.0f);
    return p;
}

// Опорные позы по времени: между соседними — плавный переход.
HumanPose keyed(const HumanPose* keys, const float* times, int count, float t) {
    if (t <= times[0]) {
        return keys[0];
    }
    for (int i = 1; i < count; ++i) {
        if (t < times[i]) {
            const float u = (t - times[i - 1]) / (times[i] - times[i - 1]);
            return blend(keys[i - 1], keys[i], pose::smooth(u));
        }
    }
    return keys[count - 1];
}

// Ноги врозь в устойчивой стойке (бой, работа).
void stance(HumanPose& p, float bounce) {
    p.leftLeg = leg(0.22f, 0.12f, 0.3f + bounce);
    p.rightLeg = leg(-0.2f, 0.12f, 0.3f + bounce);
}

// На коленях: таз на высоте голени.
void kneel(HumanPose& p) {
    p.leftLeg = leg(0.1f, 0.08f, 1.6f);
    p.rightLeg = leg(0.1f, 0.08f, 1.6f);
}

// На корточках.
void crouch(HumanPose& p) {
    p.leftLeg = leg(1.5f, 0.2f, 2.0f);
    p.rightLeg = leg(1.5f, 0.2f, 2.0f);
}

// Сидит на земле: ноги вперёд, правая согнута коленом вверх.
void sitLegs(HumanPose& p) {
    p.seat = kLyingPelvis;
    p.leftLeg = leg(1.5f, 0.1f, 0.05f);
    p.rightLeg = leg(2.2f, 0.1f, 2.4f);
}

// Руки скрещены на груди.
void crossArms(HumanPose& p) {
    p.leftArm = arm(0.55f, -0.55f, 1.75f);
    p.rightArm = arm(0.5f, -0.5f, 1.8f);
}

// Руки за спиной.
void armsBehind(HumanPose& p) {
    p.leftArm = arm(-0.3f, -0.12f, 1.1f);
    p.rightArm = arm(-0.3f, -0.12f, 1.1f);
}

// Угол инструмента от предплечья, чтобы черенок шёл под углом world от
// вертикали вперёд: рука наклоняется вместе с корпусом.
float toolFor(const ArmPose& hand, float lean, float world) {
    return world - (hand.raise + hand.elbow - lean);
}

// Человек с руками у лица — плачет, прячет лицо.
void handsToFace(HumanPose& p) {
    p.leftArm = arm(1.35f, -0.15f, 2.45f);
    p.rightArm = arm(1.35f, -0.15f, 2.45f);
}

} // namespace

HumanPose blend(const HumanPose& a, const HumanPose& b, float t) {
    HumanPose p;
    p.lean = lerpf(a.lean, b.lean, t);
    p.side = lerpf(a.side, b.side, t);
    p.twist = lerpf(a.twist, b.twist, t);
    p.headYaw = lerpf(a.headYaw, b.headYaw, t);
    p.headPitch = lerpf(a.headPitch, b.headPitch, t);
    p.headTilt = lerpf(a.headTilt, b.headTilt, t);
    p.leftLeg = lerpLeg(a.leftLeg, b.leftLeg, t);
    p.rightLeg = lerpLeg(a.rightLeg, b.rightLeg, t);
    p.leftArm = lerpArm(a.leftArm, b.leftArm, t);
    p.rightArm = lerpArm(a.rightArm, b.rightArm, t);
    p.seat = lerpf(a.seat, b.seat, t);
    p.bodyPitch = lerpf(a.bodyPitch, b.bodyPitch, t);
    p.bodyRoll = lerpf(a.bodyRoll, b.bodyRoll, t);
    p.toolPitch = lerpf(a.toolPitch, b.toolPitch, t);
    return p;
}

float lyingness(const HumanPose& pose) {
    const float angle = std::max(std::abs(pose.bodyPitch), std::abs(pose.bodyRoll));
    return std::sin(std::min(angle, kHalfPi));
}

float pelvisHeight(const HumanPose& pose) {
    const auto support = [](const LegPose& leg) {
        return (kThigh * std::cos(leg.hip) + kShin * std::cos(leg.hip - leg.knee)) * std::cos(leg.spread);
    };
    // Сидящий опирается на сиденье, стоящий — на ту ногу, что прямее.
    const float standing = pose.seat > 0.0f ? pose.seat : std::max(support(pose.leftLeg), support(pose.rightLeg));
    const float height = std::max(standing, kLyingPelvis);
    return lerpf(height, kLyingPelvis, lyingness(pose));
}

AnimalPose blend(const AnimalPose& a, const AnimalPose& b, float t) {
    AnimalPose p;
    p.graze = lerpf(a.graze, b.graze, t);
    p.lookUp = lerpf(a.lookUp, b.lookUp, t);
    p.headYaw = lerpf(a.headYaw, b.headYaw, t);
    p.ears = lerpf(a.ears, b.ears, t);
    p.scratch = lerpf(a.scratch, b.scratch, t);
    p.scratchPhase = b.scratchPhase;
    p.sitUp = lerpf(a.sitUp, b.sitUp, t);
    p.charge = lerpf(a.charge, b.charge, t);
    return p;
}

namespace pose {

float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float wave(float t, float period, float offset) {
    return 0.5f - 0.5f * std::cos(kTau * (t / period + offset));
}

// --- Стоя ----------------------------------------------------------------------

HumanPose stand(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    // Переминается с ноги на ногу: вес то на левой, то на правой.
    const float period = 5.0f + 3.0f * unit(seed, 1);
    const float shift = std::sin(kTau * (t / period + unit(seed, 2)));
    p.side = 0.03f * shift;
    p.leftLeg.knee = 0.12f * std::max(0.0f, -shift);
    p.rightLeg.knee = 0.12f * std::max(0.0f, shift);
    p.headYaw = 0.2f * std::sin(t * 0.31f + 6.0f * unit(seed, 3));
    p.headPitch = 0.04f * std::sin(t * 0.23f);
    // Дыхание.
    p.leftArm.out += 0.015f * std::sin(t * 1.6f);
    p.rightArm.out += 0.015f * std::sin(t * 1.6f);
    return p;
}

HumanPose lookAround(float t, std::uint32_t seed, float nervous) {
    HumanPose p = stand(t, seed);
    // Взгляд перескакивает с места на место и задерживается.
    static constexpr float kTargets[] = {-1.0f, 0.3f, 1.0f, -0.4f, 0.7f, -0.8f};
    const float period = 2.6f - 1.4f * nervous;
    const float steps = t / period + 6.0f * unit(seed, 4);
    const int index = static_cast<int>(std::floor(steps));
    const float u = smooth((steps - static_cast<float>(index)) / 0.35f);
    const float from = kTargets[static_cast<unsigned>(index) % std::size(kTargets)];
    const float to = kTargets[static_cast<unsigned>(index + 1) % std::size(kTargets)];
    const float look = lerpf(from, to, u);
    p.headYaw = 0.95f * look;
    p.twist = 0.25f * look;
    p.headPitch = -0.05f;
    if (nervous > 0.0f) {
        p.leftArm = arm(0.3f * nervous, 0.1f, 0.15f + 0.6f * nervous);
        p.rightArm = arm(0.3f * nervous, 0.1f, 0.15f + 0.6f * nervous);
        p.leftLeg.knee += 0.15f * nervous;
        p.rightLeg.knee += 0.15f * nervous;
    }
    return p;
}

HumanPose stretch(float t) {
    HumanPose p = relaxed();
    const float e = smooth(t / 1.2f) * (1.0f - smooth((t - 2.6f) / 1.0f));
    p.leftArm = arm(2.9f * e, 0.06f + 0.25f * e, 0.15f + 0.15f * e);
    p.rightArm = arm(2.9f * e, 0.06f + 0.25f * e, 0.15f + 0.15f * e);
    p.lean = -0.15f * e;
    p.headPitch = -0.35f * e;
    p.side = 0.06f * e * std::sin(t * 1.8f);
    return p;
}

HumanPose armsCrossed(float t, std::uint32_t seed) {
    HumanPose p = stand(t, seed);
    crossArms(p);
    p.headTilt = 0.05f;
    return p;
}

HumanPose handsOnHips(float t, std::uint32_t seed) {
    HumanPose p = stand(t, seed);
    p.leftArm = arm(-0.35f, 0.45f, 1.4f);
    p.rightArm = arm(-0.35f, 0.45f, 1.4f);
    return p;
}

HumanPose scratchHead(float t) {
    HumanPose p = relaxed();
    p.rightArm = arm(2.5f, 0.35f, 2.3f + 0.1f * std::sin(t * 12.0f));
    p.headTilt = -0.15f;
    p.headPitch = 0.1f;
    return p;
}

// --- Ходьба ----------------------------------------------------------------------

HumanPose walk(Gait gait, std::uint32_t seed) {
    HumanPose p = relaxed();
    const float s = std::sin(gait.phase);
    const float c = std::cos(gait.phase);
    const float w = std::min(gait.swing, 1.3f);
    const float amp = 0.42f * std::min(w, 1.2f);
    p.leftLeg = leg(amp * s, 0.04f, 0.15f * w + 0.55f * w * std::max(0.0f, c));
    p.rightLeg = leg(-amp * s, 0.04f, 0.15f * w + 0.55f * w * std::max(0.0f, -c));
    // Руки — против ног; у каждого свой размах.
    const float arms = 0.7f * (0.8f + 0.4f * unit(seed, 5));
    p.leftArm = arm(-arms * amp * s, 0.06f, 0.2f + 0.25f * w);
    p.rightArm = arm(arms * amp * s, 0.06f, 0.2f + 0.25f * w);
    p.twist = -0.06f * s * w;
    p.lean = 0.04f * w;
    return p;
}

HumanPose run(Gait gait) {
    HumanPose p = relaxed();
    const float s = std::sin(gait.phase);
    const float c = std::cos(gait.phase);
    const float w = std::clamp(gait.swing, 0.0f, 1.3f);
    const float amp = 0.75f * std::min(w, 1.0f);
    p.leftLeg = leg(amp * s, 0.04f, 0.3f * w + 1.3f * w * std::max(0.0f, c));
    p.rightLeg = leg(-amp * s, 0.04f, 0.3f * w + 1.3f * w * std::max(0.0f, -c));
    p.leftArm = arm(-0.9f * amp * s, 0.08f, 1.5f);
    p.rightArm = arm(0.9f * amp * s, 0.08f, 1.5f);
    p.lean = 0.3f;
    p.headPitch = -0.2f;
    p.twist = -0.1f * s;
    return p;
}

HumanPose sneak(Gait gait) {
    HumanPose p = relaxed();
    const float s = std::sin(gait.phase);
    const float c = std::cos(gait.phase);
    const float w = std::min(gait.swing, 1.0f);
    p.leftLeg = leg(0.55f + 0.3f * s * w, 0.08f, 1.2f + 0.4f * w * std::max(0.0f, c));
    p.rightLeg = leg(0.55f - 0.3f * s * w, 0.08f, 1.2f + 0.4f * w * std::max(0.0f, -c));
    p.lean = 0.45f;
    p.headPitch = -0.35f;
    p.leftArm = arm(0.9f, -0.1f, 0.9f);
    p.rightArm = arm(0.9f, -0.1f, 0.9f);
    return p;
}

HumanPose patrol(Gait gait) {
    HumanPose p = walk({gait.phase, std::min(gait.swing, 0.85f)}, 0);
    armsBehind(p);
    p.headYaw = 0.45f * std::sin(gait.phase * 0.25f);
    return p;
}

HumanPose limp(Gait gait) {
    HumanPose p = relaxed();
    const float s = std::sin(gait.phase);
    const float c = std::cos(gait.phase);
    const float w = std::min(gait.swing, 1.0f);
    // Правая нога ранена: короткий прямой шаг, тело проваливается на неё.
    p.leftLeg = leg(0.45f * s * w, 0.05f, 0.15f * w + 0.55f * w * std::max(0.0f, c));
    p.rightLeg = leg(-0.2f * s * w, 0.05f, 0.05f);
    p.side = 0.12f * s * w;
    p.lean = 0.15f;
    p.headPitch = 0.15f;
    p.leftArm = arm(-0.3f * s * w, 0.1f, 0.3f);
    p.rightArm = arm(0.3f, -0.45f, 1.7f); // держится за рану
    return p;
}

HumanPose backAway(Gait gait) {
    HumanPose p = walk({gait.phase, std::min(gait.swing, 0.8f)}, 0);
    // Пятится: ноги шагают назад, руки выставлены перед собой.
    std::swap(p.leftLeg.hip, p.rightLeg.hip);
    p.leftArm = arm(0.9f, 0.1f, 1.1f);
    p.rightArm = arm(0.9f, 0.1f, 1.1f);
    p.lean = -0.05f;
    return p;
}

HumanPose pocketsWalk(Gait gait) {
    HumanPose p = walk(gait, 0);
    p.leftArm = arm(-0.1f, 0.12f, 0.6f);
    p.rightArm = arm(-0.1f, 0.12f, 0.6f);
    p.headPitch = 0.1f;
    return p;
}

// --- Отдых -----------------------------------------------------------------------

HumanPose sitGround(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    sitLegs(p);
    p.lean = -0.2f;
    p.leftArm = arm(-0.55f, 0.2f, 0.0f); // опирается сзади
    p.rightArm = arm(0.9f, 0.05f, 0.6f); // рука на колене
    p.headYaw = 0.35f * std::sin(t * 0.21f + 6.0f * unit(seed, 6));
    p.headPitch = -0.05f;
    return p;
}

HumanPose sitKnees(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    p.seat = kLyingPelvis;
    p.leftLeg = leg(2.2f, 0.08f, 2.4f);
    p.rightLeg = leg(2.2f, 0.08f, 2.4f);
    p.lean = 0.35f;
    p.leftArm = arm(1.0f, -0.15f, 0.7f);
    p.rightArm = arm(1.0f, -0.15f, 0.7f);
    p.headPitch = 0.15f + 0.05f * std::sin(t * 0.4f);
    p.headYaw = 0.25f * std::sin(t * 0.17f + 6.0f * unit(seed, 7));
    return p;
}

HumanPose sitStump(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    p.seat = 0.45f;
    p.leftLeg = leg(1.45f, 0.14f, 1.45f);
    p.rightLeg = leg(1.45f, 0.14f, 1.45f);
    p.lean = 0.35f;
    p.leftArm = arm(0.55f, 0.0f, 1.0f);
    p.rightArm = arm(0.55f, 0.0f, 1.0f);
    if (unit(seed, 8) < 0.4f) {
        p.rightArm = arm(0.9f, -0.15f, 2.3f); // подпёр подбородок
    }
    p.headPitch = -0.1f;
    p.headYaw = 0.3f * std::sin(t * 0.19f + 6.0f * unit(seed, 9));
    return p;
}

HumanPose lieBack(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    p.bodyPitch = kHalfPi;
    p.leftArm = arm(2.8f, 0.45f, 2.1f);
    p.rightArm = arm(2.8f, 0.45f, 2.1f);
    if (unit(seed, 10) < 0.5f) {
        p.rightLeg = leg(0.6f, 0.05f, 1.2f); // колено вверх
    }
    p.lean = 0.01f * std::sin(t * 1.2f);
    p.headYaw = 0.2f * std::sin(t * 0.13f);
    return p;
}

HumanPose sitChair(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    p.seat = kChairSeat;
    p.leftLeg = leg(1.5f, 0.08f, 1.5f);
    p.rightLeg = leg(1.5f, 0.08f, 1.5f);
    if (unit(seed, 21) < 0.35f) {
        p.rightLeg = leg(1.3f, 0.05f, 1.05f); // нога вытянута вперёд
    }
    p.lean = 0.06f;
    p.leftArm = arm(0.5f, 0.04f, 1.1f);
    p.rightArm = arm(0.5f, 0.04f, 1.1f);
    p.headYaw = 0.25f * std::sin(t * 0.17f + 6.0f * unit(seed, 22));
    p.headPitch = 0.05f + 0.03f * std::sin(t * 0.3f);
    return p;
}

HumanPose sitSofa(float t, std::uint32_t seed) {
    HumanPose p = relaxed();
    p.seat = kSofaSeat;
    p.leftLeg = leg(1.3f, 0.12f, 1.15f);
    p.rightLeg = leg(1.3f, 0.12f, 1.15f);
    if (unit(seed, 23) < 0.4f) {
        p.rightLeg = leg(1.55f, -0.12f, 1.25f); // нога на ногу
    }
    p.lean = -0.2f;
    p.leftArm = arm(0.2f, 0.4f, 0.8f); // на подлокотнике
    p.rightArm = arm(0.55f, 0.0f, 1.0f);
    p.headYaw = 0.2f * std::sin(t * 0.11f + 6.0f * unit(seed, 24));
    p.headPitch = -0.05f;
    return p;
}

HumanPose eatTable(float t) {
    HumanPose p = sitChair(t, 0);
    // Локти на столе, ложку — ко рту.
    const float cycle = std::fmod(t, 3.6f);
    const float e = smooth(cycle / 0.6f) * (1.0f - smooth((cycle - 1.3f) / 0.6f));
    p.leftArm = arm(0.75f, -0.05f, 1.0f);
    p.rightArm = arm(lerpf(0.75f, 1.05f, e), lerpf(-0.05f, -0.25f, e), lerpf(1.0f, 2.3f, e));
    p.lean = 0.18f;
    p.headPitch = lerpf(0.2f, 0.0f, e) + 0.03f * std::sin(t * 9.0f);
    return p;
}

HumanPose talkSeated(float t, std::uint32_t seed) {
    HumanPose p = sitChair(t, seed);
    const HumanPose up = talk(t, seed, 0.45f);
    p.rightArm = up.rightArm;
    p.leftArm = arm(0.7f, -0.05f, 1.0f);
    p.headYaw = up.headYaw;
    p.headPitch = up.headPitch;
    p.headTilt = up.headTilt;
    p.lean = 0.12f;
    return p;
}

HumanPose bandageSeated(float t) {
    HumanPose p = sitChair(t, 0);
    const HumanPose up = bandage(t);
    p.leftArm = up.leftArm;
    p.rightArm = up.rightArm;
    p.lean = 0.3f;
    p.headPitch = 0.4f;
    return p;
}

HumanPose leanWall(float t, std::uint32_t seed) {
    HumanPose p = stand(t, seed);
    p.bodyPitch = 0.12f;
    p.side = 0.0f;
    p.rightLeg = leg(0.2f, -0.18f, 0.25f); // нога за ногу
    if (unit(seed, 11) < 0.5f) {
        crossArms(p);
    } else {
        p.leftArm = arm(-0.1f, 0.12f, 0.6f);
        p.rightArm = arm(-0.1f, 0.12f, 0.6f);
    }
    return p;
}

// --- Еда, питьё, сон ---------------------------------------------------------------

HumanPose eat(float t, bool sitting) {
    HumanPose p = sitting ? sitGround(t, 0) : stand(t, 0);
    // Подносит еду ко рту и жуёт.
    const float cycle = std::fmod(t, 3.2f);
    const float e = smooth(cycle / 0.5f) * (1.0f - smooth((cycle - 1.1f) / 0.5f));
    p.rightArm = arm(lerpf(0.5f, 1.0f, e), lerpf(0.05f, -0.2f, e), lerpf(1.2f, 2.4f, e));
    p.leftArm = arm(0.5f, 0.0f, 1.3f);
    p.headPitch = lerpf(0.12f, -0.05f, e) + 0.03f * std::sin(t * 9.0f);
    if (sitting) {
        p.lean = 0.1f;
    }
    return p;
}

HumanPose drinkBottle(float t) {
    HumanPose p = stand(t, 0);
    const float cycle = std::fmod(t, 4.0f);
    const float e = smooth(cycle / 0.6f) * (1.0f - smooth((cycle - 2.0f) / 0.6f));
    p.rightArm = arm(lerpf(0.4f, 1.3f, e), lerpf(0.05f, -0.15f, e), lerpf(1.2f, 2.2f, e));
    p.headPitch = -0.45f * e;
    p.lean = -0.05f * e;
    return p;
}

HumanPose scoopWater(float t) {
    HumanPose low = relaxed();
    kneel(low);
    low.lean = 1.0f;
    low.leftArm = arm(1.5f, -0.1f, 0.2f);
    low.rightArm = arm(1.5f, -0.1f, 0.2f);
    low.headPitch = 0.2f;
    HumanPose up = low;
    up.lean = 0.35f;
    up.leftArm = arm(1.0f, -0.2f, 2.3f);
    up.rightArm = arm(1.0f, -0.2f, 2.3f);
    up.headPitch = -0.2f;
    const float cycle = std::fmod(t, 3.6f);
    const float e = smooth((cycle - 1.0f) / 0.7f) * (1.0f - smooth((cycle - 2.6f) / 0.7f));
    return blend(low, up, e);
}

HumanPose sleepBack(float t) {
    HumanPose p = relaxed();
    p.bodyPitch = kHalfPi;
    p.leftArm = arm(0.35f, -0.1f, 1.2f);
    p.rightArm = arm(0.35f, -0.1f, 1.2f);
    p.lean = 0.015f * std::sin(t * 1.1f); // дышит
    p.headYaw = 0.5f;
    return p;
}

HumanPose sleepCurled(float t) {
    HumanPose p = relaxed();
    p.bodyRoll = kHalfPi;
    p.leftLeg = leg(1.2f, 0.0f, 1.7f);
    p.rightLeg = leg(1.0f, 0.0f, 1.5f);
    p.leftArm = arm(1.3f, -0.2f, 1.4f);
    p.rightArm = arm(1.4f, -0.1f, 1.6f);
    p.lean = 0.25f + 0.015f * std::sin(t * 1.1f);
    p.headPitch = 0.35f;
    return p;
}

// --- Работа ----------------------------------------------------------------------

HumanPose dig(float t) {
    // Воткнуть лопату ногой → поднять землю → отбросить в сторону.
    HumanPose thrust = relaxed();
    thrust.leftLeg = leg(0.15f, 0.14f, 0.2f);
    thrust.rightLeg = leg(0.3f, 0.14f, 0.45f);
    thrust.lean = 0.5f;
    thrust.leftArm = arm(0.75f, -0.15f, 0.5f);
    thrust.rightArm = arm(0.7f, -0.05f, 0.4f);
    thrust.toolPitch = toolFor(thrust.rightArm, thrust.lean, 0.35f);
    thrust.headPitch = 0.3f;
    HumanPose lift = thrust;
    lift.rightLeg = leg(-0.1f, 0.14f, 0.25f);
    lift.lean = 0.35f;
    lift.leftArm = arm(1.0f, -0.2f, 1.0f);
    lift.rightArm = arm(1.1f, -0.05f, 0.9f);
    lift.toolPitch = toolFor(lift.rightArm, lift.lean, 1.1f);
    HumanPose toss = lift;
    toss.twist = 0.5f;
    toss.leftArm = arm(0.9f, -0.1f, 0.9f);
    toss.rightArm = arm(0.85f, 0.1f, 0.6f);
    toss.toolPitch = toolFor(toss.rightArm, toss.lean, 1.4f);
    static constexpr float kTimes[] = {0.0f, 0.7f, 1.2f, 1.7f, 2.4f};
    const HumanPose keys[] = {thrust, thrust, lift, toss, thrust};
    return keyed(keys, kTimes, 5, std::fmod(t, 2.4f));
}

HumanPose reap(float t) {
    HumanPose p = relaxed();
    stance(p, 0.0f);
    const float s = std::sin(kTau * t / 2.2f);
    p.twist = 0.55f * s;
    p.lean = 0.3f;
    p.leftArm = arm(0.85f, 0.1f, 0.5f);
    p.rightArm = arm(0.75f, 0.1f, 0.4f);
    p.toolPitch = toolFor(p.rightArm, p.lean, 1.0f);
    p.headPitch = 0.25f;
    return p;
}

HumanPose counter(float t, std::uint32_t seed) {
    HumanPose p = stand(t, seed);
    p.lean = 0.12f;
    p.leftArm = arm(0.75f, -0.05f, 0.55f);
    p.rightArm = arm(0.75f, -0.05f, 0.55f);
    // Время от времени объясняет что-то покупателю.
    const float e = wave(t, 7.0f + 3.0f * unit(seed, 12));
    const float g = smooth((e - 0.6f) / 0.3f);
    p.rightArm = arm(lerpf(0.75f, 1.15f, g), 0.0f, lerpf(0.55f, 1.0f, g) + 0.15f * g * std::sin(t * 4.0f));
    return p;
}

HumanPose examine(float t) {
    HumanPose p = relaxed();
    p.leftLeg.knee = 0.2f;
    p.rightLeg.knee = 0.2f;
    p.lean = 0.55f;
    p.leftArm = arm(1.05f + 0.1f * std::sin(t * 1.3f), -0.1f, 0.5f);
    p.rightArm = arm(1.05f + 0.1f * std::sin(t * 1.3f + 1.7f), -0.1f, 0.5f);
    p.headPitch = 0.35f;
    return p;
}

HumanPose clipboard(float t) {
    HumanPose p = stand(t, 0);
    p.rightArm = arm(0.55f, -0.1f, 1.45f);
    p.leftArm = arm(0.6f, -0.25f, 1.5f + 0.08f * std::sin(t * 9.0f));
    // Пишет, иногда поднимает голову.
    p.headPitch = 0.45f - 0.4f * smooth((wave(t, 6.0f) - 0.7f) / 0.2f);
    return p;
}

HumanPose lecture(float t, std::uint32_t seed) {
    HumanPose p = stand(t, seed);
    p.rightArm = arm(0.9f + 0.35f * wave(t, 2.3f, unit(seed, 13)), 0.05f, 0.7f + 0.3f * std::sin(t * 2.9f));
    p.leftArm = arm(-0.35f, 0.45f, 1.4f);
    p.headYaw = 0.5f * std::sin(t * 0.5f);
    return p;
}

HumanPose patrolStand(float t, std::uint32_t seed) {
    HumanPose p = lookAround(t * 0.6f, seed, 0.0f);
    armsBehind(p);
    p.leftLeg.spread = 0.1f;
    p.rightLeg.spread = 0.1f;
    return p;
}

// --- Обыск, сбор, разделка -----------------------------------------------------------

HumanPose rummage(float t) {
    HumanPose p = relaxed();
    crouch(p);
    p.lean = 0.45f;
    const float w = kTau * t / 1.1f;
    p.leftArm = arm(0.65f + 0.3f * std::sin(w), -0.1f, 0.6f + 0.3f * std::cos(w));
    p.rightArm = arm(0.65f - 0.3f * std::sin(w), -0.1f, 0.6f - 0.3f * std::cos(w));
    p.headPitch = 0.3f;
    return p;
}

HumanPose reachHigh(float t) {
    // Открывает шкафы и полки, достаёт, разглядывает.
    HumanPose reach = relaxed();
    reach.leftArm = arm(2.2f, 0.0f, 0.4f);
    reach.rightArm = arm(1.9f, 0.0f, 0.8f);
    reach.headPitch = -0.3f;
    HumanPose look = relaxed();
    look.leftArm = arm(0.9f, -0.15f, 1.3f);
    look.rightArm = arm(0.9f, -0.15f, 1.4f);
    look.headPitch = 0.35f;
    static constexpr float kTimes[] = {0.0f, 0.6f, 1.4f, 2.0f, 3.0f, 3.6f};
    const HumanPose keys[] = {look, reach, reach, look, look, look};
    HumanPose p = keyed(keys, kTimes, 6, std::fmod(t, 3.6f));
    p.rightArm.raise += 0.15f * std::sin(t * 5.0f) * smooth(1.0f - std::abs(std::fmod(t, 3.6f) - 1.0f));
    return p;
}

HumanPose pick(float t, bool low) {
    HumanPose p = relaxed();
    float lean = 1.15f;
    if (low) {
        crouch(p);
        lean = 0.45f;
    } else {
        p.leftLeg.knee = 0.15f;
        p.rightLeg.knee = 0.15f;
    }
    p.lean = lean;
    p.leftArm = arm(lean * 0.9f + 0.1f, 0.0f, 0.3f);
    // Срывает и кладёт в сумку на боку.
    const float cycle = std::fmod(t, 2.5f);
    const float e = smooth((cycle - 1.3f) / 0.4f) * (1.0f - smooth((cycle - 2.0f) / 0.4f));
    const ArmPose reach = arm(lean + 0.25f, -0.05f, 0.15f + 0.1f * std::sin(t * 7.0f));
    const ArmPose bag = arm(0.3f, 0.25f, 1.2f);
    p.rightArm = lerpArm(reach, bag, e);
    p.headPitch = 0.2f;
    return p;
}

HumanPose butcher(float t) {
    HumanPose p = relaxed();
    p.leftLeg = leg(1.4f, 0.1f, 1.5f);
    p.rightLeg = leg(0.05f, 0.1f, 1.55f);
    p.lean = 0.65f;
    const float s = std::sin(kTau * t / 0.9f);
    p.rightArm = arm(0.95f + 0.35f * s, 0.0f, 0.6f - 0.3f * s);
    p.leftArm = arm(1.05f, -0.1f, 0.3f);
    p.headPitch = 0.3f;
    return p;
}

// --- Общение -----------------------------------------------------------------------

HumanPose talk(float t, std::uint32_t seed, float energy) {
    HumanPose p = stand(t, seed);
    const float a = unit(seed, 14);
    p.rightArm = arm(0.45f + 0.35f * energy * wave(t, 1.7f + a, a), 0.05f, 1.3f + 0.3f * std::sin(t * (2.3f + a)));
    p.leftArm = arm(0.35f + 0.2f * energy * wave(t, 2.3f - a, 0.5f), 0.05f, 1.1f + 0.2f * std::sin(t * 1.9f));
    p.headPitch = 0.06f * std::sin(t * 3.1f);
    p.headYaw = 0.1f * std::sin(t * 0.9f);
    return p;
}

HumanPose nod(float t) {
    HumanPose p = stand(t, 0);
    p.headPitch = 0.22f * std::max(0.0f, std::sin(t * 5.0f)) * (1.0f - smooth((std::fmod(t, 3.0f) - 1.2f) / 0.3f));
    p.leftArm = arm(0.1f, 0.06f, 0.4f);
    p.rightArm = arm(0.1f, 0.06f, 0.4f);
    return p;
}

HumanPose laugh(float t) {
    HumanPose calm = stand(t, 0);
    HumanPose p = calm;
    const float shake = std::sin(t * 14.0f);
    p.headPitch = -0.3f + 0.05f * shake;
    p.lean = -0.1f + 0.05f * shake;
    p.leftArm = arm(0.4f, -0.3f, 1.5f);
    p.rightArm = arm(0.4f, -0.3f, 1.5f);
    // Отсмеялся — снова спокоен.
    return blend(p, calm, smooth((t - 2.5f) / 1.0f));
}

HumanPose listen(float t, std::uint32_t seed) {
    HumanPose p = armsCrossed(t, seed);
    p.headTilt = 0.12f;
    p.headPitch = 0.1f * std::max(0.0f, std::sin(t * 1.3f)) * std::max(0.0f, std::sin(t * 0.4f));
    return p;
}

HumanPose argue(float t, std::uint32_t seed) {
    HumanPose point = stand(t, seed);
    point.rightArm = arm(1.4f + 0.12f * std::sin(t * 7.0f), -0.05f, 0.1f); // тычет пальцем
    point.leftArm = arm(0.6f + 0.4f * wave(t, 1.3f), 0.1f, 1.0f);
    point.lean = 0.15f + 0.05f * std::sin(t * 3.0f);
    point.twist = 0.15f * std::sin(t * 2.0f);
    HumanPose throwUp = point;
    throwUp.leftArm = arm(1.2f, 0.45f, 1.1f);
    throwUp.rightArm = arm(1.2f, 0.45f, 1.1f);
    throwUp.lean = -0.05f;
    throwUp.headPitch = -0.15f;
    return blend(point, throwUp, smooth((wave(t, 4.0f, unit(seed, 15)) - 0.6f) / 0.25f));
}

HumanPose hug(float t) {
    HumanPose p = relaxed();
    p.leftArm = arm(1.35f, 0.35f, 1.5f);
    p.rightArm = arm(1.35f, 0.35f, 1.5f);
    p.lean = 0.1f;
    p.headTilt = 0.2f;
    p.side = 0.05f * std::sin(t * 1.5f);
    return p;
}

HumanPose offer(float t) {
    HumanPose p = stand(t, 0);
    const float e = smooth(t / 0.6f);
    p.rightArm = arm(lerpf(0.3f, 1.25f, e), 0.0f, lerpf(1.0f, 0.25f, e));
    p.lean = 0.08f * e;
    return p;
}

// --- Бой ----------------------------------------------------------------------------

HumanPose guard(float t, int weapon) {
    HumanPose p = relaxed();
    stance(p, 0.05f * std::sin(t * 4.0f));
    p.lean = 0.12f;
    switch (weapon) {
    case 0:
        p.leftArm = arm(1.0f, -0.1f, 1.8f);
        p.rightArm = arm(0.95f, -0.1f, 1.9f);
        break;
    case 1:
        p.rightArm = arm(0.9f, 0.0f, 1.1f);
        p.leftArm = arm(0.8f, -0.1f, 1.6f);
        break;
    case 2:
        p.rightArm = arm(1.4f, 0.1f, 1.2f);
        p.leftArm = arm(0.9f, -0.1f, 1.4f);
        break;
    default:
        p = aim(t, weapon);
        p.rightArm.raise -= 0.4f;
        p.leftArm.raise -= 0.4f;
        break;
    }
    return p;
}

HumanPose aim(float t, int weapon) {
    HumanPose p = relaxed();
    stance(p, 0.0f);
    if (weapon == 3) {
        p.rightArm = arm(1.5f, -0.1f, 0.0f);
        p.leftArm = arm(1.45f, -0.3f, 0.15f);
    } else {
        p.rightArm = arm(1.45f, -0.05f, 0.0f);
        p.leftArm = arm(1.35f, -0.35f, 0.25f);
        p.headTilt = 0.1f;
        p.twist = 0.15f;
    }
    p.headYaw = -0.1f;
    // Дыхание чуть водит стволом.
    p.rightArm.raise += 0.015f * std::sin(t * 1.7f);
    p.leftArm.raise += 0.015f * std::sin(t * 1.7f);
    return p;
}

HumanPose reload(float t, int weapon) {
    HumanPose p = relaxed();
    stance(p, 0.0f);
    const float work = 0.25f * std::sin(t * 9.0f);
    if (weapon == 3) {
        p.rightArm = arm(0.9f, -0.1f, 0.8f);
        p.leftArm = arm(0.8f, -0.3f, 1.2f + work);
    } else {
        p.rightArm = arm(0.8f, -0.05f, 0.7f);
        p.leftArm = arm(0.75f, -0.35f, 1.0f + work);
    }
    p.headPitch = 0.45f;
    return p;
}

float attackLength(int weapon) {
    switch (weapon) {
    case 2:
        return 0.65f;
    case 3:
        return 0.3f;
    case 4:
        return 0.6f;
    default:
        return 0.45f;
    }
}

HumanPose attack(const HumanPose& base, float age, int weapon) {
    if (age < 0.0f || age >= attackLength(weapon)) {
        return base;
    }
    if (weapon >= 3) {
        // Отдача: ствол подбрасывает, плечо уходит назад.
        const float k = age < 0.05f ? age / 0.05f : std::exp(-(age - 0.05f) / 0.07f);
        HumanPose p = base;
        p.rightArm.raise += 0.3f * k;
        p.leftArm.raise += 0.25f * k;
        p.lean -= 0.08f * k;
        p.headPitch -= 0.1f * k;
        if (weapon == 4) {
            // Перезарядка затвором или цевьём после выстрела.
            const float cycle = smooth((age - 0.25f) / 0.1f) * (1.0f - smooth((age - 0.45f) / 0.12f));
            p.leftArm.elbow += 0.7f * cycle;
            p.leftArm.raise -= 0.2f * cycle;
        }
        return p;
    }
    HumanPose wind = base;
    HumanPose strike = base;
    float windAt = 0.12f;
    float strikeAt = 0.24f;
    if (weapon == 2) {
        // Топор — сверху.
        wind.rightArm = arm(2.9f, 0.1f, 0.8f);
        wind.leftArm = arm(2.5f, -0.1f, 0.8f);
        wind.lean = -0.1f;
        strike.rightArm = arm(0.7f, -0.1f, 0.1f);
        strike.leftArm = arm(0.75f, -0.2f, 0.2f);
        strike.lean = 0.35f;
        windAt = 0.2f;
        strikeAt = 0.35f;
    } else {
        // Кулак или нож — прямой удар правой.
        wind.rightArm = arm(0.9f, 0.0f, 2.0f);
        wind.twist = 0.25f;
        strike.rightArm = arm(weapon == 1 ? 1.3f : 1.55f, -0.1f, 0.05f);
        strike.twist = -0.35f;
        strike.lean = base.lean + 0.12f;
    }
    const float times[] = {0.0f, windAt, strikeAt, attackLength(weapon)};
    const HumanPose keys[] = {base, wind, strike, base};
    return keyed(keys, times, 4, age);
}

// --- Раны, горе, страх ------------------------------------------------------------------

HumanPose bandage(float t) {
    HumanPose p = relaxed();
    p.seat = kLyingPelvis;
    p.leftLeg = leg(1.5f, 0.1f, 0.05f);
    p.rightLeg = leg(1.9f, 0.1f, 2.0f);
    p.lean = 0.55f;
    const float w = t * 4.0f;
    p.rightArm = arm(0.9f + 0.15f * std::sin(w), -0.1f, 0.6f + 0.2f * std::cos(w));
    p.leftArm = arm(0.9f - 0.15f * std::sin(w), -0.3f, 0.6f - 0.2f * std::cos(w));
    p.headPitch = 0.4f;
    return p;
}

HumanPose holdWound(float t) {
    HumanPose p = stand(t, 0);
    p.leftArm = arm(0.3f, -0.55f, 1.75f);
    p.rightArm = arm(0.1f, 0.06f, 0.3f);
    p.rightLeg.knee = 0.2f;
    p.lean = 0.2f;
    p.headPitch = 0.2f;
    p.side = 0.02f * std::sin(t * 4.0f); // тяжело дышит
    return p;
}

HumanPose kneelGrief(float t, bool touch) {
    HumanPose p = relaxed();
    kneel(p);
    p.lean = 0.45f + 0.04f * std::sin(t * 5.0f);
    p.headPitch = 0.55f;
    handsToFace(p);
    if (touch) {
        p.rightArm = arm(1.4f, -0.1f, 0.2f); // рука на теле
    }
    p.side = 0.03f * std::sin(t * 6.0f); // плечи вздрагивают
    return p;
}

HumanPose standGrief(float t) {
    HumanPose p = stand(t, 0);
    p.lean = 0.15f;
    p.headPitch = 0.5f;
    const float e = smooth((wave(t, 9.0f) - 0.3f) / 0.3f);
    HumanPose face = p;
    handsToFace(face);
    p = blend(p, face, e);
    p.side += 0.025f * std::sin(t * 6.0f);
    return p;
}

HumanPose cower(float t) {
    HumanPose p = relaxed();
    p.leftLeg = leg(1.6f, 0.15f, 2.1f);
    p.rightLeg = leg(1.6f, 0.15f, 2.1f);
    p.lean = 0.7f;
    p.leftArm = arm(2.4f, 0.2f, 2.0f);
    p.rightArm = arm(2.4f, 0.2f, 2.0f);
    p.headPitch = 0.4f;
    return tremble(p, t, 1.0f);
}

HumanPose tremble(const HumanPose& base, float t, float amount) {
    HumanPose p = base;
    p.side += 0.025f * amount * std::sin(t * 31.0f);
    p.lean += 0.02f * amount * std::sin(t * 27.0f + 1.0f);
    p.headTilt += 0.03f * amount * std::sin(t * 23.0f);
    p.leftArm.raise += 0.03f * amount * std::sin(t * 29.0f + 2.0f);
    p.rightArm.raise += 0.03f * amount * std::sin(t * 26.0f + 3.0f);
    return p;
}

HumanPose lookAt(const HumanPose& base, float yaw, float pitch, float weight) {
    HumanPose p = base;
    const float wanted = std::clamp(yaw, -1.6f, 1.6f);
    // Голова — до 70°, остальное — плечи.
    const float head = std::clamp(wanted, -1.2f, 1.2f);
    p.headYaw = lerpf(base.headYaw, head * 0.8f, weight);
    p.twist = lerpf(base.twist, base.twist + wanted * 0.2f, weight);
    p.headPitch = lerpf(base.headPitch, std::clamp(pitch, -0.5f, 0.6f), weight * 0.6f);
    return p;
}

// --- Тела и зомби -------------------------------------------------------------------

HumanPose corpse(std::uint32_t seed) {
    HumanPose p = relaxed();
    p.bodyPitch = unit(seed, 20) < 0.6f ? -kHalfPi : kHalfPi; // чаще ничком
    switch (static_cast<int>(unit(seed, 21) * 3.0f)) {
    case 0:
        p.leftArm = arm(2.7f, 0.35f, 0.3f);
        p.rightArm = arm(2.6f, 0.45f, 0.4f);
        break;
    case 1:
        p.leftArm = arm(0.2f, 1.2f, 0.2f);
        p.rightArm = arm(0.1f, 0.15f, 0.1f);
        break;
    default:
        p.leftArm = arm(1.4f, 0.2f, 1.5f);
        p.rightArm = arm(2.2f, 0.6f, 0.6f);
        break;
    }
    p.leftLeg = leg(0.0f, 0.15f + 0.1f * unit(seed, 22), 0.0f);
    p.rightLeg = leg(0.0f, 0.15f, 0.0f);
    if (unit(seed, 23) < 0.5f) {
        p.rightLeg = leg(0.6f, 0.15f, 1.1f);
    }
    p.headYaw = unit(seed, 24) < 0.5f ? 0.9f : -0.9f;
    return p;
}

HumanPose twitch(const HumanPose& base, float t, float amount) {
    HumanPose p = base;
    p.leftArm.raise += 0.6f * amount * spike(t, 7.3f, 0.0f);
    p.rightArm.elbow += 0.8f * amount * spike(t, 6.1f, 1.3f);
    p.leftLeg.knee += 0.5f * amount * spike(t, 5.7f, 2.2f);
    p.headYaw += 0.4f * amount * spike(t, 8.9f, 3.1f);
    p.lean += 0.15f * amount * spike(t, 4.3f, 4.7f);
    return p;
}

HumanPose zombieWalk(Gait gait, int style, bool chasing) {
    HumanPose p = relaxed();
    const float s = std::sin(gait.phase);
    const float c = std::cos(gait.phase);
    const float w = std::min(gait.swing, 1.3f);
    const float amp = chasing ? 0.45f : 0.28f;
    p.leftLeg = leg(amp * s * w, 0.08f, 0.12f + 0.25f * w * std::max(0.0f, c));
    p.rightLeg = leg(-amp * s * w, 0.08f, 0.12f + 0.25f * w * std::max(0.0f, -c));
    if (style == 2) {
        p.rightLeg = leg(-0.4f * amp * s * w, 0.1f, 0.05f); // волочит правую ногу
    } else if (style == 3) {
        p.leftLeg = leg(0.4f * amp * s * w, 0.1f, 0.05f);
    }
    const float sway = std::sin(gait.phase * 0.5f);
    p.lean = chasing ? 0.45f : 0.3f;
    p.side = (chasing ? 0.04f : 0.08f) * sway;
    p.headTilt = 0.15f * (style == 1 ? 1.6f : 1.0f) * std::sin(gait.phase * 0.5f + 1.0f);
    p.headPitch = style == 1 ? 0.35f : 0.05f;
    const float reach = chasing ? 0.1f : 0.0f;
    switch (style) {
    case 0:
        p.leftArm = arm(1.35f + reach + 0.08f * sway, 0.05f, 0.15f);
        p.rightArm = arm(1.35f + reach - 0.08f * sway, 0.05f, 0.15f);
        break;
    case 1:
        p.leftArm = arm(0.15f - 0.2f * s, 0.1f, 0.2f); // руки болтаются
        p.rightArm = arm(0.15f + 0.2f * s, 0.1f, 0.2f);
        break;
    case 2:
        p.leftArm = arm(1.3f + reach + 0.06f * sway, 0.05f, 0.2f);
        p.rightArm = arm(0.1f + 0.15f * s, 0.12f, 0.15f);
        break;
    default:
        p.leftArm = arm(1.1f + reach, 0.1f, 0.6f + 0.1f * sway); // скрюченные пальцы
        p.rightArm = arm(1.1f + reach, 0.1f, 0.6f - 0.1f * sway);
        break;
    }
    return p;
}

HumanPose zombieStand(float t, int style) {
    HumanPose p = zombieWalk({t * 0.9f, 0.0f}, style, false);
    p.side = 0.07f * std::sin(t * 0.9f);
    p.headTilt += 0.1f * std::sin(t * 0.6f);
    // Голова дёргается — изредка, без причины.
    p.headYaw = 0.5f * spike(t, 3.1f, static_cast<float>(style));
    p.headPitch += 0.2f * spike(t, 2.3f, 1.0f + static_cast<float>(style));
    return p;
}

HumanPose zombieFeed(float t) {
    HumanPose p = relaxed();
    kneel(p);
    p.lean = 1.05f;
    p.headPitch = 0.5f + 0.15f * std::sin(t * 5.0f);
    p.leftArm = arm(1.3f + 0.25f * std::sin(t * 3.1f), -0.1f, 0.9f);
    p.rightArm = arm(1.3f + 0.25f * std::sin(t * 3.1f + 2.0f), -0.1f, 0.9f);
    p.side = 0.04f * std::sin(t * 1.7f);
    return p;
}

HumanPose zombieBang(float t) {
    HumanPose p = relaxed();
    stance(p, 0.0f);
    const auto pulse = [](float x) {
        return std::pow(std::max(0.0f, std::sin(kTau * x / 0.8f)), 3.0f);
    };
    p.lean = 0.2f + 0.08f * pulse(t);
    p.leftArm = arm(1.9f - 0.7f * pulse(t), 0.05f, 0.5f);
    p.rightArm = arm(1.9f - 0.7f * pulse(t + 0.4f), 0.05f, 0.5f);
    p.headPitch = 0.1f;
    return p;
}

HumanPose zombieReach(float t, int style) {
    HumanPose p = zombieStand(t, style);
    stance(p, 0.0f);
    p.lean = 0.45f;
    p.leftArm = arm(1.55f + 0.1f * std::sin(t * 6.0f), 0.05f, 0.25f);
    p.rightArm = arm(1.55f + 0.1f * std::sin(t * 6.0f + 1.5f), 0.05f, 0.25f);
    return p;
}

HumanPose zombieAttack(const HumanPose& base, float age) {
    if (age < 0.0f || age >= kZombieAttackLength) {
        return base;
    }
    HumanPose wind = base;
    wind.leftArm = arm(2.0f, 0.15f, 0.5f);
    wind.rightArm = arm(2.0f, 0.15f, 0.5f);
    wind.lean = 0.2f;
    HumanPose strike = base;
    strike.leftArm = arm(1.0f, -0.2f, 0.3f);
    strike.rightArm = arm(1.0f, -0.2f, 0.3f);
    strike.lean = 0.6f;
    strike.headPitch = -0.2f;
    static constexpr float kTimes[] = {0.0f, 0.2f, 0.35f, kZombieAttackLength};
    const HumanPose keys[] = {base, wind, strike, base};
    return keyed(keys, kTimes, 4, age);
}

HumanPose zombieRise(float progress, std::uint32_t seed, int style) {
    HumanPose lying = corpse(seed);
    HumanPose kneeling = relaxed();
    kneel(kneeling);
    kneeling.lean = 0.8f;
    kneeling.headPitch = 0.4f;
    kneeling.leftArm = arm(0.9f, 0.1f, 0.3f);
    kneeling.rightArm = arm(0.9f, 0.1f, 0.3f);
    const HumanPose standing = zombieStand(0.0f, style);
    static constexpr float kTimes[] = {0.0f, 0.35f, 0.65f, 1.0f};
    const HumanPose keys[] = {lying, lying, kneeling, standing};
    const float t = progress * kRiseSeconds;
    return twitch(keyed(keys, kTimes, 4, progress), t, 1.0f - smooth((progress - 0.5f) / 0.5f));
}

// --- Звери ------------------------------------------------------------------------

AnimalPose graze(float t, std::uint32_t seed) {
    AnimalPose p;
    // Пасётся, изредка поднимает голову и жуёт.
    const float period = 7.0f + 4.0f * unit(seed, 30);
    const float cycle = std::fmod(t + period * unit(seed, 31), period);
    const float up = smooth((cycle - period + 2.2f) / 0.4f) * (1.0f - smooth((cycle - period + 0.5f) / 0.4f));
    p.graze = 1.0f - 0.85f * up;
    p.headYaw = 0.15f * std::sin(t * 0.7f + 6.0f * unit(seed, 32));
    p.ears = 0.3f * spike(t, 3.0f, 6.0f * unit(seed, 33));
    return p;
}

AnimalPose listenAnimal(float t, float yaw) {
    AnimalPose p;
    p.lookUp = 1.0f;
    p.headYaw = std::clamp(yaw, -1.2f, 1.2f);
    p.ears = 1.0f;
    p.headYaw += 0.03f * std::sin(t * 2.0f);
    return p;
}

AnimalPose scratchAnimal(float t) {
    AnimalPose p;
    p.scratch = 1.0f;
    p.scratchPhase = t * 14.0f;
    p.graze = 0.3f;
    return p;
}

AnimalPose sitUpHare(float t) {
    AnimalPose p;
    p.sitUp = 1.0f;
    p.ears = 1.0f;
    p.lookUp = 0.5f;
    p.headYaw = 0.4f * std::sin(t * 0.7f);
    return p;
}

} // namespace pose

} // namespace lw
