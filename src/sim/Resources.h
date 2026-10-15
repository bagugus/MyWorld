#pragma once

#include "sim/Calendar.h"
#include "sim/Grid.h"
#include "sim/Map.h"

#include <cstdint>

namespace lw {

class Places;

// Возобновляемые ресурсы мира: ягоды и грибы в лесу, дождевая вода.
//
// Часть состояния мира (входит в World::stateHash): их расходуют люди, а
// восстанавливают сезон и погода (ResourceSystem). Лес — слой по блокам
// Places::kSpotBlock: сущность на каждые 32 × 32 м леса — десятки тысяч
// сущностей без позиции и поведения, а слой стоит байт на блок.
class Resources {
public:
    // Порций ягод и грибов в лесном блоке — не больше, и сколько в начале мира.
    static constexpr std::uint8_t kForestMax = 4;
    static constexpr std::uint8_t kForestStart = 2;

    // Запас по лесным блокам Places (у нелесных блоков — 0).
    void build(const Places& places);

    // Запас блока, в котором лежит клетка; 0 вне леса и за краем карты.
    std::uint8_t forestAt(Cell cell) const;
    // Взять порцию из блока клетки; false, если там пусто.
    bool takeForest(Cell cell);
    // Суточное изменение запаса по сезону: рост весной, летом и осенью, убыль
    // зимой. dayOfSeason — какой день сезона начинается.
    void growForest(Season season, int dayOfSeason);

    // Дождь за minutes игровых минут при интенсивности precipitation (0..1).
    // Возвращает, сколько порций воды набралось в каждую бочку (обычно 0 или 1).
    int collectRain(float precipitation, float minutes);

    const Grid<std::uint8_t>& forest() const { return forest_; }
    // Лесных блоков и сколько из них сейчас пусто.
    int forestBlocks() const { return forestBlocks_; }
    long long forestTotal() const;
    int forestEmpty() const;

    // Накопленный дождь, доли порции.
    float rainAccumulated() const { return rain_; }
    // Сколько порций дождевой воды набралось в каждую бочку с начала мира.
    std::uint32_t rainPortions() const { return rainPortions_; }

    std::uint64_t hash() const;

private:
    Grid<std::uint8_t> forest_;
    // Лесной ли блок: производное от карты, в хеш не входит.
    Grid<std::uint8_t> isForest_;
    int forestBlocks_ = 0;
    float rain_ = 0.0f;
    std::uint32_t rainPortions_ = 0;
};

} // namespace lw
