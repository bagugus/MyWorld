#pragma once

#include "render/Lighting.h"

#include <raylib.h>

#include <string>

namespace lw {

// Свет 3D-вида на видеокарте (этап 11.4): общий кусок шейдеров (рассеянный и
// направленный свет, тень, туман), карта теней от солнца или луны вокруг точки
// взгляда, шейдер моделей, небо с солнцем, луной, звёздами и облаками и
// детальная шумовая текстура. Создавать после InitWindow, уничтожать до CloseWindow.
//
// Карта теней и детальная текстура держатся на своих текстурных блоках
// (kShadowUnit, kDetailUnit): батч rlgl и DrawMesh их не трогают, поэтому
// шейдеры видят их при любом сбросе батча.
class SceneShading {
public:
    SceneShading();
    ~SceneShading();
    SceneShading(const SceneShading&) = delete;
    SceneShading& operator=(const SceneShading&) = delete;

    // Шейдер с общим куском: "#version 330", униформы света и функции
    // shadowAt(p, n), lighting(n, shadow), withFog(color, p).
    static Shader loadShader(const char* vertex, const char* fragmentBody);

    // Свет кадра. eye — где камера; fogScale — множитель плотности дымки
    // (сверху дымки видно меньше).
    void setLight(const SceneLight& light, Vector3 eye, float fogScale);
    const SceneLight& light() const { return light_; }
    // Поставить униформы света, тени и тумана в шейдер (после setLight и тени).
    void apply(const Shader& shader) const;

    // Тени. Квадрат карты теней — вокруг точки center с полустороной radius;
    // false — тени в этом кадре не нужны (ночь без луны, пасмурно, слишком далеко).
    // Между begin и end рисуются отбрасывающие тень: глубина с точки зрения света.
    bool beginShadowPass(Vector3 center, float radius);
    void endShadowPass();
    bool shadowsOn() const { return shadowsOn_; }
    // Попадает ли точка (с запасом size) в квадрат теней.
    bool inShadowArea(Vector3 point, float size) const;
    Vector2 shadowCenter() const { return {shadowCenter_.x, shadowCenter_.z}; }
    float shadowRadius() const { return shadowRadius_; }

    // Небо на весь экран: вызывать до BeginMode3D. time — секунды для облаков.
    void drawSky(const Camera3D& camera, float time);

    // Модели: всё, что выводится батчем rlgl между begin и end, освещается
    // (нормаль — rlNormal3f, свечение — rlTexCoord2f(1, 0)).
    void beginModels() const;
    void endModels() const;

    // Привязать карту теней и детальную текстуру к их блокам; отвязать.
    void bindTextures() const;
    void unbindTextures() const;

    static constexpr int kShadowUnit = 7;
    static constexpr int kDetailUnit = 6;

private:
    SceneLight light_{};
    Vector3 eye_{};
    float fogDensity_ = 0.0f;

    Shader modelShader_{};
    Shader skyShader_{};
    Texture2D detail_{};

    RenderTexture2D shadowTarget_{};
    unsigned int shadowDepth_ = 0;
    bool shadowsOn_ = false;
    Vector3 shadowCenter_{};
    float shadowRadius_ = 0.0f;
    float shadowRange_ = 1.0f; // глубина ортогональной проекции света, метры
    Matrix lightViewProj_{};
};

} // namespace lw
