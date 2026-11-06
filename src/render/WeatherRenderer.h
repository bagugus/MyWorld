#pragma once

namespace lw {

struct Environment;
struct Weather;

// Погода и время суток поверх мира, в экранных координатах: осадки, туман,
// затемнение ночью, оттенок на рассвете и закате, вспышки молний.
// Только читает состояние; частицы и молнии — чисто визуальные, на мир не влияют.
class WeatherRenderer {
public:
    // Анимация идёт по реальному времени и стоит на паузе вместе с миром.
    void update(float realDt, bool running);

    // Вызывать после EndMode2D, до интерфейса. lit — сцена уже освещена по
    // времени суток (3D-вид): ночь, туман и закат там в шейдерах, поверх —
    // только осадки и молнии.
    // indoors — камера в комнате (12а.14): осадков на экране нет.
    void draw(const Environment& environment, const Weather& weather, bool lit = false, bool indoors = false) const;

private:
    void drawPrecipitation(const Weather& weather, bool snow) const;

    double time_ = 0.0;
};

} // namespace lw
