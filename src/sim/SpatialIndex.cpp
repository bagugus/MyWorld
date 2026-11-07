#include "sim/SpatialIndex.h"

#include "sim/Components.h"
#include "sim/Parallel.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>

namespace lw {

namespace {

int bucketCount(float worldSize) {
    return std::max(1, static_cast<int>(std::ceil(worldSize / SpatialIndex::kBucketSize)));
}

} // namespace

SpatialIndex::SpatialIndex(float worldWidth, float worldHeight)
    : columns_(bucketCount(worldWidth)),
      rows_(bucketCount(worldHeight)),
      rowEntries_(static_cast<std::size_t>(rows_)),
      occupied_((static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_) + 63) / 64, 0) {}

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
    const auto* positions = registry.storage<Position>();
    if (positions == nullptr || positions->empty()) {
        return;
    }
    // Номер сущности меньше размера разреженного массива хранилища: слоты —
    // заранее, чтобы параллельный проход не менял вектор.
    if (slots_.size() < positions->extent()) {
        slots_.resize(positions->extent());
    }
    // Сначала параллельно (этап 2а.1): кто остался в своей корзине, тому
    // достаточно новой позиции в своём слоте — слоты разных сущностей не
    // пересекаются. Остальные (сменили корзину, новые, слот занят прежней
    // сущностью) откладываются и ставятся по одному.
    static_assert(kSyncWorkers >= static_cast<std::size_t>(kMaxParallelThreads));
    for (std::vector<Moved>& list : moved_) {
        list.clear();
    }
    const entt::entity* entities = positions->data();
    constexpr std::size_t kChunk = 512;
    constexpr std::size_t kMinParallel = 2048;
    parallelFor(positions->size(), kChunk, kMinParallel, [&](std::size_t begin, std::size_t end, int worker) {
        std::vector<Moved>& list = moved_[static_cast<std::size_t>(worker)];
        for (std::size_t i = begin; i < end; ++i) {
            const entt::entity entity = entities[i];
            const Position& position = positions->get(entity);
            Slot& slot = slots_[static_cast<std::size_t>(entt::to_entity(entity))];
            if (slot.entity == entity && bucketAt(position.x, position.y) == slot.bucket) {
                slot.x = position.x;
                slot.y = position.y;
                continue;
            }
            list.push_back({entity, position.x, position.y});
        }
    });
    // Порядок вставок на результат не влияет — ряд всегда отсортирован по
    // столбцу и id, — но идёт по номеру сущности, чтобы не зависеть от того,
    // какой поток какой кусок взял.
    std::vector<Moved>& all = moved_[0];
    for (std::size_t worker = 1; worker < moved_.size(); ++worker) {
        all.insert(all.end(), moved_[worker].begin(), moved_[worker].end());
    }
    std::sort(all.begin(), all.end(), [](const Moved& a, const Moved& b) {
        return entt::to_integral(a.entity) < entt::to_integral(b.entity);
    });
    for (const Moved& moved : all) {
        place(registry, moved.entity, moved.x, moved.y);
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
        insertEntry(bucket, Entry{0, entity, id});
        slot = Slot{entity, id, bucket, x, y};
        ++size_;
        return;
    }
    slot.x = x;
    slot.y = y;
    if (bucket != slot.bucket) {
        eraseEntry(slot.bucket, slot.id);
        insertEntry(bucket, Entry{0, entity, slot.id});
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

std::size_t SpatialIndex::memoryBytes() const {
    std::size_t bytes = rowEntries_.capacity() * sizeof(std::vector<Entry>) +
                        occupied_.capacity() * sizeof(std::uint64_t) + slots_.capacity() * sizeof(Slot);
    for (const std::vector<Entry>& row : rowEntries_) {
        bytes += row.capacity() * sizeof(Entry);
    }
    return bytes;
}

void SpatialIndex::insertEntry(std::uint32_t bucket, const Entry& entry) {
    const auto columns = static_cast<std::uint32_t>(columns_);
    std::vector<Entry>& row = rowEntries_[bucket / columns];
    Entry placed = entry;
    placed.column = bucket % columns;
    const auto it = std::lower_bound(row.begin(), row.end(), placed, [](const Entry& e, const Entry& value) {
        return before(e, value.column, value.id);
    });
    row.insert(it, placed);
    occupied_[bucket >> 6] |= std::uint64_t{1} << (bucket & 63);
}

void SpatialIndex::eraseEntry(std::uint32_t bucket, EntityId id) {
    const auto columns = static_cast<std::uint32_t>(columns_);
    std::vector<Entry>& row = rowEntries_[bucket / columns];
    const std::uint32_t column = bucket % columns;
    auto it = std::lower_bound(row.begin(), row.end(), id, [column](const Entry& e, EntityId value) {
        return before(e, column, value);
    });
    assert(it != row.end() && it->column == column && it->id == id);
    it = row.erase(it);
    // Корзина опустела, если соседи по ряду — из других столбцов.
    const bool left = it != row.begin() && std::prev(it)->column == column;
    const bool right = it != row.end() && it->column == column;
    if (!left && !right) {
        occupied_[bucket >> 6] &= ~(std::uint64_t{1} << (bucket & 63));
    }
}

} // namespace lw
