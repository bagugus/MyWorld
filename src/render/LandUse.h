#pragma once

#include "sim/Components.h"
#include "sim/Grid.h"
#include "sim/Map.h"

#include <cstdint>
#include <vector>

namespace lw {

class World;

// Что лежит на земле вне леса (этап 11.9): облик мира для 3D, без графики.
// Симуляция об этом не знает: для неё есть только слои карты и здания. Всё —
// детерминированно по карте и зданиям, без рандома мира: одна карта — один облик.
//
// По карте: течение рек (куда и как быстро, перекаты), насколько врезано русло,
// придорожные и прибрежные клетки. По зданиям и поселениям (addSettlements):
// дворы домов с заборами и калитками, огороды, колодцы, скамейки, поленницы,
// стога у сараев, столбы с проводами вдоль дорог, мостки на берегу.
class LandUse {
public:
    explicit LandUse(const Map& map);

    const Map& map() const { return map_; }

    // Течение реки в клетке: куда (единичный вектор, x — восток, z — юг), скорость
    // (м/с) и перекат (0..1: вода падает круто, пенится). Вне рек — нули.
    struct Flow {
        float x = 0.0f;
        float z = 0.0f;
        float speed = 0.0f;
        float rapids = 0.0f;
    };
    Flow flowAt(Cell cell) const;
    // Насколько поверхность воды в 3D опущена ниже расчётной, метры: у рек русло
    // врезано (вода заметно ниже берегов), у устья — плавно как у озёр.
    float channelInset(Cell cell) const;
    // Высота берега над водой в клетке воды, метры: у рек круче, у озёр положе, у моря — ноль.
    float bankHeight(Cell cell) const;

    // Клетки суши вне леса.
    enum Flags : std::uint8_t {
        kBuilding = 1, // под зданием
        kYard = 2,     // двор дома или сарая
        kGarden = 4,   // огород во дворе
        kRoadside = 8, // у дороги (обочина, канава)
        kShore = 16,   // у пресной воды (камыш, осока)
        kSettled = 32, // в поселении (мусор после коллапса, низкая трава)
        kDoor = 64,    // перед входом
    };
    std::uint8_t flags(Cell cell) const {
        return map_.contains(cell) ? flags_.at(cell.x, cell.y) : 0;
    }
    bool has(Cell cell, Flags flag) const { return (flags(cell) & flag) != 0; }

    // Участок здания: двор с забором — у домов и сараев.
    enum class FenceStyle : std::uint8_t { Picket, Wattle, Boards, Rails };
    struct Lot {
        BuildingKind kind = BuildingKind::House;
        Cell origin;
        int width = 0;
        int height = 0;
        Cell door;
        bool village = false;
        FenceStyle fence = FenceStyle::Picket;
        std::uint32_t bits = 0;
    };
    const std::vector<Lot>& lots() const { return lots_; }
    // Участок клетки (двор или здание): номер в lots() + 1; 0 — ничей.
    int lotOf(Cell cell) const { return map_.contains(cell) ? lot_.at(cell.x, cell.y) : 0; }

    // Отдельные предметы: стоят в точке (x, z), смотрят или тянутся по (dx, dz).
    enum class ThingKind : std::uint8_t {
        Fence,    // пролёт забора от точки по направлению на length метров
        Gate,     // пролёт с калиткой (перед входом)
        Well,     // колодец
        Bench,    // скамейка у входа: (dx, dz) — куда смотрит сидящий
        Woodpile, // поленница вдоль стены
        Haystack, // стог у сарая
        Pole,     // столб: провода — к links
        Pier,     // мостки: от берега по (dx, dz) на length метров
    };
    struct Thing {
        ThingKind kind = ThingKind::Fence;
        FenceStyle style = FenceStyle::Picket;
        float x = 0.0f;
        float z = 0.0f;
        float dx = 1.0f;
        float dz = 0.0f;
        float length = 0.0f;
        int links[2] = {-1, -1}; // столб: соседние столбы (номера в things())
        std::uint32_t bits = 0;
    };
    const std::vector<Thing>& things() const { return things_; }
    // Предметы в квадрате kBucketCells × kBucketCells клеток (bx, by) — номера в things().
    static constexpr int kBucketCells = 32;
    const std::vector<int>& thingsIn(int bx, int by) const;

    // Дворы, огороды, предметы у домов, столбы и мостки — по зданиям и
    // поселениям мира; один раз (здания не меняются).
    void addSettlements(const World& world);
    bool hasSettlements() const { return settled_; }

private:
    void computeFlow();
    void computeChannels();
    void markRoadsAndShores();
    void addLots(const World& world);
    void addYardThings();
    void addPoles(const World& world);
    void addPiers(const World& world);
    void bucketThings();
    float waterLevel(Cell cell) const;

    const Map& map_;
    Grid<std::uint8_t> flags_;
    Grid<std::uint16_t> lot_;
    Grid<std::uint8_t> flowDir_; // 0..7 — куда течёт (kDx8, kDy8), 255 — не река
    Grid<std::uint8_t> drop_;    // падение уровня до следующей клетки, сантиметры
    Grid<std::uint8_t> inset_;   // опускание воды, сантиметры
    std::vector<Lot> lots_;
    std::vector<Thing> things_;
    std::vector<std::vector<int>> buckets_;
    int bucketsX_ = 0;
    bool settled_ = false;
};

} // namespace lw
