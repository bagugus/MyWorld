// Тесты поз и движений (этап 12а.11) — без графики: библиотека поз
// (render/Poses) и выбор движения по состоянию мира (render/Animator).
// Облик не входит в хеш мира и не должен его менять; поза всегда конечна,
// стоящий стоит на земле, а выбор движения одинаков при одинаковом входе.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "render/Animator.h"
#include "render/Poses.h"
#include "sim/Components.h"
#include "sim/Death.h"
#include "sim/World.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

bool finite(const lw::HumanPose& p) {
    const float values[] = {p.lean, p.side, p.twist, p.headYaw, p.headPitch, p.headTilt, p.leftLeg.hip,
                            p.leftLeg.spread, p.leftLeg.knee, p.rightLeg.hip, p.rightLeg.spread, p.rightLeg.knee,
                            p.leftArm.raise, p.leftArm.out, p.leftArm.elbow, p.rightArm.raise, p.rightArm.out,
                            p.rightArm.elbow, p.seat, p.bodyPitch, p.bodyRoll, p.toolPitch};
    for (const float v : values) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return std::isfinite(lw::pelvisHeight(p));
}

bool finite(const lw::AnimalPose& p) {
    return std::isfinite(p.graze) && std::isfinite(p.lookUp) && std::isfinite(p.headYaw) && std::isfinite(p.ears) &&
           std::isfinite(p.scratch) && std::isfinite(p.sitUp) && std::isfinite(p.charge);
}

// Нижняя точка ноги над землёй: стоящий не проваливается и не висит.
float footHeight(const lw::HumanPose& p, const lw::LegPose& leg) {
    return lw::pelvisHeight(p) -
           (lw::kThigh * std::cos(leg.hip) + lw::kShin * std::cos(leg.hip - leg.knee)) * std::cos(leg.spread);
}

void testPoseMath() {
    const lw::HumanPose standing = lw::pose::stand(0.0f, 1);
    CHECK(std::abs(lw::pelvisHeight(standing) - lw::kHipHeight) < 0.02f);
    const lw::HumanPose lying = lw::pose::sleepBack(0.0f);
    CHECK(std::abs(lw::pelvisHeight(lying) - lw::kLyingPelvis) < 1e-3f);
    CHECK(lw::lyingness(lying) > 0.99f && lw::lyingness(standing) < 0.01f);
    const lw::HumanPose stump = lw::pose::sitStump(0.0f, 1);
    CHECK(std::abs(lw::pelvisHeight(stump) - stump.seat) < 1e-3f);
    // Смесь: концы — исходные позы, середина — между ними.
    const lw::HumanPose half = lw::blend(standing, lying, 0.5f);
    CHECK(std::abs(lw::blend(standing, lying, 0.0f).bodyPitch - standing.bodyPitch) < 1e-6f);
    CHECK(std::abs(lw::blend(standing, lying, 1.0f).bodyPitch - lying.bodyPitch) < 1e-6f);
    CHECK(half.bodyPitch > standing.bodyPitch && half.bodyPitch < lying.bodyPitch);
}

void testPoseLibrary() {
    using namespace lw::pose;
    for (int i = 0; i < 40; ++i) {
        const float t = static_cast<float>(i) * 0.37f;
        const lw::Gait gait{t * 2.0f, 1.0f};
        const lw::HumanPose poses[] = {
            stand(t, 3), lookAround(t, 3, 0.7f), stretch(t), armsCrossed(t, 3), handsOnHips(t, 3), scratchHead(t),
            walk(gait, 3), run(gait), sneak(gait), patrol(gait), limp(gait), backAway(gait), pocketsWalk(gait),
            sitGround(t, 3), sitKnees(t, 3), sitStump(t, 3), lieBack(t, 3), leanWall(t, 3), eat(t, false),
            eat(t, true), drinkBottle(t), scoopWater(t), sleepBack(t), sleepCurled(t), dig(t), reap(t),
            counter(t, 3), examine(t), clipboard(t), lecture(t, 3), patrolStand(t, 3), rummage(t), reachHigh(t),
            pick(t, false), pick(t, true), butcher(t), talk(t, 3, 0.5f), nod(t), laugh(t), listen(t, 3),
            argue(t, 3), hug(t), offer(t), guard(t, 0), guard(t, 2), guard(t, 4), aim(t, 3), aim(t, 4),
            reload(t, 4), attack(guard(t, 0), t * 0.1f, 0), attack(guard(t, 2), t * 0.1f, 2),
            attack(aim(t, 4), t * 0.1f, 4), bandage(t), holdWound(t), kneelGrief(t, true), standGrief(t), cower(t),
            corpse(static_cast<std::uint32_t>(i)), twitch(corpse(5), t, 1.0f), zombieWalk(gait, i % 4, i % 2 == 0),
            zombieStand(t, i % 4), zombieFeed(t), zombieBang(t), zombieReach(t, i % 4),
            zombieAttack(zombieStand(t, 1), t * 0.1f), zombieRise(static_cast<float>(i) / 39.0f, 7, i % 4),
        };
        for (const lw::HumanPose& p : poses) {
            CHECK(finite(p));
        }
        // Стоящие и идущие стоят на земле: нижняя нога — у земли.
        for (const lw::HumanPose& p : {stand(t, 3), walk(gait, 3), run(gait), dig(t), guard(t, 0), rummage(t),
                                       scoopWater(t), kneelGrief(t, false), zombieWalk(gait, 2, false)}) {
            const float low = std::min(footHeight(p, p.leftLeg), footHeight(p, p.rightLeg));
            CHECK(low > -0.08f && low < 0.08f);
        }
        const lw::AnimalPose animals[] = {graze(t, 4), listenAnimal(t, 0.8f), scratchAnimal(t), sitUpHare(t)};
        for (const lw::AnimalPose& p : animals) {
            CHECK(finite(p));
        }
    }
}

lw::WorldConfig smallConfig() {
    lw::WorldConfig config;
    config.mapWidth = 512;
    config.mapHeight = 512;
    config.people = 80;
    config.zombies = 40;
    config.animals = 30;
    config.flocks = 2;
    config.peacetime = false;
    return config;
}

struct Frame {
    std::vector<lw::Figure> figures;
};

// Кадр: всех существ и тела — через аниматор.
Frame animateAll(lw::Animator& animator, const lw::World& world, float alpha) {
    Frame frame;
    animator.beginFrame(world, 1.0f / 60.0f, alpha);
    const entt::registry& registry = world.registry();
    for (const auto [entity, position, id] : registry.view<const lw::Position, const lw::Id>().each()) {
        if (registry.any_of<lw::Flock, lw::Cache>(entity)) {
            continue;
        }
        lw::Velocity velocity;
        if (const auto* v = registry.try_get<lw::Velocity>(entity)) {
            velocity = *v;
        }
        frame.figures.push_back(
            animator.animate(world, entity, id.value, position.x, position.y, velocity.x, velocity.y));
    }
    return frame;
}

// Мир живёт; аниматор только смотрит: хеш не меняется, позы конечны, сдвиги
// малы, два аниматора на одном входе видят одно и то же.
void testWorldAnimation() {
    lw::World world(11, smallConfig());
    for (int i = 0; i < 2000; ++i) {
        world.step();
    }
    lw::Animator a;
    lw::Animator b;
    for (int frame = 0; frame < 240; ++frame) {
        const std::uint64_t before = world.stateHash();
        const Frame fa = animateAll(a, world, 0.5f);
        const Frame fb = animateAll(b, world, 0.5f);
        CHECK(world.stateHash() == before);
        CHECK(fa.figures.size() == fb.figures.size());
        for (std::size_t i = 0; i < fa.figures.size() && i < fb.figures.size(); ++i) {
            const lw::Figure& f = fa.figures[i];
            CHECK(finite(f.human) && finite(f.animal) && std::isfinite(f.heading));
            CHECK(std::hypot(f.offsetX, f.offsetY) < 2.25f);
            CHECK(f.heading == fb.figures[i].heading);
            CHECK(f.human.lean == fb.figures[i].human.lean && f.human.leftArm.raise == fb.figures[i].human.leftArm.raise);
        }
        world.step();
    }
}

// Смерть: тело падает и через секунду лежит; событие летописи — значок над головой.
void testFallAndEmote() {
    lw::World world(12, smallConfig());
    for (int i = 0; i < 200; ++i) {
        world.step();
    }
    entt::registry& registry = world.registry();
    entt::entity victim = entt::null;
    entt::entity friendly = entt::null;
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        if (person.indoors || registry.any_of<lw::Zombie, lw::Corpse>(entity)) {
            continue;
        }
        if (victim == entt::null) {
            victim = entity;
        } else if (friendly == entt::null) {
            friendly = entity;
        }
    }
    CHECK(victim != entt::null && friendly != entt::null);
    if (victim == entt::null || friendly == entt::null) {
        return;
    }
    const lw::EntityId victimId = registry.get<lw::Id>(victim).value;
    const lw::EntityId friendId = registry.get<lw::Id>(friendly).value;
    lw::Animator animator;
    const auto show = [&](entt::entity entity, lw::EntityId id) {
        const auto& p = registry.get<lw::Position>(entity);
        return animator.animate(world, entity, id, p.x, p.y, 0.0f, 0.0f);
    };
    animator.beginFrame(world, 1.0f / 60.0f, 0.0f);
    CHECK(show(victim, victimId).body == lw::Figure::Body::Human);
    CHECK(lw::lyingness(show(victim, victimId).human) < 0.5f || registry.get<lw::Person>(victim).asleep);

    // Подружился — над головой сердце (близкие погибшего потом горюют — у них свой значок).
    world.record(lw::ChronicleKind::Befriended, friendId, victimId);
    for (int frame = 0; frame < 30; ++frame) {
        world.step();
        animator.beginFrame(world, 1.0f / 60.0f, 0.0f);
        CHECK(show(friendly, friendId).emote == lw::Emote::Heart);
    }

    lw::die(world, victim, lw::DeathCause::Wounds, lw::kInvalidId);
    for (int frame = 0; frame < 90; ++frame) {
        world.step();
        animator.beginFrame(world, 1.0f / 60.0f, 0.0f);
        const lw::Figure& body = show(victim, victimId);
        CHECK(body.body == lw::Figure::Body::HumanCorpse);
        CHECK(body.emote == lw::Emote::None);
        if (frame == 2) {
            CHECK(lw::lyingness(body.human) < 0.9f); // ещё падает
        }
        if (frame == 89) {
            CHECK(lw::lyingness(body.human) > 0.99f);
        }
    }
}

} // namespace

int main() {
    testPoseMath();
    testPoseLibrary();
    testWorldAnimation();
    testFallAndEmote();
    if (failures == 0) {
        std::printf("animation tests passed\n");
    }
    return failures;
}
