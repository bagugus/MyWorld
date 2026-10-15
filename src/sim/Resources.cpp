#include "sim/Resources.h"

#include "sim/Hash.h"
#include "sim/Places.h"

#include <algorithm>
#include <vector>

namespace lw {

namespace {

// Ливень (интенсивность 1) наполняет порцию в бочке за три игровых часа;
// слабый дождь — медленнее, пропорционально.
constexpr float kRainMinutesPerPortion = 180.0f;

// Суточное изменение запаса леса по сезону.
int forestGrowth(Season season, int dayOfSeason) {
    switch (season) {
    case Season::Spring:
        return dayOfSeason % 2 == 0 ? 1 : 0; // первая зелень: порция за двое суток
    case Season::Summer:
        return 1; // ягоды
    case Season::Autumn:
        return 2; // грибы и поздние ягоды
    case Season::Winter:
        return -1; // мороз и снег: всё, что осталось, пропадает за несколько дней
    }
    return 0;
}

} // namespace

void Resources::build(const Places& places) {
    const auto& spots = places.forageSpots();
    forest_ = Grid<std::uint8_t>(spots.width(), spots.height());
    isForest_ = Grid<std::uint8_t>(spots.width(), spots.height());
    forestBlocks_ = 0;
    for (int y = 0; y < spots.height(); ++y) {
        for (int x = 0; x < spots.width(); ++x) {
            if (spots.at(x, y)) {
                isForest_.at(x, y) = 1;
                forest_.at(x, y) = kForestStart;
                ++forestBlocks_;
            }
        }
    }
}

std::uint8_t Resources::forestAt(Cell cell) const {
    const int bx = cell.x / Places::kSpotBlock;
    const int by = cell.y / Places::kSpotBlock;
    if (cell.x < 0 || cell.y < 0 || !forest_.contains(bx, by)) {
        return 0;
    }
    return forest_.at(bx, by);
}

bool Resources::takeForest(Cell cell) {
    if (forestAt(cell) == 0) {
        return false;
    }
    --forest_.at(cell.x / Places::kSpotBlock, cell.y / Places::kSpotBlock);
    return true;
}

void Resources::growForest(Season season, int dayOfSeason) {
    const int growth = forestGrowth(season, dayOfSeason);
    if (growth == 0) {
        return;
    }
    std::vector<std::uint8_t>& stock = forest_.cells();
    const std::vector<std::uint8_t>& forest = isForest_.cells();
    for (std::size_t i = 0; i < stock.size(); ++i) {
        if (forest[i] != 0) {
            stock[i] = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(stock[i]) + growth, 0, static_cast<int>(kForestMax)));
        }
    }
}

int Resources::collectRain(float precipitation, float minutes) {
    if (precipitation <= 0.0f) {
        return 0;
    }
    rain_ += precipitation * minutes / kRainMinutesPerPortion;
    int portions = 0;
    while (rain_ >= 1.0f) {
        rain_ -= 1.0f;
        ++portions;
    }
    rainPortions_ += static_cast<std::uint32_t>(portions);
    return portions;
}

long long Resources::forestTotal() const {
    long long total = 0;
    for (const std::uint8_t stock : forest_.cells()) {
        total += stock;
    }
    return total;
}

int Resources::forestEmpty() const {
    int empty = 0;
    for (std::size_t i = 0; i < forest_.cells().size(); ++i) {
        empty += isForest_.cells()[i] != 0 && forest_.cells()[i] == 0 ? 1 : 0;
    }
    return empty;
}

std::uint64_t Resources::hash() const {
    Hasher hasher;
    for (const std::uint8_t stock : forest_.cells()) {
        hasher.add(stock);
    }
    hasher.add(rain_);
    hasher.add(rainPortions_);
    return hasher.value();
}

} // namespace lw
