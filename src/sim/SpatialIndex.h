#pragma once

#include "sim/Id.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lw {

// Сущность, найденная запросом к индексу. Позиция — из снимка индекса.
struct SpatialHit {
    EntityId id = kInvalidId;
    entt::entity entity = entt::null;
    float x = 0.0f;
    float y = 0.0f;
    float distanceSq = 0.0f; // квадрат расстояния до точки запроса, м²
};

// Пространственный индекс для поиска соседей: равномерная сетка корзин поверх мира.
// В индексе — все сущности с компонентом Position (у них должен быть и Id).
//
// Снимок. Индекс хранит позиции на момент последней синхронизации (sync), а мир
// синхронизирует его в конце тика. Поэтому запросы в течение тика видят всех на
// местах, где те были в начале тика, и результат не зависит от того, в каком
// порядке системы двигают существ. Созданные за тик сущности появляются в индексе
// со следующей синхронизацией, удалённые (или потерявшие Position) исчезают сразу.
//
// Детерминизм. Записи в корзине отсортированы по EntityId, поэтому содержимое
// индекса зависит только от позиций сущностей, а не от истории вставок и порядка
// хранения в реестре. Так он восстанавливается после загрузки сохранения одним
// вызовом sync, а в хеш мира не входит: это производные данные.
//
// Точки за краем мира относятся к крайним корзинам — их тоже можно искать.
class SpatialIndex {
public:
    // Сторона корзины, метры. Запросы в основном — в радиусе зрения и слуха
    // (десятки метров): такой круг накрывает несколько корзин, а пустые корзины
    // на карте по умолчанию (256 × 256) стоят немного.
    static constexpr float kBucketSize = 32.0f;

    SpatialIndex(float worldWidth, float worldHeight);

    // Привести индекс к позициям всех сущностей с Position.
    void sync(const entt::registry& registry);
    // Убрать сущность из индекса; если её там нет — ничего.
    void remove(entt::entity entity);
    // Слушатель сигнала registry.on_destroy<Position>().
    void onPositionDestroyed(entt::registry& registry, entt::entity entity);

    std::size_t size() const { return size_; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }

    // Вызывает fn(const SpatialHit&) для каждой сущности не дальше radius от точки.
    // Порядок: корзины по строкам, внутри корзины — по возрастанию id. Он не зависит
    // от истории индекса, но это не порядок по расстоянию.
    template <typename Fn>
    void forEachInRadius(float x, float y, float radius, Fn&& fn) const;

    // То же, результат — в out (прежнее содержимое стирается).
    void queryRadius(float x, float y, float radius, std::vector<SpatialHit>& out) const;

    // Ближайшая к точке сущность не дальше maxRadius, для которой accept(hit) == true.
    // При равных расстояниях — с меньшим id. Корзины обходятся кольцами от центра,
    // поэтому близкая цель находится быстро даже при большом maxRadius.
    template <typename Accept>
    std::optional<SpatialHit> nearest(float x, float y, float maxRadius, Accept&& accept) const;
    std::optional<SpatialHit> nearest(float x, float y, float maxRadius) const {
        return nearest(x, y, maxRadius, [](const SpatialHit&) { return true; });
    }

private:
    // Запись корзины. Позиция — в slots_: при движении внутри корзины
    // меняется только она, а корзина не трогается.
    struct Entry {
        EntityId id;
        entt::entity entity;
    };

    // Снимок сущности; индекс массива — номер сущности в реестре (entt::to_entity).
    struct Slot {
        entt::entity entity = entt::null; // null — номер свободен
        EntityId id = kInvalidId;
        std::uint32_t bucket = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    const Slot& slotOf(const Entry& entry) const {
        return slots_[static_cast<std::size_t>(entt::to_entity(entry.entity))];
    }
    std::size_t bucketIndex(int bx, int by) const {
        return static_cast<std::size_t>(by) * static_cast<std::size_t>(columns_) +
               static_cast<std::size_t>(bx);
    }
    const std::vector<Entry>& bucket(int bx, int by) const { return buckets_[bucketIndex(bx, by)]; }
    // Есть ли кто-то в корзине. Битовая карта занимает 8 КБ на карту по
    // умолчанию и не вылетает из кэша, а заголовки корзин — 1,5 МБ: при обходе
    // круга в сотню корзин, почти все пустые, проверка бита экономит промахи.
    bool occupied(int bx, int by) const {
        const std::size_t index = bucketIndex(bx, by);
        return (occupied_[index >> 6] >> (index & 63)) & 1u;
    }

    // Номер корзины по одной оси; за краем мира — крайняя корзина.
    static int axisBucket(float coordinate, int count);
    std::uint32_t bucketAt(float x, float y) const {
        return static_cast<std::uint32_t>(axisBucket(y, rows_) * columns_ +
                                          axisBucket(x, columns_));
    }

    void place(const entt::registry& registry, entt::entity entity, float x, float y);
    void insertEntry(std::uint32_t bucket, const Entry& entry);
    void eraseEntry(std::uint32_t bucket, EntityId id);

    // Проверить корзину (bx, by) и обновить лучшего кандидата для nearest.
    template <typename Accept>
    void scanNearest(int bx, int by, float x, float y, float maxRadiusSq, Accept& accept,
                     std::optional<SpatialHit>& best) const;

    int columns_;
    int rows_;
    std::size_t size_ = 0;
    std::vector<std::vector<Entry>> buckets_;
    std::vector<std::uint64_t> occupied_; // бит на корзину: она не пуста
    std::vector<Slot> slots_;
};

template <typename Fn>
void SpatialIndex::forEachInRadius(float x, float y, float radius, Fn&& fn) const {
    if (!(radius >= 0.0f)) {
        return;
    }
    const float radiusSq = radius * radius;
    const int x0 = axisBucket(x - radius, columns_);
    const int x1 = axisBucket(x + radius, columns_);
    const int y0 = axisBucket(y - radius, rows_);
    const int y1 = axisBucket(y + radius, rows_);
    for (int by = y0; by <= y1; ++by) {
        // Непустые корзины строки — по битам карты занятости, словами по 64.
        std::size_t index = bucketIndex(x0, by);
        const std::size_t end = bucketIndex(x1, by) + 1;
        while (index < end) {
            const std::size_t shift = index & 63;
            const std::size_t take = std::min<std::size_t>(64 - shift, end - index);
            std::uint64_t bits = occupied_[index >> 6] >> shift;
            if (take < 64) {
                bits &= (std::uint64_t{1} << take) - 1;
            }
            while (bits != 0) {
                const std::size_t occupiedIndex =
                    index + static_cast<std::size_t>(std::countr_zero(bits));
                for (const Entry& entry : buckets_[occupiedIndex]) {
                    const Slot& slot = slotOf(entry);
                    const float dx = slot.x - x;
                    const float dy = slot.y - y;
                    const float distanceSq = dx * dx + dy * dy;
                    if (distanceSq <= radiusSq) {
                        fn(SpatialHit{entry.id, entry.entity, slot.x, slot.y, distanceSq});
                    }
                }
                bits &= bits - 1;
            }
            index += take;
        }
    }
}

template <typename Accept>
void SpatialIndex::scanNearest(int bx, int by, float x, float y, float maxRadiusSq,
                               Accept& accept, std::optional<SpatialHit>& best) const {
    if (bx < 0 || by < 0 || bx >= columns_ || by >= rows_ || !occupied(bx, by)) {
        return;
    }
    for (const Entry& entry : bucket(bx, by)) {
        const Slot& slot = slotOf(entry);
        const float dx = slot.x - x;
        const float dy = slot.y - y;
        const float distanceSq = dx * dx + dy * dy;
        if (distanceSq > maxRadiusSq) {
            continue;
        }
        if (best && (distanceSq > best->distanceSq ||
                     (distanceSq == best->distanceSq && entry.id > best->id))) {
            continue;
        }
        const SpatialHit hit{entry.id, entry.entity, slot.x, slot.y, distanceSq};
        if (accept(hit)) {
            best = hit;
        }
    }
}

template <typename Accept>
std::optional<SpatialHit> SpatialIndex::nearest(float x, float y, float maxRadius,
                                                Accept&& accept) const {
    std::optional<SpatialHit> best;
    if (!(maxRadius >= 0.0f)) {
        return best;
    }
    const float maxRadiusSq = maxRadius * maxRadius;
    const int cx = axisBucket(x, columns_);
    const int cy = axisBucket(y, rows_);
    // Кольцо ring — корзины на расстоянии ring от центральной по Чебышёву.
    // Всё в нём не ближе (ring - 1) * kBucketSize к точке запроса; для точек за
    // краем мира тоже: они дальше от внутренних корзин, а не ближе.
    const int lastRing = std::max(std::max(cx, columns_ - 1 - cx), std::max(cy, rows_ - 1 - cy));
    scanNearest(cx, cy, x, y, maxRadiusSq, accept, best);
    for (int ring = 1; ring <= lastRing; ++ring) {
        const float innerDistance = static_cast<float>(ring - 1) * kBucketSize;
        // Строгое сравнение оставляет шанс кандидату на том же расстоянии с меньшим id.
        if (innerDistance > maxRadius ||
            (best && best->distanceSq < innerDistance * innerDistance)) {
            break;
        }
        for (int bx = cx - ring; bx <= cx + ring; ++bx) {
            scanNearest(bx, cy - ring, x, y, maxRadiusSq, accept, best);
            scanNearest(bx, cy + ring, x, y, maxRadiusSq, accept, best);
        }
        for (int by = cy - ring + 1; by <= cy + ring - 1; ++by) {
            scanNearest(cx - ring, by, x, y, maxRadiusSq, accept, best);
            scanNearest(cx + ring, by, x, y, maxRadiusSq, accept, best);
        }
    }
    return best;
}

} // namespace lw
