#pragma once

#include <raylib.h>

namespace lw {

struct Environment;
struct Weather;

// Свет и небо 3D-вида (этап 11.4) по времени суток, сезону и погоде. Только
// чистые функции от обстановки: симуляция о них не знает, графику они не трогают.
//
// Цвета — доли 0..1 (1 — полная яркость текстуры или цвета модели), направления —
// единичные векторы в осях мира 3D: X — восток, Y — вверх, Z — юг.
struct SceneLight {
    // Главный направленный свет: солнце, а когда оно ниже горизонта и луна
    // светит ярче — луна. lightDir — на источник.
    Vector3 lightDir{0.0f, 1.0f, 0.0f};
    Vector3 lightColor{};
    bool moonlit = false; // главный свет — луна
    // Рассеянный свет: сверху — от неба, снизу — отражённый от земли.
    Vector3 ambientSky{};
    Vector3 ambientGround{};
    // Насколько темнеет тень (0 — теней нет: ночь без луны, пасмурно).
    float shadowStrength = 0.0f;

    // Небо: у зенита и у горизонта; дымка и туман тянут дальнее к fogColor.
    Vector3 zenith{};
    Vector3 horizon{};
    Vector3 fogColor{};
    float fogDensity = 0.0f; // на метр: доля скрытого — 1 − exp(−плотность · расстояние)

    Vector3 sunDir{0.0f, 1.0f, 0.0f};
    Vector3 sunColor{}; // цвет диска и ореола: белый днём, оранжевый у горизонта
    Vector3 moonDir{0.0f, -1.0f, 0.0f};
    float moonFullness = 0.0f;
    float night = 0.0f;      // 0 — день, 1 — ночь: звёзды, свет в окнах
    float cloudCover = 0.0f; // облака на небе
    float windX = 0.0f;      // куда сносит облака, м/с
    float windZ = 0.0f;
};

// Направление на солнце: азимут — от времени до полудня и долготы дня (летом
// восходит на северо-востоке, зимой — на юго-востоке), высота — из SunState.
Vector3 sunDirection(float azimuthDegrees, float elevationDegrees);

SceneLight sceneLightAt(const Environment& environment, const Weather& weather);

} // namespace lw
