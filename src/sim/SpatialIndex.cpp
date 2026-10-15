#include "sim/SpatialIndex.h"

#include "sim/Components.h"

#include <cassert>
#include <cmath>

namespace lw {

namespace {

int bucketCount(float worldSize) {
    return std::max(1, static_cast<int>(std::ceil(worldSize / SpatialIndex::kBucketSize)));
}

} // namespace

SpatialIndex::SpatialIndex(float worldWidth, float worldHeight)
    : columns_(bucketCount(worldWidth)),
      rows_(bucketCount(worldHeight)),
      buckets_(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_)) {}

int SpatialIndex::axisBucket(float coordinate, int count) {
    // Сравнения до приведения к int: так NaN, бесконечность и далёкие точки
    // не переполняют целое, а попадают в крайнюю корзину.
    const float bucket = coordinate / kBucketSize;
    if (!(bucket >= 0.0f)) {
        return 0;
    }
    if (bucket >= static_cast<float>(count)) {
        return count - 1;
    }
    return static_cast<int>(bucket);
}

void SpatialIndex::sync(const entt::registry& registry) {
    // Порядок обхода реестра на результат не влияет: каждая сущность
    // ставится на своё место в отсортированной корзине.
    for (const auto [entity, position] : registry.view<const Position>().each()) {
        place(registry, entity, position.x, position.y);
    }
}

void SpatialIndex::place(const entt::registry& registry, entt::entity entity, float x, float y) {
    const auto index = static_cast<std::size_t>(entt::to_entity(entity));
    if (index >= slots_.size()) {
        slots_.resize(index + 1);
    }
    // Номер занят прежней сущностью, удалённой мимо сигнала, — убрать её запись.
    if (slots_[index].entity != entt::null && slots_[index].entity != entity) {
        remove(slots_[index].entity);
    }

    Slot& slot = slots_[index];
    const std::uint32_t bucket = bucketAt(x, y);
    if (slot.entity == entt::null) {
        const EntityId id = registry.get<Id>(entity).value;
        insertEntry(bucket, Entry{id, entity});
        slot = Slot{entity, id, bucket, x, y};
        ++size_;
        return;
    }
    slot.x = x;
    slot.y = y;
    if (bucket != slot.bucket) {
        eraseEntry(slot.bucket, slot.id);
        insertEntry(bucket, Entry{slot.id, entity});
        slot.bucket = bucket;
    }
}

void SpatialIndex::remove(entt::entity entity) {
    const auto index = static_cast<std::size_t>(entt::to_entity(entity));
    if (index >= slots_.size() || slots_[index].entity != entity) {
        return;
    }
    Slot& slot = slots_[index];
    eraseEntry(slot.bucket, slot.id);
    slot = Slot{};
    --size_;
}

void SpatialIndex::onPositionDestroyed(entt::registry& /*registry*/, entt::entity entity) {
    remove(entity);
}

void SpatialIndex::queryRadius(float x, float y, float radius,
                               std::vector<SpatialHit>& out) const {
    out.clear();
    forEachInRadius(x, y, radius, [&out](const SpatialHit& hit) { out.push_back(hit); });
}

void SpatialIndex::insertEntry(std::uint32_t bucket, const Entry& entry) {
    std::vector<Entry>& entries = buckets_[bucket];
    const auto it = std::lower_bound(entries.begin(), entries.end(), entry.id,
                                     [](const Entry& e, EntityId id) { return e.id < id; });
    entries.insert(it, entry);
}

void SpatialIndex::eraseEntry(std::uint32_t bucket, EntityId id) {
    std::vector<Entry>& entries = buckets_[bucket];
    const auto it = std::lower_bound(entries.begin(), entries.end(), id,
                                     [](const Entry& e, EntityId value) { return e.id < value; });
    assert(it != entries.end() && it->id == id);
    entries.erase(it);
}

} // namespace lw
