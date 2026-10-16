#include "sim/systems/MemorySystem.h"

#include "sim/Components.h"
#include "sim/Memory.h"
#include "sim/Perception.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Тело близкого замечают не дальше этого, метры, если его не закрывают стены.
constexpr float kBodyNoticeRange = 30.0f;

} // namespace

void updateMemories(World& world, float minutes) {
    entt::registry& registry = world.registry();
    const std::uint64_t tick = world.tick();
    // Что человек заметил за эту минуту.
    struct Finding {
        MemoryKind kind;
        EntityId who;
        EntityId about;
    };
    std::vector<Finding> findings;
    // Порядок — порядок хранения в реестре: он детерминирован. experience
    // компоненты не добавляет и не удаляет.
    auto view = registry.view<const Person, Memories, const Position>();
    for (const auto [entity, person, memories, position] : view.each()) {
        fadeMemories(memories, minutes);
        const auto* relations = registry.try_get<Relations>(entity);
        if (person.asleep || person.indoors || relations == nullptr || relations->count == 0) {
            continue;
        }
        const auto close = [&](EntityId other) {
            const Bond* bond = relations->find(other);
            return bond != nullptr && closeness(*bond) >= kCloseEnough;
        };
        findings.clear();
        world.spatial().forEachInRadius(
            position.x, position.y, kBodyNoticeRange, [&](const SpatialHit& hit) {
                const auto* corpse = registry.try_get<Corpse>(hit.entity);
                if (corpse == nullptr || corpse->kind != CorpseKind::Human || !close(hit.id) ||
                    memories.find(MemoryKind::Bereaved, hit.id) != nullptr) {
                    return;
                }
                if (sightCost(world.map(), world.nav(), position.x, position.y, hit.x, hit.y,
                              kBodyNoticeRange) <= kBodyNoticeRange) {
                    findings.push_back({MemoryKind::Bereaved, hit.id, corpse->killer});
                }
            });
        if (const auto* awareness = registry.try_get<Awareness>(entity)) {
            for (std::size_t i = 0; i < awareness->count; ++i) {
                const Contact& contact = awareness->contacts[i];
                if (!contact.seen || !isFresh(contact, tick) || !close(contact.id) ||
                    memories.find(MemoryKind::Turned, contact.id) != nullptr) {
                    continue;
                }
                const entt::entity other = world.find(contact.id);
                if (other != entt::null && registry.all_of<Zombie>(other)) {
                    findings.push_back({MemoryKind::Turned, contact.id, kInvalidId});
                }
            }
        }
        // Порядок обхода индекса — не порядок мира: по виду и id.
        std::sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
            return std::pair(a.kind, a.who) < std::pair(b.kind, b.who);
        });
        for (const Finding& finding : findings) {
            experience(world, entity, finding.kind, finding.who, finding.about,
                       finding.kind == MemoryKind::Bereaved ? 1 : 0);
        }
    }
}

void tickMemories(World& world) {
    if (world.tick() % kTicksPerGameMinute == 0) {
        updateMemories(world, 1.0f);
    }
}

} // namespace lw
