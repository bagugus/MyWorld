#pragma once

#include "sim/Id.h"

#include <cstdint>
#include <unordered_map>

namespace lw {

class World;

namespace ui {

// Ракурсы и композиция режима «Сериал» (этап 12а.12) — то, что не зависит от
// графики и проверяется тестом (lw_composition_tests): смысл сцены из мира,
// память сторон кадра (правило 180°), свободное место по взгляду (правило
// третей), цвет и виньетка по настроению. Кадр по ним ставит кинокамера
// (ui/CinemaCamera); симуляцию это не трогает.

// О чём сцена: от этого — план, ракурс, движение камеры и цвет.
enum class SceneSense : std::uint8_t {
    Daily,    // быт: идёт, работает, отдыхает
    Talk,     // разговор, обмен, помощь — двое рядом
    Chase,    // погоня: бежит от угрозы
    Fight,    // драка или выстрел
    Ambush,   // засада: угроза рядом, а герой о ней не знает
    Threat,   // угроза видна: зомби, орда, напавший; или сам герой — угроза
    Grief,    // горе: весть о гибели близкого
    Helpless, // беспомощность: тяжело ранен, лихорадка
    Family,   // дома с домашними
    Count
};

const char* sceneSenseTitle(SceneSense sense);

struct SceneReading {
    SceneSense sense = SceneSense::Daily;
    // С кем сцена: собеседник, преследователь, противник, угроза; kInvalidId — ни с кем.
    EntityId other = kInvalidId;
    float otherX = 0.0f; // где он, метры мира
    float otherY = 0.0f;
    bool panic = false;    // паника: голландский угол
    bool menacing = false; // герой сам — угроза (зомби, мститель): нижний ракурс на него
    bool moving = false;   // идёт или бежит
    float moveX = 0.0f;    // куда (единичный вектор), если moving
    float moveY = 0.0f;
};

// Смысл сцены вокруг hero сейчас. Только читает мир.
SceneReading readScene(const World& world, EntityId hero);

// Правило 180°: камера держится одной стороны линии действия между двумя
// людьми — от склейки к склейке они остаются на своих сторонах кадра; и
// направление движения героя по экрану сохраняется между склейками.
class ScreenMemory {
public:
    // С какой стороны линии a → b стоит камера: +1 — справа по ходу от a к b,
    // −1 — слева. Пара встречается впервые — suggested (и запоминается).
    float sideOf(EntityId a, EntityId b, float suggested);
    // Куда по экрану шёл герой в прошлых кадрах: +1 — вправо, −1 — влево, 0 — не помним.
    float motionOf(EntityId hero) const;
    void rememberMotion(EntityId hero, float screenDirection);
    void clear();

private:
    struct Entry {
        float value = 0.0f;
        std::uint64_t used = 0;
    };
    void trim(std::unordered_map<std::uint64_t, Entry>& table);

    std::unordered_map<std::uint64_t, Entry> pairs_;
    std::unordered_map<std::uint64_t, Entry> motion_;
    std::uint64_t clock_ = 0;
};

// Куда по экрану смотрит или идёт тот, кто обращён по курсу heading (atan2(dx,
// dy), как у моделей), если камера стоит с курса cameraYaw (градусы, как у
// орбиты): −1 — влево, +1 — вправо, 0 — на камеру или от неё.
float screenDirection(float heading, float cameraYaw);
// Правило третей: на сколько ширины экрана от середины поставить того, кто
// смотрит по экрану в сторону look (−1..1): на третью линию, свободное место —
// перед взглядом. previous — нынешнее место (гистерезис: на мелкий поворот
// головы кадр не перестраивается).
float thirdsOffset(float look, float previous);

// Цвет и виньетка кадра по настроению (только кино, в шейдерах SceneShading):
// tint — добавка к единице по каналам, desaturate — доля серого, contrast —
// прибавка контраста, vignette — затемнение углов. Всё нулевое — кадр как есть.
struct ColorGrade {
    float tintR = 0.0f;
    float tintG = 0.0f;
    float tintB = 0.0f;
    float desaturate = 0.0f;
    float contrast = 0.0f;
    float vignette = 0.0f;
};
ColorGrade gradeOf(SceneSense sense, bool panic, float night);
// Плавный переход между настроениями: t — доля пути 0..1.
ColorGrade mixGrade(const ColorGrade& a, const ColorGrade& b, float t);

} // namespace ui
} // namespace lw
