#pragma once

#include "render/Interior.h"
#include "render/Poses.h"
#include "sim/Components.h"

#include <raylib.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace lw {

// Низкополигональные модели 3D-вида (этап 11.3): здания с крышами, окнами и
// дверью, люди и зомби из частей, звери, птицы, тела, ящики. Модель — набор
// коробок и граней без текстуры; у каждой грани — нормаль (rlNormal3f), по
// которой её освещает шейдер моделей (render/Lighting3D, этап 11.4); светящиеся
// сами грани (окна ночью) помечены rlTexCoord2f(1, 0), грани внутри
// помещения (12а.14) — rlTexCoord2f(2, 0).
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

// Человек или зомби ростом около 1,75 м в позе pose (render/Poses, этап 12а.11):
// колени, локти, поворот головы и корпуса, сидит, лежит, на коленях.
struct HumanLook {
    Color shirt{};      // по занятию или состоянию — как цвет кружка сверху
    Color trousers{};
    Color skin{};
    Color hair{};
    bool zombie = false;
    bool aiming = false;  // оружие в руках, а не за спиной или на поясе
    WeaponKind weapon = WeaponKind::Fists;
    bool simple = false;  // издали: ноги одним блоком, без рук и оружия
    Prop prop = Prop::None; // что в руке или рядом: инструмент, бутылка, прилавок
    bool bag = false;       // сумка на боку
};
void emitHuman(const ModelFrame& frame, const HumanLook& look, const HumanPose& pose);

// Зверь своего вида: ноги — по шагу, голова и уши — по позе; antlers — рога (олень).
void emitAnimal(const ModelFrame& frame, AnimalKind kind, Color color, Gait gait, const AnimalPose& pose,
                bool antlers);

// Ворона: flap — фаза взмаха крыльев, сидящая — со сложенными крыльями;
// peck — сидящая клюёт (0 — голова поднята, 1 — у земли).
void emitBird(const ModelFrame& frame, bool flying, float flap, float peck = 0.0f);

// Значок эмоции над головой: base — низ значка, yaw — к зрителю, size — высота, м.
void emitEmote(Vector3 base, float yaw, Emote emote, float size);

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
    float detail = 1.0f;     // насколько окна и дверь отличимы от стены: у края
                             // детализации тают в цвет стены, а не мелькают
    float decalOut = 0.04f;  // окна и дверь — на столько метров снаружи стены
                             // (при взгляде в упор; вскользь — дальше)
    std::optional<Vector3> eye; // откуда смотрят: стены вскользь — окна тают в цвет стены
    float lights = 0.0f;     // доля окон, светящихся ночью: 0 — темно
};
// Высота крыши здания над землёй (для отсечения).
float buildingTop(const BuildingModel& model);
// Здание целиком: коробка и, если detailed, окна, дверь, крыльцо, вывеска.
void emitBuilding(const BuildingModel& model);
// Коробка: цоколь, стены, крыша со снегом, труба, парапет. От взгляда не
// зависит (только от снега), поэтому идёт в статичные сетки (render/Buildings3D).
void emitBuildingShell(const BuildingModel& model);
// Окна, дверь, крыльцо, вывеска — вблизи, по взгляду (decalOut, eye, detail, lights).
void emitBuildingDetails(const BuildingModel& model);

// Коробка здания (этап 12а.14): пол первого этажа, высота и число этажей —
// по ним строится планировка внутри (render/Interior).
struct BuildingShell {
    float floor = 0.0f;   // пол первого этажа, мир
    float storey = 3.0f;  // высота этажа, м
    int storeys = 1;
    float wallTop = 0.0f; // верх стен
};
BuildingShell buildingShell(const BuildingModel& model);

// Проёмы в наружных стенах (render/Interior, WallOpening), в осях мира: те же,
// что рисует emitBuilding снаружи, чтобы окна в комнате совпадали с окнами дома.
std::vector<WallOpening> buildingOpenings(const BuildingModel& model);

// Что из планировки прятать и как светить (этап 12а.14).
struct InteriorLook {
    const std::vector<bool>* hiddenWalls = nullptr;     // по индексу InteriorLayout::walls
    const std::vector<bool>* hiddenFurniture = nullptr; // по индексу InteriorLayout::furniture
    bool lampsOn = false;  // абажуры светятся
    bool shadowPass = false; // в карту теней: стены, пол, потолок, без мелочи
};
// Комнаты изнутри: пол, потолок, стены с проёмами дверей и окон, мебель.
// Грани помечены как «внутри» (рассеянный свет приглушён, render/Lighting3D).
void emitInterior(const InteriorLayout& layout, const InteriorLook& look);

// Следующие грани — внутри помещения: рассеянный свет неба на них приглушён
// (люди в комнате). Пишется в первую текстурную координату грани.
void setModelIndoor(bool indoor);

// Грани, собранные в массивы вместо rlgl (этап 2а.2): из них строятся
// статичные сетки (здания вдали — render/Buildings3D). Четыре вершины на грань
// (четырёхугольник a-b-c-d), атрибуты — как у шейдера моделей.
struct ModelMesh {
    std::vector<float> vertices;       // x, y, z
    std::vector<float> normals;        // x, y, z
    std::vector<float> texcoords;      // u, v: свечение или «внутри», растворение
    std::vector<unsigned char> colors; // r, g, b, a
    std::size_t vertexCount() const { return vertices.size() / 3; }
    void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 normal, Vector2 texcoord, Color color);
    void clear();
};
// Пока захват включён, все emit* пишут грани в sink, а не в rlgl.
void beginModelCapture(ModelMesh& sink);
void endModelCapture();

// Коробка по осям мира от min до max (с дном, если bottom).
void emitWorldBox(Vector3 min, Vector3 max, Color color, bool bottom = false);
// Коробка с серединой center и полуразмерами half, повёрнутая на yaw вокруг
// вертикали (ось Z коробки — по направлению yaw, как у ModelFrame).
void emitTurnedBox(Vector3 center, Vector3 half, float yaw, Color color, bool bottom = false);

// Метка выбранного: светящаяся перевёрнутая пирамидка высотой size, острием в tip.
void emitBeacon(Vector3 tip, float size, Color color);

// Насколько видны следующие модели: 1 — целиком, меньше — растворяются точками
// (у края дальности прорисовки). Пишется во вторую текстурную координату грани.
void setModelFade(float visible);

} // namespace lw
