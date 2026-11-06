#include "ui/SeriesFrame.h"

#include <algorithm>
#include <cctype>
#include <cfloat>

namespace lw::ui::frame {

namespace {

constexpr float kAspect = 2.35f;     // широкий кадр
constexpr float kMinBarShare = 0.09f; // полоса не уже такой доли высоты экрана
constexpr float kShadowAlpha = 0.75f;

ImDrawList* canvas() {
    return ImGui::GetForegroundDrawList();
}

ImU32 colorOf(ImVec4 color, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, color.w * alpha});
}

} // namespace

float scale() {
    return ImGui::GetIO().DisplaySize.y / 900.0f;
}

float barHeight(float shown) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float bar = std::max((display.y - display.x / kAspect) * 0.5f, display.y * kMinBarShare);
    return bar * std::clamp(shown, 0.0f, 1.0f);
}

void drawBars(float shown) {
    const float bar = barHeight(shown);
    if (bar <= 0.5f) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImU32 black = IM_COL32(0, 0, 0, 255);
    canvas()->AddRectFilled({0.0f, 0.0f}, {display.x, bar}, black);
    canvas()->AddRectFilled({0.0f, display.y - bar}, display, black);
}

void drawBlackout(float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    canvas()->AddRectFilled({0.0f, 0.0f}, display, colorOf({0.0f, 0.0f, 0.0f, 1.0f}, std::min(alpha, 1.0f)));
}

float measure(ImFont* font, float size, std::string_view line, float wrap) {
    if (font == nullptr) {
        font = ImGui::GetFont();
    }
    const ImVec2 extent =
        font->CalcTextSizeA(size, FLT_MAX, wrap, line.data(), line.data() + line.size());
    return extent.y;
}

float text(ImFont* font, float size, float x, float y, Align align, ImVec4 color, std::string_view line,
           float wrap) {
    if (line.empty() || color.w <= 0.0f) {
        return 0.0f;
    }
    if (font == nullptr) {
        font = ImGui::GetFont();
    }
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, wrap, line.data(), line.data() + line.size());
    float left = x;
    if (align == Align::Center) {
        left = x - (wrap > 0.0f ? std::min(wrap, extent.x) : extent.x) * 0.5f;
    } else if (align == Align::Right) {
        left = x - extent.x;
    }
    // Мягкая тень — на любом фоне читается.
    const float offset = std::max(1.0f, size * 0.06f);
    const ImU32 shadow = colorOf({0.0f, 0.0f, 0.0f, kShadowAlpha}, color.w);
    canvas()->AddText(font, size, {left + offset, y + offset}, shadow, line.data(), line.data() + line.size(),
                      wrap);
    canvas()->AddText(font, size, {left, y}, colorOf(color), line.data(), line.data() + line.size(), wrap);
    return extent.y;
}

std::string clean(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        // « #123» сразу после слова — убрать вместе с пробелом.
        if (line[i] == ' ' && i + 2 < line.size() && line[i + 1] == '#' &&
            std::isdigit(static_cast<unsigned char>(line[i + 2])) && i > 0 &&
            (std::isalpha(static_cast<unsigned char>(line[i - 1])) ||
             static_cast<unsigned char>(line[i - 1]) >= 0x80)) {
            std::size_t j = i + 2;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                ++j;
            }
            i = j - 1;
            continue;
        }
        out += line[i];
    }
    return out;
}

float fade(float age, float fadeIn, float length, float fadeOut) {
    float alpha = fadeIn > 0.0f ? std::clamp(age / fadeIn, 0.0f, 1.0f) : 1.0f;
    if (length > 0.0f && fadeOut > 0.0f) {
        alpha = std::min(alpha, std::clamp((length - age) / fadeOut, 0.0f, 1.0f));
    } else if (length > 0.0f && age > length) {
        alpha = 0.0f;
    }
    // Плавнее линейного.
    return alpha * alpha * (3.0f - 2.0f * alpha);
}

} // namespace lw::ui::frame
