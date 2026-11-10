#include "sim/Lod.h"

#include "sim/World.h"

#include <algorithm>
#include <cmath>

namespace lw {

std::uint64_t soundLifetime(const World& world) {
    return world.config().lod ? 2 * kSoundLifetimeTicks * kMaxDetailStride : kSoundLifetimeTicks;
}

Detail detailOf(const entt::registry& registry, entt::entity entity) {
    return DetailLookup(registry).of(entity);
}

void wake(World& world, entt::entity entity) {
    if (!world.config().lod) {
        return; // без LOD все и так на полном уровне
    }
    entt::registry& registry = world.registry();
    Lod* lod = registry.try_get<Lod>(entity);
    if (lod == nullptr) {
        return; // появилось после пересчёта: и так полный
    }
    lod->fullUntil = std::max(lod->fullUntil, world.tick() + kFullHoldTicks);
    if (lod->level != Detail::Full) {
        lod->level = Detail::Full;
        lod->cause = LodCause::Woken;
        if (auto* awareness = registry.try_get<Awareness>(entity)) {
            awareness->window = senseWindow(Detail::Full);
        }
    }
}

void setDetail(entt::registry& registry, entt::entity entity, Detail level, LodCause cause,
               std::uint64_t fullUntil) {
    Lod& lod = registry.get_or_emplace<Lod>(entity);
    lod.level = level;
    lod.cause = cause;
    lod.fullUntil = fullUntil;
    lod.id = registry.get<Id>(entity).value;
    if (auto* awareness = registry.try_get<Awareness>(entity)) {
        awareness->window = senseWindow(level);
    }
}

void attachLod(entt::registry& registry, entt::entity entity) {
    Lod lod;
    lod.id = registry.get<Id>(entity).value;
    registry.emplace_or_replace<Lod>(entity, lod);
}

DetailVerdict judgeDetail(LodKind kind, bool active, const Surroundings& around) {
    if (active) {
        return {Detail::Full, LodCause::Action};
    }
    // Встреча: для человека — зомби рядом, для зомби, зверя и птиц — люди
    // рядом (охота, вспугнуть, погоня). Зомби зверей и птиц не замечают, а те
    // только уходят от них: зомби рядом — спокойный уровень, не полный.
    const bool animal = kind == LodKind::Animal || kind == LodKind::Flock;
    const bool encounter = kind == LodKind::Person ? around.undeadNear : around.peopleNear;
    if (encounter) {
        return {Detail::Full, LodCause::Encounter};
    }
    if (around.activeNear) {
        return {Detail::Full, LodCause::Nearby};
    }
    // Человек всегда среди людей хотя бы сам с собой: ниже спокойного не
    // опускается — у него память, отношения, истории.
    if (kind == LodKind::Person || around.peopleFar) {
        return {Detail::Calm, LodCause::People};
    }
    if (animal && around.undeadNear) {
        return {Detail::Calm, LodCause::Undead};
    }
    return {Detail::Dormant, LodCause::Wilderness};
}

// --- Карта активности -----------------------------------------------------

void ActivityMap::reset(float worldWidth, float worldHeight) {
    width_ = std::max(1, static_cast<int>(std::ceil(worldWidth / kRegionSize)));
    height_ = std::max(1, static_cast<int>(std::ceil(worldHeight / kRegionSize)));
    words_ = (width_ + 63) / 64;
    const int tail = width_ % 64;
    lastMask_ = tail == 0 ? ~0ull : (1ull << tail) - 1;
    const auto size = static_cast<std::size_t>(words_) * static_cast<std::size_t>(height_);
    for (std::vector<std::uint64_t>& layer : bits_) {
        layer.assign(size, 0);
    }
    scratch_.assign(size, 0);
    updated_ = 0;
    computed_ = false;
}

void ActivityMap::clear() {
    for (std::vector<std::uint64_t>& layer : bits_) {
        std::fill(layer.begin(), layer.end(), 0);
    }
}

int ActivityMap::regionX(float x) const {
    return std::clamp(static_cast<int>(std::floor(x / kRegionSize)), 0, width_ - 1);
}

int ActivityMap::regionY(float y) const {
    return std::clamp(static_cast<int>(std::floor(y / kRegionSize)), 0, height_ - 1);
}

void ActivityMap::mark(Layer layer, float x, float y) {
    const int rx = regionX(x);
    const auto index = static_cast<std::size_t>(regionY(y)) * static_cast<std::size_t>(words_) +
                       static_cast<std::size_t>(rx / 64);
    bits_[layer][index] |= 1ull << (rx % 64);
}

bool ActivityMap::testRegion(Layer layer, int rx, int ry) const {
    if (rx < 0 || ry < 0 || rx >= width_ || ry >= height_) {
        return false;
    }
    const auto index = static_cast<std::size_t>(ry) * static_cast<std::size_t>(words_) +
                       static_cast<std::size_t>(rx / 64);
    return (bits_[layer][index] >> (rx % 64) & 1ull) != 0;
}

Surroundings ActivityMap::around(float x, float y) const {
    const int rx = regionX(x);
    const int ry = regionY(y);
    return {testRegion(PeopleNear, rx, ry), testRegion(UndeadNear, rx, ry),
            testRegion(ActiveNear, rx, ry), testRegion(PeopleFar, rx, ry)};
}

void ActivityMap::dilate(Layer from, Layer to, int radius) {
    const std::vector<std::uint64_t>& source = bits_[from];
    std::vector<std::uint64_t>& target = bits_[to];
    const auto words = static_cast<std::size_t>(words_);
    // По горизонтали: бит x строки — в x - radius .. x + radius. Бит i слова w —
    // район w·64 + i; сдвиг влево — к большим x, с переносом из соседнего слова.
    for (int y = 0; y < height_; ++y) {
        const std::uint64_t* row = &source[static_cast<std::size_t>(y) * words];
        std::uint64_t* out = &scratch_[static_cast<std::size_t>(y) * words];
        for (std::size_t w = 0; w < words; ++w) {
            std::uint64_t value = row[w];
            for (int s = 1; s <= radius; ++s) {
                value |= row[w] << s | row[w] >> s;
                if (w > 0) {
                    value |= row[w - 1] >> (64 - s);
                }
                if (w + 1 < words) {
                    value |= row[w + 1] << (64 - s);
                }
            }
            out[w] = value;
        }
        out[words - 1] &= lastMask_;
    }
    // По вертикали: строка y — объединение строк y - radius .. y + radius.
    for (int y = 0; y < height_; ++y) {
        const int y0 = std::max(0, y - radius);
        const int y1 = std::min(height_ - 1, y + radius);
        std::uint64_t* out = &target[static_cast<std::size_t>(y) * words];
        for (std::size_t w = 0; w < words; ++w) {
            std::uint64_t value = 0;
            for (int yy = y0; yy <= y1; ++yy) {
                value |= scratch_[static_cast<std::size_t>(yy) * words + w];
            }
            out[w] = value;
        }
    }
}

std::size_t ActivityMap::memoryBytes() const {
    std::size_t bytes = scratch_.capacity() * sizeof(std::uint64_t);
    for (const std::vector<std::uint64_t>& layer : bits_) {
        bytes += layer.capacity() * sizeof(std::uint64_t);
    }
    return bytes;
}

} // namespace lw
