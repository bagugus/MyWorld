#include "ui/Composition.h"

#include "sim/Components.h"
#include "sim/Scenario.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "sim/systems/ZombieSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lw::ui {

namespace {

constexpr float kDeg = 3.14159265358979f / 180.0f;
// Горе — весть о гибели близкого не старше стольких игровых минут.
constexpr std::uint64_t kGriefTicks = 60 * kTicksPerGameMinute;
// Засада: угроза не дальше стольких метров, а герой её не замечает.
constexpr float kAmbushRadius = 32.0f;
// Мститель ищет обидчика: засада — дальше, он подкрадывается.
constexpr float kStalkRadius = 45.0f;
// Угроза в кадре — не дальше; противник в драке — тоже.
constexpr float kThreatRadius = 45.0f;
constexpr float kFightRadius = 30.0f;
// Разговор — двое не дальше стольких метров.
constexpr float kTalkRadius = 5.0f;
// Идёт — быстрее стольких метров в секунду.
constexpr float kMovingSpeed = 0.4f;
// Беспомощность: здоровье ниже, лихорадка (заражение) выше.
constexpr float kHelplessHealth = 0.35f;
constexpr float kFeverInfection = 0.5f;
// Правило третей: третья линия — на шестую часть ширины от середины.
constexpr float kThird = 1.0f / 6.0f;
// Взгляд по экрану слабее — кадр не перестраивается.
constexpr float kLookDeadZone = 0.35f;
// Сколько пар и героев помнить.
constexpr std::size_t kMemoryLimit = 256;

float distance(float ax, float ay, float bx, float by) {
    return std::hypot(ax - bx, ay - by);
}

std::uint64_t pairKey(EntityId lo, EntityId hi) {
    return lo * 0x9E3779B97F4A7C15ull ^ (hi + 0x632BE59BD9B4E019ull + (lo << 6) + (lo >> 2));
}

bool socialAction(Action action) {
    return action == Action::Socialize || action == Action::Trade || action == Action::Share;
}

} // namespace

const char* sceneSenseTitle(SceneSense sense) {
    switch (sense) {
    case SceneSense::Daily:
        return "быт";
    case SceneSense::Talk:
        return "разговор";
    case SceneSense::Chase:
        return "погоня";
    case SceneSense::Fight:
        return "драка";
    case SceneSense::Ambush:
        return "засада";
    case SceneSense::Threat:
        return "угроза";
    case SceneSense::Grief:
        return "горе";
    case SceneSense::Helpless:
        return "беспомощность";
    case SceneSense::Family:
        return "семья";
    case SceneSense::Count:
        break;
    }
    return "?";
}

SceneReading readScene(const World& world, EntityId hero) {
    SceneReading reading;
    const entt::registry& registry = world.registry();
    const entt::entity entity = world.find(hero);
    if (entity == entt::null || !registry.all_of<Position>(entity) || registry.all_of<Corpse>(entity)) {
        return reading;
    }
    const Position& at = registry.get<Position>(entity);
    if (const auto* velocity = registry.try_get<Velocity>(entity)) {
        const float speed = std::hypot(velocity->x, velocity->y); // м/с
        if (speed > kMovingSpeed) {
            reading.moving = true;
            reading.moveX = velocity->x / speed;
            reading.moveY = velocity->y / speed;
        }
    }
    const auto place = [&](EntityId id) {
        const entt::entity other = world.find(id);
        if (other == entt::null || !registry.all_of<Position>(other)) {
            return false;
        }
        const Position& p = registry.get<Position>(other);
        reading.other = id;
        reading.otherX = p.x;
        reading.otherY = p.y;
        return true;
    };
    const auto near = [&](EntityId id, float radius) {
        const entt::entity other = world.find(id);
        if (other == entt::null || !registry.all_of<Position>(other) || registry.all_of<Corpse>(other)) {
            return false;
        }
        const Position& p = registry.get<Position>(other);
        return distance(p.x, p.y, at.x, at.y) <= radius;
    };

    // Зомби — сам угроза: нижний ракурс; гонится — за кем.
    if (const auto* zombie = registry.try_get<Zombie>(entity)) {
        reading.sense = SceneSense::Threat;
        reading.menacing = true;
        if (zombie->state == ZombieState::Chase && near(zombie->target, kThreatRadius)) {
            place(zombie->target);
        }
        return reading;
    }
    if (!registry.all_of<Person>(entity)) {
        return reading; // звери, птицы — быт
    }
    const Person& person = registry.get<Person>(entity);
    const auto* mind = registry.try_get<Mind>(entity);
    const auto* needs = registry.try_get<Needs>(entity);
    const auto* wounds = registry.try_get<Wounds>(entity);
    const float fear = needs != nullptr ? needs->fear : 0.0f;
    reading.panic = fear > 0.7f || (worldPhase(world) == WorldPhase::Panic && fear > 0.45f);
    const Action action = mind != nullptr ? mind->action : Action::Idle;

    // Драка: бьёт или стреляет, противник рядом.
    if (action == Action::Fight && mind != nullptr && near(mind->target, kFightRadius)) {
        reading.sense = SceneSense::Fight;
        place(mind->target);
        return reading;
    }
    if (const auto* combat = registry.try_get<Combat>(entity); combat != nullptr && near(combat->target, kFightRadius)) {
        reading.sense = SceneSense::Fight;
        place(combat->target);
        return reading;
    }
    const Threat threat = threatOf(world, entity);
    // Погоня: бежит, угроза за спиной.
    if (action == Action::Flee) {
        reading.sense = SceneSense::Chase;
        reading.panic = true;
        if (threat.nearest != kInvalidId && near(threat.nearest, kThreatRadius)) {
            place(threat.nearest);
        }
        return reading;
    }
    // Мститель у цели — сам угроза.
    if (action == Action::Avenge && mind != nullptr && near(mind->target, kThreatRadius)) {
        reading.sense = SceneSense::Threat;
        reading.menacing = true;
        place(mind->target);
        return reading;
    }
    // Засада: зомби рядом или мститель подкрадывается, а герой не замечает.
    if (!person.indoors) {
        const auto* awareness = registry.try_get<Awareness>(entity);
        const auto unseen = [&](EntityId id) {
            const Contact* contact = awareness != nullptr ? awareness->find(id) : nullptr;
            return contact == nullptr;
        };
        float best = 1e9f;
        EntityId lurker = kInvalidId;
        world.spatial().forEachInRadius(at.x, at.y, kStalkRadius, [&](const SpatialHit& hit) {
            if (hit.id == hero) {
                return;
            }
            const float d = std::sqrt(hit.distanceSq);
            bool stalks = false;
            if (registry.all_of<Zombie>(hit.entity) && !registry.all_of<Corpse>(hit.entity)) {
                stalks = d <= kAmbushRadius;
            } else if (const auto* other = registry.try_get<Mind>(hit.entity);
                       other != nullptr && other->action == Action::Avenge && other->target == hero &&
                       !registry.all_of<Corpse>(hit.entity)) {
                stalks = true;
            }
            if (stalks && unseen(hit.id) && (d < best || (d == best && hit.id < lurker))) {
                best = d;
                lurker = hit.id;
            }
        });
        if (lurker != kInvalidId) {
            reading.sense = SceneSense::Ambush;
            place(lurker);
            return reading;
        }
    }
    // Угроза видна.
    if (threat.level > 0.25f && threat.nearest != kInvalidId && near(threat.nearest, kThreatRadius)) {
        reading.sense = SceneSense::Threat;
        place(threat.nearest);
        return reading;
    }
    // Горе: только что узнал о гибели близкого.
    const Chronicle& chronicle = world.chronicle();
    const auto own = chronicle.eventsOf(hero);
    for (auto it = own.rbegin(); it != own.rend(); ++it) {
        const ChronicleEvent& event = chronicle.events()[*it];
        if (event.tick + kGriefTicks < world.tick()) {
            break;
        }
        if ((event.kind == ChronicleKind::Mourned || event.kind == ChronicleKind::SawTurned) &&
            event.subject == hero) {
            reading.sense = SceneSense::Grief;
            return reading;
        }
    }
    // Беспомощность: тяжело ранен или в лихорадке.
    const float health = needs != nullptr ? needs->health : 1.0f;
    if (health < kHelplessHealth || (wounds != nullptr && wounds->infection > kFeverInfection)) {
        reading.sense = SceneSense::Helpless;
        return reading;
    }
    // Разговор: сам подошёл к кому-то или подошли к нему.
    if (mind != nullptr && socialAction(action) && mind->phase == ActionPhase::Perform &&
        near(mind->target, kTalkRadius)) {
        reading.sense = SceneSense::Talk;
        place(mind->target);
        return reading;
    }
    if (!person.indoors) {
        EntityId speaker = kInvalidId;
        world.spatial().forEachInRadius(at.x, at.y, kTalkRadius, [&](const SpatialHit& hit) {
            const auto* other = registry.try_get<Mind>(hit.entity);
            if (hit.id != hero && other != nullptr && socialAction(other->action) && other->target == hero &&
                (speaker == kInvalidId || hit.id < speaker)) {
                speaker = hit.id;
            }
        });
        if (speaker != kInvalidId) {
            reading.sense = SceneSense::Talk;
            place(speaker);
            return reading;
        }
    }
    // Дома с домашними, кто не спит.
    if (person.indoors && person.home != kInvalidId) {
        EntityId mate = kInvalidId;
        for (const auto [other, p] : registry.view<const Person>().each()) {
            if (other == entity || !p.indoors || p.asleep || p.home != person.home ||
                registry.any_of<Corpse, Zombie>(other)) {
                continue;
            }
            const EntityId id = registry.get<Id>(other).value;
            if (mate == kInvalidId || id < mate) {
                mate = id;
            }
        }
        if (mate != kInvalidId && !person.asleep) {
            reading.sense = SceneSense::Family;
            place(mate);
            return reading;
        }
    }
    return reading;
}

float ScreenMemory::sideOf(EntityId a, EntityId b, float suggested) {
    const bool flip = b < a;
    const EntityId lo = flip ? b : a;
    const EntityId hi = flip ? a : b;
    const std::uint64_t key = pairKey(lo, hi);
    auto found = pairs_.find(key);
    if (found == pairs_.end()) {
        trim(pairs_);
        found = pairs_.emplace(key, Entry{flip ? -suggested : suggested, 0}).first;
    }
    found->second.used = ++clock_;
    const float side = found->second.value >= 0.0f ? 1.0f : -1.0f;
    return flip ? -side : side;
}

float ScreenMemory::motionOf(EntityId hero) const {
    const auto found = motion_.find(hero);
    return found == motion_.end() ? 0.0f : found->second.value;
}

void ScreenMemory::rememberMotion(EntityId hero, float screenDirection) {
    if (std::abs(screenDirection) < 0.3f) {
        return; // идёт на камеру или от неё — направление по экрану не задано
    }
    trim(motion_);
    Entry& entry = motion_[hero];
    entry.value = screenDirection > 0.0f ? 1.0f : -1.0f;
    entry.used = ++clock_;
}

void ScreenMemory::clear() {
    pairs_.clear();
    motion_.clear();
    clock_ = 0;
}

void ScreenMemory::trim(std::unordered_map<std::uint64_t, Entry>& table) {
    if (table.size() < kMemoryLimit) {
        return;
    }
    // Забыть самую старую половину.
    std::vector<std::uint64_t> used;
    used.reserve(table.size());
    for (const auto& [key, entry] : table) {
        used.push_back(entry.used);
    }
    std::nth_element(used.begin(), used.begin() + static_cast<std::ptrdiff_t>(used.size() / 2), used.end());
    const std::uint64_t median = used[used.size() / 2];
    for (auto it = table.begin(); it != table.end();) {
        it = it->second.used < median ? table.erase(it) : std::next(it);
    }
}

float screenDirection(float heading, float cameraYaw) {
    return std::sin(heading - cameraYaw * kDeg);
}

float thirdsOffset(float look, float previous) {
    if (std::abs(look) < kLookDeadZone) {
        return previous; // смотрит на камеру или от неё — остаётся, где стоял
    }
    return look > 0.0f ? -kThird : kThird; // свободное место — перед взглядом
}

ColorGrade gradeOf(SceneSense sense, bool panic, float night) {
    ColorGrade grade;
    switch (sense) {
    case SceneSense::Daily:
        grade.vignette = 0.18f;
        break;
    case SceneSense::Talk:
        grade.tintR = 0.02f;
        grade.tintB = -0.015f;
        grade.vignette = 0.22f;
        break;
    case SceneSense::Family:
        // Тёплый свет дома.
        grade.tintR = 0.05f;
        grade.tintG = 0.02f;
        grade.tintB = -0.04f;
        grade.vignette = 0.28f;
        break;
    case SceneSense::Chase:
        grade.contrast = 0.12f;
        grade.desaturate = 0.1f;
        grade.vignette = 0.38f;
        break;
    case SceneSense::Fight:
        grade.contrast = 0.14f;
        grade.desaturate = 0.12f;
        grade.tintR = 0.02f;
        grade.vignette = 0.34f;
        break;
    case SceneSense::Ambush:
        // Холодно и тесно: зритель знает больше героя.
        grade.tintR = -0.03f;
        grade.tintG = 0.01f;
        grade.tintB = 0.03f;
        grade.desaturate = 0.2f;
        grade.contrast = 0.08f;
        grade.vignette = 0.5f;
        break;
    case SceneSense::Threat:
        grade.tintR = -0.02f;
        grade.tintB = 0.025f;
        grade.desaturate = 0.15f;
        grade.contrast = 0.1f;
        grade.vignette = 0.42f;
        break;
    case SceneSense::Grief:
        // Выцветший холодный кадр.
        grade.tintR = -0.02f;
        grade.tintB = 0.03f;
        grade.desaturate = 0.45f;
        grade.vignette = 0.45f;
        break;
    case SceneSense::Helpless:
        grade.desaturate = 0.3f;
        grade.tintG = -0.01f;
        grade.vignette = 0.45f;
        break;
    case SceneSense::Count:
        break;
    }
    if (panic) {
        grade.contrast += 0.06f;
        grade.vignette += 0.08f;
    }
    // Ночью — чуть холоднее и темнее по углам.
    const float n = std::clamp(night, 0.0f, 1.0f);
    grade.tintB += 0.02f * n;
    grade.tintR -= 0.01f * n;
    grade.vignette += 0.1f * n;
    grade.vignette = std::min(grade.vignette, 0.65f);
    return grade;
}

ColorGrade mixGrade(const ColorGrade& a, const ColorGrade& b, float t) {
    const auto lerp = [t](float x, float y) { return x + (y - x) * t; };
    ColorGrade out;
    out.tintR = lerp(a.tintR, b.tintR);
    out.tintG = lerp(a.tintG, b.tintG);
    out.tintB = lerp(a.tintB, b.tintB);
    out.desaturate = lerp(a.desaturate, b.desaturate);
    out.contrast = lerp(a.contrast, b.contrast);
    out.vignette = lerp(a.vignette, b.vignette);
    return out;
}

} // namespace lw::ui
