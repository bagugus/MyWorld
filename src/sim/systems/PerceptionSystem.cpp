#include "sim/systems/PerceptionSystem.h"

#include "sim/Components.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace lw {

namespace {

// Сосед, которого можно заметить: в пределах дальности зрения или слышен.
struct Candidate {
    SpatialHit hit;
    float distance = 0.0f;
    float reach = 0.0f; // дальность зрения до него без учёта укрытия, леса и стен
    SoundKind heard = SoundKind::None;
};

// Замеченный в этом обновлении.
struct Fresh {
    Contact contact;
    float distance = 0.0f;
};

void merge(Awareness& awareness, std::vector<Fresh>& fresh, std::uint64_t tick) {
    // Сначала замеченные сейчас, ближние вперёд; затем помнящиеся, недавние вперёд.
    std::sort(fresh.begin(), fresh.end(), [](const Fresh& a, const Fresh& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.contact.id < b.contact.id;
    });
    Contact old[Awareness::kMaxContacts];
    std::size_t oldCount = 0;
    for (std::size_t i = 0; i < awareness.count; ++i) {
        const Contact& contact = awareness.contacts[i];
        const bool refreshed = std::any_of(fresh.begin(), fresh.end(), [&](const Fresh& f) {
            return f.contact.id == contact.id;
        });
        if (!refreshed && contact.tick + kContactMemoryTicks >= tick) {
            old[oldCount++] = contact;
        }
    }
    std::sort(old, old + oldCount, [](const Contact& a, const Contact& b) {
        return a.tick != b.tick ? a.tick > b.tick : a.id < b.id;
    });

    std::size_t count = 0;
    for (const Fresh& f : fresh) {
        if (count == Awareness::kMaxContacts) {
            break;
        }
        awareness.contacts[count++] = f.contact;
    }
    for (std::size_t i = 0; i < oldCount && count < Awareness::kMaxContacts; ++i) {
        awareness.contacts[count++] = old[i];
    }
    for (std::size_t i = count; i < Awareness::kMaxContacts; ++i) {
        awareness.contacts[i] = {};
    }
    awareness.count = static_cast<std::uint8_t>(count);
}

// Восприятие в одном тике: всё, что общее для существ, берётся один раз.
class Perceiver {
public:
    Perceiver(World& world, const SenseConditions& conditions)
        : world_(world),
          lookup_(std::as_const(world.registry())),
          map_(world.map()),
          nav_(world.nav()),
          spatial_(world.spatial()),
          conditions_(conditions),
          tick_(world.tick()) {}

    void perceive(entt::entity entity, EntityId self, const Senses& senses,
                  const Position& position, Awareness& awareness) {
        const bool asleep = lookup_.asleep(entity);
        const float range = sightRange(senses, asleep, conditions_);
        const float hearing = hearingFactor(senses, asleep, conditions_);
        candidates_.clear();
        fresh_.clear();

        // Сначала дешёвые проверки: расстояние, заметность, шум.
        const float radius = std::max(range, kMaxNoiseLoudness * hearing);
        spatial_.forEachInRadius(position.x, position.y, radius, [&](const SpatialHit& hit) {
            if (hit.id == self || !lookup_.perceivable(hit.entity)) {
                return;
            }
            Candidate candidate;
            candidate.hit = hit;
            candidate.distance = std::sqrt(hit.distanceSq);
            if (range > 0.0f) {
                candidate.reach = range * bodyVisibility(lookup_, hit.entity);
            }
            const Noise noise = noiseOf(lookup_, hit.entity);
            if (noise.kind != SoundKind::None && candidate.distance <= noise.loudness * hearing) {
                candidate.heard = noise.kind;
            }
            if (candidate.heard != SoundKind::None || candidate.distance <= candidate.reach) {
                candidates_.push_back(candidate);
            }
        });
        // Луч зрения — самая дорогая проверка, а помнится только kMaxContacts
        // ближайших замеченных: лучи — от ближних к дальним, пока столько не
        // наберётся. Дальние всё равно не попали бы в Awareness.
        std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) {
            return a.distance != b.distance ? a.distance < b.distance : a.hit.id < b.hit.id;
        });
        for (const Candidate& candidate : candidates_) {
            if (fresh_.size() == Awareness::kMaxContacts) {
                break;
            }
            const SpatialHit& hit = candidate.hit;
            bool seen = false;
            // range > 0: спящий не видит даже того, кто стоит вплотную.
            if (range > 0.0f && candidate.distance <= candidate.reach) {
                const float reach = candidate.reach * coverVisibility(map_, hit.x, hit.y);
                seen = candidate.distance <= reach &&
                       sightCost(map_, nav_, position.x, position.y, hit.x, hit.y, reach) <= reach;
            }
            if (seen || candidate.heard != SoundKind::None) {
                fresh_.push_back(
                    {{hit.id, hit.x, hit.y, tick_, seen, candidate.heard}, candidate.distance});
            }
        }

        // Разовые звуки: услышанный источник — контакт там, где прозвучало, если
        // самого его не видно.
        for (const Sound& sound : world_.sounds()) {
            if (sound.source == self || sound.source == kInvalidId) {
                continue;
            }
            const float dx = sound.x - position.x;
            const float dy = sound.y - position.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > sound.loudness * hearing) {
                continue;
            }
            const auto known = std::find_if(fresh_.begin(), fresh_.end(), [&](const Fresh& f) {
                return f.contact.id == sound.source;
            });
            if (known == fresh_.end()) {
                fresh_.push_back({{sound.source, sound.x, sound.y, tick_, false, sound.kind}, distance});
            } else {
                known->contact.heard = sound.kind;
            }
        }

        merge(awareness, fresh_, tick_);
    }

private:
    World& world_;
    SenseLookup lookup_;
    const Map& map_;
    const NavGrid& nav_;
    const SpatialIndex& spatial_;
    SenseConditions conditions_;
    std::uint64_t tick_;
    std::vector<Candidate> candidates_;
    std::vector<Fresh> fresh_;
};

} // namespace

void perceive(World& world, entt::entity entity, const SenseConditions& conditions) {
    entt::registry& registry = world.registry();
    Perceiver(world, conditions)
        .perceive(entity, registry.get<Id>(entity).value, registry.get<Senses>(entity),
                  registry.get<Position>(entity), registry.get<Awareness>(entity));
}

void updatePerception(World& world) {
    const std::uint64_t tick = world.tick();
    world.forgetSounds(tick > kSoundLifetimeTicks ? tick - kSoundLifetimeTicks : 0);

    entt::registry& registry = world.registry();
    std::optional<Perceiver> perceiver;
    // Порядок — порядок хранения в реестре: каждое существо меняет только своё
    // Awareness, а читает снимок индекса, поэтому порядок на результат не влияет.
    for (const auto [entity, id, senses, position, awareness] :
         registry.view<const Id, const Senses, const Position, Awareness>().each()) {
        if ((tick + id.value) % kPerceptionIntervalTicks != 0) {
            continue;
        }
        if (!perceiver) {
            perceiver.emplace(world, senseConditions(world));
        }
        perceiver->perceive(entity, id.value, senses, position, awareness);
    }
}

} // namespace lw
