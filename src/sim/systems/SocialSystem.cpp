#include "sim/systems/SocialSystem.h"

#include "sim/Components.h"
#include "sim/Memory.h"
#include "sim/Relations.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace lw {

namespace {

// Рядом — не дальше этого, метры (как компания в NeedsSystem).
constexpr float kCompanyRadius = 20.0f;
// Узнаёт за минуту не больше стольких ближайших: в толпе на площади всех не
// запомнить.
constexpr std::size_t kMaxCompany = 3;
// Постоянные времени, часы разговора: за столько остаток до цели (знакомства —
// до 1, симпатии — до совместимости, доверия — до симпатии × знакомство)
// уменьшается примерно в e раз. Просто рядом — в kNearShare раз медленнее.
constexpr float kFamiliarityHours = 3.0f;
constexpr float kAffinityHours = 3.0f;
constexpr float kTrustHours = 24.0f;
constexpr float kNearShare = 0.2f;
// Не виделись дольше этого — знакомство забывается: от полного до нуля за
// kForgetHours.
constexpr std::uint64_t kForgetAfterTicks = 2 * kTicksPerGameDay;
constexpr float kForgetHours = 60.0f * 24.0f;
// Долг и обида угасают сами, постоянная времени — часы. Обида у
// агрессивного держится дольше, у доброго проходит быстрее: множитель
// (0,5 + агрессия) × (1,5 − альтруизм), от 0,25 до 2,25.
constexpr float kDebtHours = 30.0f * 24.0f;
constexpr float kGrudgeHours = 20.0f * 24.0f;
// Меньше этого — угасли совсем.
constexpr float kFeelingGone = 0.01f;
// Мнение о третьем (этап 6.3): в разговоре человек делится самым сильным
// чувством — к тому, кого слушатель тоже знает, если симпатия по модулю и
// знакомство не меньше этих. Симпатия слушателя к третьему тянется к мнению
// рассказчика: за час разговора — на долю kOpinionRate × симпатия к
// рассказчику (к неприятному не прислушиваются).
constexpr float kOpinionAffinity = 0.3f;
constexpr float kOpinionFamiliarity = 0.3f;
constexpr float kOpinionRate = 0.5f;
// Просто рядом (соседи у дверей, жильцы одного дома) новости тоже
// пересказывают, но реже: слух — раз в столько минут (по номеру минуты и id
// пары, без рандома), мнение — в kNearShare раз слабее.
constexpr std::uint64_t kNearRumorMinutes = 5;

bool talking(const Mind& mind) {
    return mind.action == Action::Socialize && mind.phase == ActionPhase::Perform;
}

float approach(float value, float target, float share) {
    return value + (target - value) * std::min(1.0f, share);
}

// Разговор: teller что-то рассказывает listener; conversation — в «Общении»,
// иначе просто рядом.
struct Talk {
    EntityId teller;
    entt::entity tellerEntity;
    EntityId listener;
    entt::entity listenerEntity;
    bool conversation;
};

// Что рассказано: воспоминание (слух) и мнение о третьем.
struct Told {
    EntityId listener;
    EntityId teller;
    entt::entity listenerEntity;
    bool rumor = false;
    Memory memory{};
    EntityId subject = kInvalidId; // о ком мнение
    float opinion = 0.0f;          // симпатия рассказчика к нему
    float weight = 1.0f;           // в разговоре — 1, просто рядом — kNearShare
};

// О ком teller скажет listener: самое сильное чувство (|симпатия| × знакомство)
// к живому человеку, которого listener знает. kInvalidId — не о ком.
EntityId opinionSubject(const World& world, const Relations& teller, EntityId listener,
                        const Relations& theirs) {
    const entt::registry& registry = world.registry();
    EntityId best = kInvalidId;
    float strongest = 0.0f;
    for (std::size_t i = 0; i < teller.count; ++i) {
        const Bond& bond = teller.bonds[i];
        if (bond.other == listener || std::abs(bond.affinity) < kOpinionAffinity ||
            bond.familiarity < kOpinionFamiliarity || theirs.find(bond.other) == nullptr) {
            continue;
        }
        const entt::entity other = world.find(bond.other);
        if (other == entt::null || !registry.all_of<Person>(other)) {
            continue;
        }
        const float strength = std::abs(bond.affinity) * bond.familiarity;
        if (best == kInvalidId || strength > strongest ||
            (strength == strongest && bond.other < best)) {
            best = bond.other;
            strongest = strength;
        }
    }
    return best;
}

} // namespace

void updateRelations(World& world, float minutes) {
    const float hours = minutes / 60.0f;
    const std::uint64_t tick = world.tick();
    entt::registry& registry = world.registry();
    static const Traits kAverage{};

    struct Companion {
        float distanceSq;
        EntityId id;
        entt::entity entity;
    };
    std::vector<Companion> company;
    std::vector<Talk> talks;
    // Каждый меняет только свои связи, поэтому порядок обхода не важен; слухи и
    // мнения меняют чужие — их собираем и применяем после обхода (этап 6.3).
    auto view = registry.view<const Id, const Person, Relations, const Position, const Walker>();
    for (const auto [entity, id, person, relations, position, walker] : view.each()) {
        const Traits* own = registry.try_get<Traits>(entity);
        const Traits& mine = own != nullptr ? *own : kAverage;
        const float grudgeHours =
            kGrudgeHours * (0.5f + mine.aggression) * (1.5f - mine.altruism);
        for (std::size_t i = 0; i < relations.count; ++i) {
            Bond& bond = relations.bonds[i];
            if (tick > bond.seen + kForgetAfterTicks) {
                bond.familiarity = std::max(0.0f, bond.familiarity - hours / kForgetHours);
            }
            if (bond.debt > 0.0f) {
                bond.debt -= bond.debt * std::min(1.0f, hours / kDebtHours);
                bond.debt = bond.debt < kFeelingGone ? 0.0f : bond.debt;
            }
            if (bond.grudge > 0.0f) {
                bond.grudge -= bond.grudge * std::min(1.0f, hours / grudgeHours);
                bond.grudge = bond.grudge < kFeelingGone ? 0.0f : bond.grudge;
            }
        }
        if (person.asleep || walker.moving()) {
            continue;
        }
        company.clear();
        world.spatial().forEachInRadius(
            position.x, position.y, kCompanyRadius, [&](const SpatialHit& hit) {
                if (hit.id == id.value) {
                    return;
                }
                const Person* other = registry.try_get<Person>(hit.entity);
                const Walker* steps = registry.try_get<Walker>(hit.entity);
                if (other != nullptr && !other->asleep && steps != nullptr && !steps->moving()) {
                    company.push_back({hit.distanceSq, hit.id, hit.entity});
                }
            });
        if (company.empty()) {
            continue;
        }
        // Ближайшие; при равных расстояниях — меньший id.
        const std::size_t count = std::min(kMaxCompany, company.size());
        std::partial_sort(company.begin(), company.begin() + static_cast<std::ptrdiff_t>(count),
                          company.end(), [](const Companion& a, const Companion& b) {
                              return a.distanceSq != b.distanceSq ? a.distanceSq < b.distanceSq
                                                                  : a.id < b.id;
                          });
        const Mind* mind = registry.try_get<Mind>(entity);
        const bool speaking = mind != nullptr && talking(*mind);
        for (std::size_t k = 0; k < count; ++k) {
            const Companion& companion = company[k];
            const Mind* theirMind = registry.try_get<Mind>(companion.entity);
            const bool conversation = speaking || (theirMind != nullptr && talking(*theirMind));
            const float intensity = conversation ? 1.0f : kNearShare;
            const Traits* theirs = registry.try_get<Traits>(companion.entity);
            Bond* bond = acquireBond(
                relations, companion.id,
                chemistry(id.value, mine, companion.id, theirs != nullptr ? *theirs : kAverage),
                tick);
            if (bond == nullptr) {
                continue; // все связи крепкие: незнакомца не запомнить
            }
            const float pace = hours * intensity;
            bond->familiarity = approach(bond->familiarity, 1.0f, pace / kFamiliarityHours);
            bond->affinity = approach(bond->affinity, affinityGoal(*bond), pace / kAffinityHours);
            // Доверие от одного общения только растёт; подрывают его поступки
            // (обида — сразу и пока не угаснет).
            const float trusted = trustCeiling(*bond);
            if (trusted > bond->trust) {
                bond->trust = approach(bond->trust, trusted, pace / kTrustHours);
            }
            bond->seen = tick;
            refreshBondFlags(world, id.value, *bond);
            // Рядом рассказывают новости (этап 6.3).
            if (registry.all_of<Relations>(companion.entity)) {
                talks.push_back({id.value, entity, companion.id, companion.entity, conversation});
            }
        }
    }
    if (talks.empty()) {
        return;
    }

    // Что кому рассказано — по состоянию после обхода, пока никто ничего не услышал.
    std::vector<Told> told;
    const std::uint64_t minute = tick / kTicksPerGameMinute;
    for (const Talk& talk : talks) {
        const bool rumorTime =
            talk.conversation || (minute + talk.teller + talk.listener) % kNearRumorMinutes == 0;
        const Memory* rumor =
            rumorTime ? pickRumor(world, talk.tellerEntity, talk.listenerEntity) : nullptr;
        if (rumor != nullptr) {
            Told item{talk.listener, talk.teller, talk.listenerEntity};
            item.rumor = true;
            item.memory = *rumor;
            told.push_back(item);
        }
        const Relations& tellers = registry.get<Relations>(talk.tellerEntity);
        const Relations& listeners = registry.get<Relations>(talk.listenerEntity);
        const EntityId subject = opinionSubject(world, tellers, talk.listener, listeners);
        if (subject != kInvalidId) {
            Told item{talk.listener, talk.teller, talk.listenerEntity};
            item.subject = subject;
            item.opinion = tellers.find(subject)->affinity;
            item.weight = talk.conversation ? 1.0f : kNearShare;
            told.push_back(item);
        }
    }
    // Услышанное — по слушателю и рассказчику (от одного рассказчика сначала
    // слух, потом мнение — так они и собраны): не зависит от порядка реестра.
    std::stable_sort(told.begin(), told.end(), [](const Told& a, const Told& b) {
        return std::tie(a.listener, a.teller) < std::tie(b.listener, b.teller);
    });
    for (const Told& item : told) {
        if (item.rumor) {
            hearRumor(world, item.listenerEntity, item.teller, item.memory);
            continue;
        }
        // Враг моего друга — и мне не мил: к неприятному не прислушиваются.
        Relations& relations = registry.get<Relations>(item.listenerEntity);
        const Bond* toTeller = relations.find(item.teller);
        Bond* toSubject = relations.find(item.subject);
        const float liking = toTeller != nullptr ? std::max(0.0f, toTeller->affinity) : 0.0f;
        if (toSubject == nullptr || liking <= 0.0f) {
            continue;
        }
        toSubject->affinity = approach(toSubject->affinity, item.opinion,
                                       hours * kOpinionRate * liking * item.weight);
        refreshBondFlags(world, item.listener, *toSubject);
    }
}

void tickRelations(World& world) {
    if (world.tick() % kTicksPerGameMinute == 0) {
        updateRelations(world, 1.0f);
    }
}

} // namespace lw
