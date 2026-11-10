#pragma once

#include <imgui.h>

#include <string>
#include <string_view>

namespace lw::ui {

// Оформление режима «Сериал» (этап 12а.7): широкий кадр с чёрными полосами,
// затемнение склеек и титры прямо на кадре — своими шрифтами (ui::titleFont и
// др.), с тенью, без окон. Рисуется поверх всего (передний список рисования
// ImGui), между ui::beginFrame и ui::endFrame. Что писать, решает режиссёр.
namespace frame {

// Масштаб титров: 1 — экран высотой 900 пикселей.
float scale();
// Высота каждой полосы широкого кадра (2,35 : 1), пикселей; shown — насколько
// полосы выехали, 0..1. Не меньше доли экрана, чтобы титрам было где стоять.
float barHeight(float shown);
void drawBars(float shown);
// Чёрный кадр поверх всего: alpha 0..1.
void drawBlackout(float alpha);

enum class Align { Left, Center, Right };

// Строка (или абзац, если wrap > 0) с мягкой тенью; x — левый край, середина
// или правый край по align, y — верх. Возвращает высоту текста.
float text(ImFont* font, float size, float x, float y, Align align, ImVec4 color, std::string_view line,
           float wrap = 0.0f);
// Высота текста без рисования.
float measure(ImFont* font, float size, std::string_view line, float wrap = 0.0f);

// Убрать номера из подписей: «John Miller #12» → «John Miller», «обокрал дом
// #812» → «обокрал дом»: зритель знает людей по именам, а не по номерам.
std::string clean(std::string_view line);

// Плавное появление и уход: 0 → 1 за fadeIn секунд с начала, 1 → 0 за
// fadeOut секунд до конца length (length ≤ 0 — без конца).
float fade(float age, float fadeIn, float length = 0.0f, float fadeOut = 0.0f);

} // namespace frame
} // namespace lw::ui
