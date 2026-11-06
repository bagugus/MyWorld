#pragma once

#include <cstdint>

namespace lw {

// Поля в 3D-виде (этап 11.9): лоскуты посевов на клетках Ground::Field. Только
// облик — для симуляции поле остаётся клеткой пашни. Лоскут — ячейка Вороного
// по решётке kFieldPatchSize со сдвинутыми центрами: у каждого своя культура,
// направление рядов и срок; на границе лоскутов — межа (полоса травы).
//
// Раскладка — чистая функция точки мира. Та же формула — в шейдерах (kFieldGlsl):
// земля рисует борозды и ряды, мелочь (render/Props3D) — растения на них.
// Хеш целочисленный и одинаковый в C++ и GLSL, поэтому ряды совпадают.

enum class CropKind : std::uint8_t {
    Fallow,    // пар: луг
    Wheat,     // яровая пшеница
    Rye,       // озимая рожь: зеленеет осенью, зимует под снегом
    Barley,    // ячмень
    Corn,      // кукуруза
    Potato,    // картофель
    Sunflower, // подсолнечник
    Rapeseed,  // озимый рапс: цветёт жёлтым весной
    Garden,    // огород у дома (не лоскут поля)
};

inline constexpr float kFieldPatchSize = 112.0f; // метры
// Межа: ближе этого к границе лоскута посевов нет, метры.
inline constexpr float kBalkWidth = 1.2f;

struct FieldPatch {
    CropKind crop = CropKind::Fallow;
    float angle = 0.0f;  // направление рядов, радианы (0 — вдоль x)
    float timing = 0.0f; // 0..1: на пару дней раньше или позже соседей
    float edge = 0.0f;   // расстояние до границы лоскута, метры
    int id = 0;          // номер лоскута: у соседних разный
    std::uint32_t bits = 0;
};

FieldPatch fieldPatchAt(float x, float z);
// Расстояние между рядами культуры, метры.
float rowSpacing(CropKind crop);
// Ряды культуры поперёк этой оси: координата ряда — dot(p, normal) / rowSpacing,
// середины рядов — на k + 0,5.
void rowNormal(float angle, float& nx, float& nz);

// Целочисленный хеш, одинаковый в C++ и GLSL (kFieldGlsl: hash3).
std::uint32_t fieldHash(int x, int y, std::uint32_t salt);
inline float hashUnit(std::uint32_t h) {
    return static_cast<float>(h >> 8) / 16777216.0f;
}

// Кусок шейдера: fieldPatch(p) — как fieldPatchAt; cropState(kind, year, timing) —
// высота, густота и цвет посева на долю года (с 1 марта); meadowColor(year, v) —
// цвет луга по сезону; harvestTime(kind). Подключать после общего куска света
// (SceneShading::loadShader) или самостоятельно — он ни от чего не зависит.
extern const char* const kFieldGlsl;

} // namespace lw
