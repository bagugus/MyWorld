#include "sim/systems/PerceptionSystem.h"

#include "sim/Components.h"
#include "sim/Parallel.h"
#include "sim/World.h"

#include <algorithm>
#include <array>
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
    Interest interest = Interest::Ordinary;
};

// Замеченный в этом обновлении.
struct Fresh {
    Contact contact;
    float distance = 0.0f;
    Interest interest = Interest::Ordinary;
};

// Угрозы вперёд, затем ближние; при равных — меньший id.
template <typename T>
bool closerFirst(const T& a, float distanceA, EntityId idA, const T& b, float distanceB,
                 EntityId idB) {
    if (a.interest != b.interest) {
        return a.interest < b.interest;
    }
    return distanceA != distanceB ? distanceA < distanceB : idA < idB;
}

void merge(Awareness& awareness, std::vector<Fresh>& fresh, std::uint64_t tick) {
    // Сначала замеченные сейчас, угрозы и ближние вперёд; затем помнящиеся,
    // недавние вперёд.
    std::sort(fresh.begin(), fresh.end(), [](const Fresh& a, const Fresh& b) {
        return closerFirst(a, a.distance, a.contact.id, b, b.distance, b.contact.id);
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
        // Заметность не больше 1, шум не громче kMaxNoiseLoudness: дальше
        // дальности зрения соседа не увидеть, дальше kMaxNoiseLoudness × слух —
        // не услышать. Такие соседи отсеиваются по расстоянию, до поиска
        // компонентов (этап 2а.1: днём в круге зрения 150 м почти все соседи
        // дальше 35 м, и шум у них считать незачем). Результат тот же: и при
        // подсчёте он бы не прошёл сравнение с расстоянием.
        const float audible = kMaxNoiseLoudness * hearing;
        spatial_.forEachInRadius(position.x, position.y, radius, [&](const SpatialHit& hit) {
            if (hit.id == self) {
                return;
            }
            const float distance = std::sqrt(hit.distanceSq);
            const bool inSight = range > 0.0f && distance <= range;
            const bool inEarshot = distance <= audible;
            if ((!inSight && !inEarshot) || !lookup_.perceivable(hit.entity)) {
                return;
            }
            const Interest interest = interestOf(lookup_, entity, hit.entity);
            if (interest == Interest::Ignored) {
                return;
            }
            Candidate candidate;
            candidate.hit = hit;
            candidate.interest = interest;
            candidate.distance = distance;
            if (inSight) {
                candidate.reach = range * bodyVisibility(lookup_, hit.entity);
            }
            if (inEarshot) {
                const Noise noise = noiseOf(lookup_, hit.entity);
                if (noise.kind != SoundKind::None && candidate.distance <= noise.loudness * hearing) {
                    candidate.heard = noise.kind;
                }
            }
            if (candidate.heard != SoundKind::None ||
                (candidate.reach > 0.0f && candidate.distance <= candidate.reach)) {
                candidates_.push_back(candidate);
            }
        });
        // Луч зрения — самая дорогая проверка, а помнится только kMaxContacts
        // ближайших замеченных: лучи — от ближних к дальним, пока столько не
        // наберётся. Дальние всё равно не попали бы в Awareness.
        std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) {
            return closerFirst(a, a.distance, a.hit.id, b, b.distance, b.hit.id);
        });
        for (const Candidate& candidate : candidates_) {
            if (fresh_.size() == Awareness::kMaxContacts) {
                break;
            }
            const SpatialHit& hit = candidate.hit;
            bool seen = false;
            // range > 0: спящий не видит даже того, кто стоит вплотную; reach > 0:
            // того, кто дома, не видно даже вплотную.
            if (range > 0.0f && candidate.reach > 0.0f && candidate.distance <= candidate.reach) {
                const float reach = candidate.reach * coverVisibility(map_, hit.x, hit.y);
                seen = candidate.distance <= reach &&
                       sightCost(map_, nav_, position.x, position.y, hit.x, hit.y, reach) <= reach;
            }
            if (seen || candidate.heard != SoundKind::None) {
                fresh_.push_back({{hit.id, hit.x, hit.y, tick_, seen, candidate.heard},
                                  candidate.distance,
                                  candidate.interest});
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
                // Источник мог уже исчезнуть: тогда он не угроза, а просто шум.
                const entt::entity source = world_.find(sound.source);
                const Interest interest = source != entt::null
                                              ? interestOf(lookup_, entity, source)
                                              : Interest::Ordinary;
                fresh_.push_back({{sound.source, sound.x, sound.y, tick_, false, sound.kind},
                                  distance,
                                  interest == Interest::Ignored ? Interest::Ordinary : interest});
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
    // Чья очередь в этом тике. Обход — по одному хранилищу Awareness: за тик
    // обновляется десятая часть существ, а view из четырёх компонентов
    // проверял бы у каждого три чужих хранилища (этап 2а.1). Id есть у всех
    // сущностей; Senses и Position проверяются только у тех, чья очередь.
    struct Turn {
        entt::entity entity;
        EntityId id;
        const Senses* senses;
        const Position* position;
        Awareness* awareness;
    };
    // Буфер живёт между тиками. Внутри параллельного прохода — только через
    // ссылку: thread_local в рабочем потоке — это его собственный экземпляр.
    static thread_local std::vector<Turn> buffer;
    std::vector<Turn>& turns = buffer;
    turns.clear();
    const auto& ids = registry.storage<Id>();
    const auto& senses = registry.storage<Senses>();
    const auto& positions = registry.storage<Position>();
    for (const auto [entity, awareness] : registry.view<Awareness>().each()) {
        const EntityId id = ids.get(entity).value;
        if ((tick + id) % kPerceptionIntervalTicks != 0 || !senses.contains(entity) ||
            !positions.contains(entity)) {
            continue;
        }
        turns.push_back({entity, id, &senses.get(entity), &positions.get(entity), &awareness});
    }
    if (turns.empty()) {
        return;
    }

    // Каждое существо меняет только своё Awareness, а читает снимок индекса и
    // компоненты, которых восприятие не трогает, — поэтому порядок, а значит и
    // число потоков, на результат не влияют: обновления идут параллельно
    // (sim/Parallel.h), у каждого потока свой Perceiver с рабочими буферами.
    const SenseConditions conditions = senseConditions(world);
    std::array<std::optional<Perceiver>, kMaxParallelThreads> perceivers;
    const int threads = std::min(parallelThreads(), kMaxParallelThreads);
    for (int i = 0; i < threads; ++i) {
        perceivers[static_cast<std::size_t>(i)].emplace(world, conditions);
    }
    // Кусок — несколько существ: в городе одно обновление стоит около 1,5 мкс.
    constexpr std::size_t kChunk = 6;
    constexpr std::size_t kMinParallel = 24;
    parallelFor(turns.size(), kChunk, kMinParallel, [&](std::size_t begin, std::size_t end, int worker) {
        Perceiver& perceiver = *perceivers[static_cast<std::size_t>(worker)];
        for (std::size_t i = begin; i < end; ++i) {
            const Turn& turn = turns[i];
            perceiver.perceive(turn.entity, turn.id, *turn.senses, *turn.position, *turn.awareness);
        }
    });
}

} // namespace lw
