#pragma once

#include <cstdint>

namespace lw {

// Позы существ 3D-вида (этап 12а.11): углы суставов скелета человека и зверя
// и библиотека движений — сидит, лежит, копает, пьёт, бьёт, падает. Только
// облик: ни симуляция, ни графика здесь не нужны (рисует render/Models3D,
// выбирает позу render/Animator). Углы — в радианах, длины — в метрах.
//
// Скелет человека ростом 1,75 м: таз на высоте kHipHeight, бедро и голень по
// kThigh и kShin, плечо на kShoulderHeight, плечо руки kUpperArm, предплечье
// с кистью kForearm. Оси модели: X — влево от человека, Y — вверх, Z — вперёд.

inline constexpr float kHipHeight = 0.9f;
inline constexpr float kThigh = 0.45f;
inline constexpr float kShin = 0.45f;
inline constexpr float kShoulderHeight = 1.44f;
inline constexpr float kUpperArm = 0.28f;
inline constexpr float kForearm = 0.32f;
// Таз лежащего — на такой высоте над землёй.
inline constexpr float kLyingPelvis = 0.13f;

// Походка: фаза шага (радианы, 2π — два шага) и размах (0 — стоит, 1 — обычный шаг).
struct Gait {
    float phase = 0.0f;
    float swing = 0.0f;
};

struct LegPose {
    float hip = 0.0f;    // бедро вперёд (> 0) или назад
    float spread = 0.0f; // нога в сторону, наружу
    float knee = 0.0f;   // сгиб колена, ≥ 0: голень уходит назад
};

struct ArmPose {
    float raise = 0.0f; // рука вперёд и вверх (> 0) или назад; π — над головой
    float out = 0.0f;   // в сторону, наружу (< 0 — к груди)
    float elbow = 0.0f; // сгиб локтя, ≥ 0: предплечье вперёд и вверх
};

struct HumanPose {
    float lean = 0.0f;  // корпус вперёд (> 0) от таза
    float side = 0.0f;  // корпус вбок, к левому плечу (> 0)
    float twist = 0.0f; // корпус повёрнут влево (> 0)
    float headYaw = 0.0f;   // голова влево (> 0)
    float headPitch = 0.0f; // голова вниз (> 0)
    float headTilt = 0.0f;  // голова к левому плечу (> 0)
    LegPose leftLeg;
    LegPose rightLeg;
    ArmPose leftArm;
    ArmPose rightArm;
    float seat = 0.0f;      // сидит на высоте seat (0 — стоит на ногах)
    // Всё тело вокруг таза: на спину (> 0) или ничком (< 0), на бок (> 0 — на правый).
    // Лёжа таз опускается к земле сам.
    float bodyPitch = 0.0f;
    float bodyRoll = 0.0f;
    float toolPitch = 0.0f; // инструмент в правой руке — от предплечья вперёд
};

// Смесь поз: t = 0 — a, 1 — b.
HumanPose blend(const HumanPose& a, const HumanPose& b, float t);
// Насколько поза лежачая: 0 — стоит или сидит, 1 — лежит.
float lyingness(const HumanPose& pose);
// Высота таза над землёй: ноги стоят на земле, сидящий — на сиденье, лежащий — у земли.
float pelvisHeight(const HumanPose& pose);

// Что человек держит или что стоит рядом с ним — только облик.
enum class Prop : std::uint8_t {
    None,
    Food,      // хлеб в правой руке
    Bottle,    // бутылка воды
    Shovel,    // лопата: копает
    Scythe,    // коса: косит
    Bundle,    // свёрток: протягивает другому
    Bandage,   // бинт
    Clipboard, // папка врача
    Counter,   // прилавок перед продавцом
    Stump,     // чурбак, на котором сидит
};

// Значок эмоции над головой в важный момент.
enum class Emote : std::uint8_t {
    None,
    Alarm,     // «!» красный: испуг, напали
    Discovery, // «!» жёлтый: нашёл, добрался
    Question,  // «?»: узнал новость
    Heart,     // дружба, примирение, помощь
    Anger,     // обида, гнев
    Tear,      // горе
};

// Поза зверя: всё, кроме ног (их ведёт Gait).
struct AnimalPose {
    float graze = 0.0f;   // 0 — голова прямо, 1 — у земли
    float lookUp = 0.0f;  // голова поднята, прислушивается
    float headYaw = 0.0f; // голова влево (> 0)
    float ears = 0.0f;    // уши насторожены
    float scratch = 0.0f; // чешется: 0..1, фаза — scratchPhase
    float scratchPhase = 0.0f;
    float sitUp = 0.0f;   // заяц сел столбиком
    float charge = 0.0f;  // кабан опустил голову для удара
};
AnimalPose blend(const AnimalPose& a, const AnimalPose& b, float t);

// Библиотека движений. t — секунды с начала движения, seed — разброс по
// существу (фазы, амплитуды): у каждого своя манера.
namespace pose {

// Плавный шаг 0 → 1.
float smooth(float t);
// Волна 0..1 с периодом period, сдвинутая на долю offset.
float wave(float t, float period, float offset = 0.0f);

HumanPose stand(float t, std::uint32_t seed);              // стоит, переминается
HumanPose lookAround(float t, std::uint32_t seed, float nervous); // оглядывается
HumanPose stretch(float t);                                // потягивается
HumanPose armsCrossed(float t, std::uint32_t seed);
HumanPose handsOnHips(float t, std::uint32_t seed);
HumanPose scratchHead(float t);

HumanPose walk(Gait gait, std::uint32_t seed);
HumanPose run(Gait gait);
HumanPose sneak(Gait gait);
HumanPose patrol(Gait gait);
HumanPose limp(Gait gait);
HumanPose backAway(Gait gait);
HumanPose pocketsWalk(Gait gait);

HumanPose sitGround(float t, std::uint32_t seed);   // на земле, ноги вперёд, руки сзади
HumanPose sitKnees(float t, std::uint32_t seed);    // на земле, обхватив колени
HumanPose sitStump(float t, std::uint32_t seed);    // на чурбаке, локти на коленях
HumanPose lieBack(float t, std::uint32_t seed);     // лёжа на спине, руки за головой
HumanPose leanWall(float t, std::uint32_t seed);    // прислонился спиной к стене

HumanPose eat(float t, bool sitting);
HumanPose drinkBottle(float t);
HumanPose scoopWater(float t);
HumanPose sleepBack(float t);
HumanPose sleepCurled(float t);

HumanPose dig(float t);
HumanPose reap(float t);
HumanPose counter(float t, std::uint32_t seed);
HumanPose examine(float t);
HumanPose clipboard(float t);
HumanPose lecture(float t, std::uint32_t seed);
HumanPose patrolStand(float t, std::uint32_t seed);

HumanPose rummage(float t);
HumanPose reachHigh(float t);
HumanPose pick(float t, bool crouch);
HumanPose butcher(float t);

HumanPose talk(float t, std::uint32_t seed, float energy); // жесты; energy — горячность
HumanPose nod(float t);
HumanPose laugh(float t);
HumanPose listen(float t, std::uint32_t seed);
HumanPose argue(float t, std::uint32_t seed);
HumanPose hug(float t);
HumanPose offer(float t);

// Бой. weapon — как держит: 0 — кулаки, 1 — нож, 2 — топор, 3 — пистолет, 4 — длинное.
HumanPose guard(float t, int weapon);
HumanPose aim(float t, int weapon);
HumanPose reload(float t, int weapon);
// Удар или выстрел поверх позы: age — секунды с удара.
HumanPose attack(const HumanPose& base, float age, int weapon);
// Сколько длится удар (дальше attack возвращает base).
float attackLength(int weapon);

HumanPose bandage(float t);
HumanPose holdWound(float t);
HumanPose kneelGrief(float t, bool touch);
HumanPose standGrief(float t);
HumanPose cower(float t);
// Дрожь в лихорадке и страхе поверх позы.
HumanPose tremble(const HumanPose& base, float t, float amount);
// Смотрит на цель: голова поворачивается (yaw — куда относительно корпуса).
HumanPose lookAt(const HumanPose& base, float yaw, float pitch, float weight);

// Мёртвое тело: ничком или на спине, руки и ноги — по seed.
HumanPose corpse(std::uint32_t seed);
// Тело дёргается перед тем, как встать: amount 0..1.
HumanPose twitch(const HumanPose& base, float t, float amount);

// Зомби: манера (style 0..3) — по id, на всю «жизнь».
HumanPose zombieWalk(Gait gait, int style, bool chasing);
HumanPose zombieStand(float t, int style);
HumanPose zombieFeed(float t);
HumanPose zombieBang(float t);
HumanPose zombieReach(float t, int style);
HumanPose zombieAttack(const HumanPose& base, float age);
inline constexpr float kZombieAttackLength = 0.6f;
// Встаёт из тела: progress 0..1 за kRiseSeconds.
HumanPose zombieRise(float progress, std::uint32_t seed, int style);
inline constexpr float kRiseSeconds = 3.5f;

// Звери, стоя на месте.
AnimalPose graze(float t, std::uint32_t seed);
AnimalPose listenAnimal(float t, float yaw);
AnimalPose scratchAnimal(float t);
AnimalPose sitUpHare(float t);

} // namespace pose

} // namespace lw
