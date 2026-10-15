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

    // Вызывать после EndMode2D, до интерфейса.
    void draw(const Environment& environment, const Weather& weather) const;

private:
    void drawPrecipitation(const Weather& weather, bool snow) const;

    double time_ = 0.0;
};

} // namespace lw
