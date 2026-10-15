#include "ui/ChronicleView.h"

#include "sim/Calendar.h"
#include "sim/World.h"
#include "ui/Format.h"
#include "ui/Inspector.h"

#include <imgui.h>

#include <cstdio>

namespace lw::ui {

namespace {

// Текст события: глагол перед объектом и подробности после него. Объект
// (если есть) — ссылка между ними.
struct Phrase {
    const char* verb = "";
    bool withObject = false;
    char tail[64] = "";
};

Phrase phraseOf(const ChronicleEvent& event) {
    Phrase phrase;
    switch (event.kind) {
    case ChronicleKind::Settled:
        phrase.verb = event.object != kInvalidId ? "поселился в доме" : "появился в мире без дома";
        phrase.withObject = event.object != kInvalidId;
        break;
    case ChronicleKind::Scavenged:
        phrase.verb = "обыскал здание";
        phrase.withObject = true;
        if (event.a == 0 && event.b == 0) {
            std::snprintf(phrase.tail, sizeof(phrase.tail), "— пусто");
        } else {
            std::snprintf(phrase.tail, sizeof(phrase.tail), "— еды %d, воды %d", event.a, event.b);
        }
        break;
    case ChronicleKind::BroughtHome:
        phrase.verb = "принёс домой";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "еды %d, воды %d", event.a, event.b);
        break;
    case ChronicleKind::AteLastFood:
        phrase.verb = "доел последнюю еду дома";
        phrase.withObject = true;
        break;
    case ChronicleKind::DrankLastWater:
        phrase.verb = "допил последнюю воду дома";
        phrase.withObject = true;
        break;
    case ChronicleKind::SleptOutside:
        phrase.verb = "заночевал вне дома";
        break;
    case ChronicleKind::Starving:
        phrase.verb = "голодает";
        break;
    case ChronicleKind::Dehydrated:
        phrase.verb = "мучается от жажды";
        break;
    case ChronicleKind::Exhausted:
        phrase.verb = "выбился из сил";
        break;
    case ChronicleKind::Weakened:
        phrase.verb = "ослаб";
        break;
    case ChronicleKind::Dying:
        phrase.verb = "при смерти";
        break;
    case ChronicleKind::Recovered:
        phrase.verb = "оправился";
        break;
    case ChronicleKind::Fled:
        phrase.verb = event.a != 0 ? "проснулся и бежал от зомби" : "бежал от зомби";
        phrase.withObject = event.object != kInvalidId;
        break;
    case ChronicleKind::HordeFormed:
        phrase.verb = "— орда собралась:";
        std::snprintf(phrase.tail, sizeof(phrase.tail), "%d зомби%s", event.a,
                      event.b != 0 ? " сбежались на шум" : "");
        break;
    case ChronicleKind::HordeMerged:
        phrase.verb = "— орда поглотила орду";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— теперь %d зомби", event.a);
        break;
    case ChronicleKind::HordeArrived:
        phrase.verb = "— орда пришла в поселение";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— %d зомби", event.a);
        break;
    case ChronicleKind::HordeScattered:
        phrase.verb = "— орда распалась";
        break;
    case ChronicleKind::Bitten:
        phrase.verb = "укушен зомби";
        phrase.withObject = true;
        break;
    case ChronicleKind::KilledZombie:
        phrase.verb = "упокоил зомби";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "(%s)",
                      weaponName(static_cast<WeaponKind>(event.a)));
        break;
    case ChronicleKind::Hunted:
        phrase.verb = "добыл";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "(%s)",
                      animalKindName(static_cast<AnimalKind>(event.a)));
        break;
    case ChronicleKind::Died:
        if (event.object == kInvalidId) {
            phrase.verb = "погиб:";
            std::snprintf(phrase.tail, sizeof(phrase.tail), "%s",
                          deathCauseName(static_cast<DeathCause>(event.a)));
        } else {
            phrase.verb = static_cast<DeathCause>(event.a) == DeathCause::Bleeding
                              ? "истёк кровью после нападения"
                              : "погиб в схватке с";
            phrase.withObject = true;
        }
        break;
    case ChronicleKind::Butchered:
        phrase.verb = "разделал тушу";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— мяса %d", event.a);
        break;
    case ChronicleKind::Count:
        break;
    }
    return phrase;
}

} // namespace

EntityId drawChronicleEvent(const ChronicleEvent& event, EntityId self) {
    EntityId clicked = kInvalidId;
    const CalendarTime time = calendarAt(event.tick);
    ImGui::TextDisabled("день %d, %02d:%02d", time.day, time.hour, time.minute);

    const Phrase phrase = phraseOf(event);
    if (event.subject != self) {
        ImGui::SameLine();
        if (idLink(event.subject)) {
            clicked = event.subject;
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(phrase.verb);
    // Объект — тот, чью биографию смотрят: ссылка на него самого не нужна.
    if (phrase.withObject && event.object != self) {
        ImGui::SameLine();
        if (idLink(event.object)) {
            clicked = event.object;
        }
    }
    if (phrase.tail[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextUnformatted(phrase.tail);
    }
    return clicked;
}

void drawChroniclePanel(const World& world, Inspector& inspector) {
    // Внизу посередине: по бокам — время, инспектор и статистика.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y - 12.0f}, ImGuiCond_FirstUseEver,
                            {0.5f, 1.0f});
    ImGui::SetNextWindowSize({620.0f, 240.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Летопись###chronicle")) {
        const auto& events = world.chronicle().events();
        ImGui::Text("Событий: %zu", events.size());
        ImGui::Separator();
        if (ImGui::BeginChild("events")) {
            EntityId clicked = kInvalidId;
            // Новые сверху; рисуются только видимые строки.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(events.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const std::size_t index = events.size() - 1 - static_cast<std::size_t>(row);
                    ImGui::PushID(static_cast<int>(index));
                    const EntityId id = drawChronicleEvent(events[index], kInvalidId);
                    ImGui::PopID();
                    if (id != kInvalidId) {
                        clicked = id;
                    }
                }
            }
            if (clicked != kInvalidId) {
                inspector.select(clicked);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace lw::ui
