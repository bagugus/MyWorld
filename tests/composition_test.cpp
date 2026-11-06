// Тесты ракурсов и композиции (этап 12а.12) — без графики: смысл сцены из мира
// (ui/Composition: readScene), правило 180° (ScreenMemory), правило третей,
// цвет по настроению. Чтение сцены только смотрит на мир и не меняет его хеш.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "sim/Components.h"
#include "sim/World.h"
#include "sim/systems/ZombieSystem.h"
#include "ui/Composition.h"

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

lw::WorldConfig quietConfig() {
    lw::WorldConfig config;
    config.mapWidth = 512;
    config.mapHeight = 512;
    config.people = 60;
    config.zombies = 0;
    config.animals = 10;
    config.flocks = 1;
    config.peacetime = false;
    return config;
}

// Двое на улице, живые; их entt и id.
struct Pair {
    entt::entity a = entt::null;
    entt::entity b = entt::null;
    lw::EntityId aId = lw::kInvalidId;
    lw::EntityId bId = lw::kInvalidId;
};

Pair outdoorPair(lw::World& world) {
    Pair pair;
    entt::registry& registry = world.registry();
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        if (person.indoors || person.asleep || registry.any_of<lw::Zombie, lw::Corpse>(entity)) {
            continue;
        }
        if (pair.a == entt::null) {
            pair.a = entity;
        } else if (pair.b == entt::null) {
            pair.b = entity;
            break;
        }
    }
    if (pair.a != entt::null && pair.b != entt::null) {
        pair.aId = registry.get<lw::Id>(pair.a).value;
        pair.bId = registry.get<lw::Id>(pair.b).value;
    }
    return pair;
}

// Успокоить человека: ничего не делает, никого не замечает, здоров.
void calm(lw::World& world, entt::entity entity) {
    entt::registry& registry = world.registry();
    registry.get<lw::Mind>(entity).action = lw::Action::Idle;
    registry.get<lw::Mind>(entity).target = lw::kInvalidId;
    if (auto* awareness = registry.try_get<lw::Awareness>(entity)) {
        awareness->count = 0;
    }
    if (auto* needs = registry.try_get<lw::Needs>(entity)) {
        needs->fear = 0.0f;
        needs->health = 1.0f;
    }
    if (auto* combat = registry.try_get<lw::Combat>(entity)) {
        combat->target = lw::kInvalidId;
    }
}

void testReadScene() {
    lw::World world(21, quietConfig());
    for (int i = 0; i < 400; ++i) {
        world.step();
    }
    Pair pair = outdoorPair(world);
    CHECK(pair.a != entt::null && pair.b != entt::null);
    if (pair.b == entt::null) {
        return;
    }
    entt::registry& registry = world.registry();
    calm(world, pair.a);
    calm(world, pair.b);

    // Нет такого — быт.
    CHECK(lw::ui::readScene(world, 999999999u).sense == lw::ui::SceneSense::Daily);

    // Только читает: хеш мира прежний.
    const std::uint64_t before = world.stateHash();
    (void)lw::ui::readScene(world, pair.aId);
    CHECK(world.stateHash() == before);

    // Разговор: подошёл к соседу и говорит.
    auto& a = registry.get<lw::Position>(pair.a);
    auto& b = registry.get<lw::Position>(pair.b);
    b.x = a.x + 2.0f;
    b.y = a.y;
    lw::Mind& mind = registry.get<lw::Mind>(pair.a);
    mind.action = lw::Action::Socialize;
    mind.phase = lw::ActionPhase::Perform;
    mind.target = pair.bId;
    const lw::ui::SceneReading talk = lw::ui::readScene(world, pair.aId);
    CHECK(talk.sense == lw::ui::SceneSense::Talk);
    CHECK(talk.other == pair.bId);
    CHECK(std::abs(talk.otherX - b.x) < 1e-3f);
    // Собеседник далеко — уже не разговор.
    b.x = a.x + 40.0f;
    CHECK(lw::ui::readScene(world, pair.aId).sense == lw::ui::SceneSense::Daily);
    calm(world, pair.a);

    // Горе: весть о гибели близкого — только что.
    world.record(lw::ChronicleKind::Mourned, pair.aId, pair.bId);
    CHECK(lw::ui::readScene(world, pair.aId).sense == lw::ui::SceneSense::Grief);

    // Беспомощность: тяжело ранен.
    registry.get<lw::Needs>(pair.b).health = 0.2f;
    CHECK(lw::ui::readScene(world, pair.bId).sense == lw::ui::SceneSense::Helpless);
    registry.get<lw::Needs>(pair.b).health = 1.0f;

    // Засада: зомби в нескольких метрах, а герой его не замечает.
    const lw::Cell cell{static_cast<int>(std::floor((b.x + 6.0f) / lw::Map::kCellSize)),
                        static_cast<int>(std::floor(b.y / lw::Map::kCellSize))};
    const entt::entity zombie = lw::createZombie(world, cell, world.rng(lw::RngStream::Zombies));
    const lw::EntityId zombieId = registry.get<lw::Id>(zombie).value;
    world.step(); // зомби — в пространственном индексе
    calm(world, pair.b);
    const lw::ui::SceneReading ambush = lw::ui::readScene(world, pair.bId);
    CHECK(ambush.sense == lw::ui::SceneSense::Ambush);
    CHECK(ambush.other == zombieId);
    // Сам зомби — угроза: нижний ракурс.
    const lw::ui::SceneReading menace = lw::ui::readScene(world, zombieId);
    CHECK(menace.sense == lw::ui::SceneSense::Threat && menace.menacing);
    // Заметил его — угроза видна (или уже бежит).
    world.registry().get<lw::Awareness>(pair.b).contacts[0] = {zombieId, lw::Map::cellCenter(cell.x), lw::Map::cellCenter(cell.y), world.tick(), true};
    world.registry().get<lw::Awareness>(pair.b).count = 1;
    const lw::ui::SceneSense seen = lw::ui::readScene(world, pair.bId).sense;
    CHECK(seen == lw::ui::SceneSense::Threat || seen == lw::ui::SceneSense::Chase);
    registry.get<lw::Mind>(pair.b).action = lw::Action::Flee;
    const lw::ui::SceneReading chase = lw::ui::readScene(world, pair.bId);
    CHECK(chase.sense == lw::ui::SceneSense::Chase && chase.panic);
}

void testScreenMemory() {
    lw::ui::ScreenMemory memory;
    // Пара встретилась впервые — сторона подсказанная; потом — та же, с какой
    // стороны ни смотри и что ни подсказывай.
    CHECK(memory.sideOf(5, 9, 1.0f) == 1.0f);
    CHECK(memory.sideOf(9, 5, 1.0f) == -1.0f);
    CHECK(memory.sideOf(5, 9, -1.0f) == 1.0f);
    CHECK(memory.sideOf(3, 4, -1.0f) == -1.0f);
    CHECK(memory.sideOf(4, 3, -1.0f) == 1.0f);
    // Направление по экрану: слабое не запоминается.
    CHECK(memory.motionOf(7) == 0.0f);
    memory.rememberMotion(7, 0.1f);
    CHECK(memory.motionOf(7) == 0.0f);
    memory.rememberMotion(7, -0.8f);
    CHECK(memory.motionOf(7) == -1.0f);
    // Много пар — старые забываются, новые помнятся.
    for (lw::EntityId i = 100; i < 1200; ++i) {
        memory.sideOf(i, i + 1, (i % 2 == 0) ? 1.0f : -1.0f);
    }
    CHECK(memory.sideOf(1198, 1199, 1.0f) == 1.0f);
    CHECK(memory.sideOf(1199, 1200, 1.0f) == -1.0f);
    memory.clear();
    CHECK(memory.motionOf(7) == 0.0f);
}

void testThirds() {
    const float kPi = 3.14159265f;
    // Камера с юга (курс 0) смотрит на север: идущий на восток — вправо по экрану.
    CHECK(lw::ui::screenDirection(kPi * 0.5f, 0.0f) > 0.99f);
    CHECK(lw::ui::screenDirection(-kPi * 0.5f, 0.0f) < -0.99f);
    CHECK(std::abs(lw::ui::screenDirection(0.0f, 0.0f)) < 1e-4f);
    // Камера с востока (курс 90): идущий на север — вправо.
    CHECK(lw::ui::screenDirection(kPi, 90.0f) > 0.99f);
    // Смотрит вправо — стоит на левой трети, свободное место — справа.
    CHECK(std::abs(lw::ui::thirdsOffset(0.9f, 0.0f) + 1.0f / 6.0f) < 1e-5f);
    CHECK(std::abs(lw::ui::thirdsOffset(-0.9f, 0.0f) - 1.0f / 6.0f) < 1e-5f);
    // Смотрит на камеру — остаётся, где стоял.
    CHECK(lw::ui::thirdsOffset(0.1f, 0.12f) == 0.12f);
}

void testGrade() {
    const lw::ui::ColorGrade daily = lw::ui::gradeOf(lw::ui::SceneSense::Daily, false, 0.0f);
    CHECK(daily.desaturate == 0.0f && daily.tintR == 0.0f && daily.tintG == 0.0f && daily.tintB == 0.0f);
    const lw::ui::ColorGrade grief = lw::ui::gradeOf(lw::ui::SceneSense::Grief, false, 0.0f);
    CHECK(grief.desaturate > daily.desaturate && grief.vignette > daily.vignette);
    const lw::ui::ColorGrade family = lw::ui::gradeOf(lw::ui::SceneSense::Family, false, 0.0f);
    CHECK(family.tintR > 0.0f && family.tintB < 0.0f); // тепло
    const lw::ui::ColorGrade threat = lw::ui::gradeOf(lw::ui::SceneSense::Threat, true, 1.0f);
    CHECK(threat.vignette <= 0.65f && threat.tintB > 0.0f);
    for (int i = 0; i < static_cast<int>(lw::ui::SceneSense::Count); ++i) {
        const lw::ui::ColorGrade g = lw::ui::gradeOf(static_cast<lw::ui::SceneSense>(i), i % 2 == 0, 0.5f);
        CHECK(g.desaturate >= 0.0f && g.desaturate <= 0.6f && g.vignette >= 0.0f && g.vignette <= 0.65f);
        CHECK(std::abs(g.tintR) < 0.1f && std::abs(g.tintG) < 0.1f && std::abs(g.tintB) < 0.1f);
    }
    const lw::ui::ColorGrade start = lw::ui::mixGrade(daily, grief, 0.0f);
    const lw::ui::ColorGrade end = lw::ui::mixGrade(daily, grief, 1.0f);
    CHECK(start.desaturate == daily.desaturate && std::abs(end.desaturate - grief.desaturate) < 1e-6f);
}

} // namespace

int main() {
    testReadScene();
    testScreenMemory();
    testThirds();
    testGrade();
    if (failures == 0) {
        std::printf("composition tests passed\n");
    }
    return failures;
}
