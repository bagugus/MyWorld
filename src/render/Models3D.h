#pragma once

#include "sim/Components.h"

#include <raylib.h>

#include <cstdint>

namespace lw {

// Низкополигональные модели 3D-вида (этап 11.3): здания с крышами, окнами и
// дверью, люди и зомби из частей, звери, птицы, тела, ящики. Модель — набор
// коробок и граней без текстуры; у каждой грани — нормаль (rlNormal3f), по
// которой её освещает шейдер моделей (render/Lighting3D, этап 11.4); светящиеся
// сами грани (окна ночью) помечены rlTexCoord2f(1, 0).
//
// Все emit* выводят четырёхугольники в уже открытый rlBegin(RL_QUADS) с
// текстурой по умолчанию; грани идут против часовой стрелки снаружи.

// Где стоит модель и как повёрнута. Локальные оси модели: X — вбок, Y — вверх,
// Z — вперёд; origin — точка на земле под моделью.
struct ModelFrame {
    Vector3 origin{};
    float yaw = 0.0f;   // куда смотрит: 0 — на юг (+Z мира), π/2 — на восток (+X)
    float scale = 1.0f;
    float pitch = 0.0f; // наклон всей модели вперёд-назад: −π/2 — лежит на спине
    float roll = 0.0f;  // на боку
};

// Направление взгляда по смещению в плоскости мира (x — восток, y — юг).
float headingOf(float dx, float dy);

// Походка: фаза шага (радианы) и размах (0 — стоит, 1 — обычный шаг).
struct Gait {
    float phase = 0.0f;
    float swing = 0.0f;
};

// Человек или зомби ростом около 1,75 м.
struct HumanLook {
    Color shirt{};      // по занятию или состоянию — как цвет кружка сверху
    Color trousers{};
    Color skin{};
    bool zombie = false;  // руки вперёд, сутулится, бредёт
    bool running = false; // наклон вперёд
    bool aiming = false;  // держит оружие перед собой
    WeaponKind weapon = WeaponKind::Fists;
};
void emitHuman(const ModelFrame& frame, const HumanLook& look, Gait gait);

// Зверь своего вида; grazing — голова к земле, antlers — рога (олень).
void emitAnimal(const ModelFrame& frame, AnimalKind kind, Color color, Gait gait, bool grazing,
                bool antlers);

// Ворона: flap — фаза взмаха крыльев, сидящая — со сложенными крыльями.
void emitBird(const ModelFrame& frame, bool flying, float flap);

// Ящик груза ивента со стороной около 1,2 м.
void emitCrate(const ModelFrame& frame, Color color);

// Здание на участке [x0, x1] × [z0, z1] мира. low и high — нижний и верхний
// углы земли под участком: фундамент уходит ниже low, пол — над high.
struct BuildingModel {
    BuildingKind kind = BuildingKind::House;
    float x0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float z1 = 0.0f;
    float low = 0.0f;
    float high = 0.0f;
    Vector2 door{};          // середина двери на стене, мир (x, z)
    float doorGround = 0.0f; // высота земли у двери: ниже пола — крыльцо
    std::uint64_t seed = 0;  // из id: этажность, цвет крыши
    float snow = 0.0f;       // снег на крыше, 0..1
    bool detailed = true;    // окна, дверь, крыльцо — только вблизи
    float lights = 0.0f;     // доля окон, светящихся ночью: 0 — темно
};
// Высота крыши здания над землёй (для отсечения).
float buildingTop(const BuildingModel& model);
void emitBuilding(const BuildingModel& model);

// Коробка по осям мира от min до max (с дном, если bottom).
void emitWorldBox(Vector3 min, Vector3 max, Color color, bool bottom = false);

} // namespace lw
