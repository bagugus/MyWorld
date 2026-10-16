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
// (если есть) — ссылка между ними; за ним — связка и третий участник (этап 6.3).
struct Phrase {
    const char* verb = "";
    bool withObject = false;
    const char* middle = "";
    bool withThird = false;
    // Третий участник — тот, чью биографию смотрят: вместо ссылки на него самого.
    const char* thirdSelf = "его";
    char tail[64] = "";
};

// Поступок для «видел, как X …» и «узнал, что X …» (MemoryKind).
const char* deedVerb(std::int32_t kind) {
    switch (static_cast<MemoryKind>(kind)) {
    case MemoryKind::Saved:
        return "спас";
    case MemoryKind::Defended:
        return "заступился за";
    case MemoryKind::Abandoned:
        return "бросил в беде";
    case MemoryKind::Robbed:
        return "обокрал";
    case MemoryKind::Expelled:
        return "изгнал из группы";
    case MemoryKind::Helped:
        return "помог";
    case MemoryKind::Betrayed:
        return "предал";
    case MemoryKind::Assaulted:
        return "напал на";
    case MemoryKind::Killed:
        return "убил";
    default:
        break;
    }
    return "что-то сделал с";
}

// «Узнал, что его …» — о поступке с ним самим (MemoryKind).
const char* heardVerb(std::int32_t kind) {
    switch (static_cast<MemoryKind>(kind)) {
    case MemoryKind::Saved:
        return "узнал, что его спас";
    case MemoryKind::Defended:
        return "узнал, что за него заступился";
    case MemoryKind::Abandoned:
        return "узнал, что его бросил в беде";
    case MemoryKind::Robbed:
        return "узнал, что его обокрал";
    case MemoryKind::Expelled:
        return "узнал, что его изгнал";
    case MemoryKind::Helped:
        return "узнал, что ему помог";
    case MemoryKind::Betrayed:
        return "узнал, что его предал";
    case MemoryKind::Assaulted:
        return "узнал, что на него напал";
    default:
        break;
    }
    return "узнал о поступке";
}

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
        if (event.b == 1) { // от человека (этап 6.5)
            phrase.verb = event.a != 0 ? "проснулся и бежал от" : "бежал от";
        } else {
            phrase.verb = event.a != 0 ? "проснулся и бежал от зомби" : "бежал от зомби";
        }
        phrase.withObject = event.object != kInvalidId;
        break;
    case ChronicleKind::HordeFormed:
        phrase.verb = "— орда собралась:";
        std::snprintf(phrase.tail, sizeof(phrase.tail), "%d зомби%s", event.a,
                      event.b == 1   ? " сбежались на шум"
                      : event.b == 2 ? " пришли издалека"
                                     : "");
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
            switch (static_cast<DeathCause>(event.a)) {
            case DeathCause::Bleeding:
                phrase.verb = "истёк кровью после нападения";
                break;
            case DeathCause::Infection:
                phrase.verb = "сгорел от заражения после укуса";
                break;
            default:
                phrase.verb = "погиб в схватке с";
                break;
            }
            phrase.withObject = true;
        }
        break;
    case ChronicleKind::Turned:
        phrase.verb = "встал зомби";
        break;
    case ChronicleKind::Feverish:
        phrase.verb = "слёг в лихорадке от укуса";
        break;
    case ChronicleKind::FoundWeapon:
        phrase.verb = "нашёл оружие:";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— %s",
                      weaponName(static_cast<WeaponKind>(event.a)));
        break;
    case ChronicleKind::LootedBody:
        phrase.verb = "обыскал тело";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— еды %d, воды %d", event.a, event.b);
        break;
    case ChronicleKind::Treated:
        phrase.verb = event.a == 0 ? "перевязал рану" : "принял лекарство";
        break;
    case ChronicleKind::Butchered:
        phrase.verb = "разделал тушу";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— мяса %d", event.a);
        break;
    case ChronicleKind::Befriended:
        phrase.verb = "подружился с";
        phrase.withObject = true;
        break;
    case ChronicleKind::Disliked:
        phrase.verb = "невзлюбил";
        phrase.withObject = true;
        break;
    case ChronicleKind::SavedBy:
        phrase.verb = "обязан жизнью";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— тот убил напавшего зомби");
        break;
    case ChronicleKind::DefendedBy:
        phrase.verb = "благодарен";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— заступился в схватке с зомби");
        break;
    case ChronicleKind::AbandonedBy:
        phrase.verb = "не простит";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— убежал, бросив его зомби");
        break;
    case ChronicleKind::CaughtThief:
        phrase.verb = "застал за обыском своего дома";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— взято вещей: %d", event.a);
        break;
    case ChronicleKind::Mourned:
        phrase.verb = event.a == 1 ? "нашёл тело близкого" : "узнал о гибели близкого";
        phrase.withObject = true;
        if (event.third != kInvalidId) {
            phrase.middle = "— рассказал";
            phrase.withThird = true;
            phrase.thirdSelf = "он";
        }
        break;
    case ChronicleKind::SawTurned:
        phrase.verb = "узнал в зомби близкого";
        phrase.withObject = true;
        break;
    case ChronicleKind::Witnessed:
        phrase.verb = "видел, как";
        phrase.withObject = true;
        phrase.middle = deedVerb(event.a);
        phrase.withThird = true;
        break;
    case ChronicleKind::HeardOf:
        phrase.verb = heardVerb(event.a);
        phrase.withObject = true;
        phrase.middle = "— рассказал";
        phrase.withThird = true;
        phrase.thirdSelf = "он";
        break;
    case ChronicleKind::Reconciled:
        phrase.verb = "помирился с";
        phrase.withObject = true;
        break;
    case ChronicleKind::GroupFormed:
        phrase.verb = "собрал группу";
        phrase.withObject = true;
        break;
    case ChronicleKind::JoinedGroup:
        phrase.verb = "вступил в группу";
        phrase.withObject = true;
        if (event.a == 1) {
            std::snprintf(phrase.tail, sizeof(phrase.tail), "и переехал на её базу");
        }
        break;
    case ChronicleKind::LeftGroup:
        phrase.verb = "ушёл из группы";
        phrase.withObject = true;
        if (event.third != kInvalidId) {
            phrase.middle = "— не поладил с вожаком";
            phrase.withThird = true;
        }
        break;
    case ChronicleKind::Expelled:
        phrase.verb = "изгнал";
        phrase.withObject = true;
        phrase.middle = "из группы";
        phrase.withThird = true;
        break;
    case ChronicleKind::LedGroup:
        phrase.verb = "возглавил группу";
        phrase.withObject = true;
        if (event.third != kInvalidId) {
            phrase.middle = event.a == 1   ? "— сместил"
                            : event.a == 2 ? "— после того, как сбежал"
                                           : "— после гибели";
            phrase.withThird = true;
        }
        break;
    case ChronicleKind::GroupDisbanded:
        phrase.verb = event.object != kInvalidId ? "распалась, остался" : "распалась";
        phrase.withObject = event.object != kInvalidId;
        break;
    case ChronicleKind::Shared:
        phrase.verb = "поделился с";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— %s, %d%s",
                      itemName(static_cast<ItemKind>(event.a & 0xFF)), event.b,
                      (event.a & 256) != 0 ? "; вернул долг" : "");
        break;
    case ChronicleKind::Traded:
        phrase.verb = "обменялся с";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— получил %s, %d; отдал %s, %d",
                      itemName(static_cast<ItemKind>(event.a & 0xFF)), event.a >> 8,
                      itemName(static_cast<ItemKind>(event.b & 0xFF)), event.b >> 8);
        break;
    case ChronicleKind::Stole:
        phrase.verb = "обокрал дом";
        phrase.withObject = true;
        if (event.third != kInvalidId) {
            phrase.middle = "у";
            phrase.withThird = true;
            phrase.thirdSelf = "него";
        }
        std::snprintf(phrase.tail, sizeof(phrase.tail), "— еды %d, воды %d", event.a, event.b);
        break;
    case ChronicleKind::Deserted:
        phrase.verb = "сбежал из группы";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "с её запасами — вещей %d", event.a);
        break;
    case ChronicleKind::BetrayedBy:
        phrase.verb = "его предал";
        phrase.withObject = true;
        if (event.a > 0) {
            std::snprintf(phrase.tail, sizeof(phrase.tail), "— вещей %d", event.a);
        }
        break;
    case ChronicleKind::AssaultedBy:
        phrase.verb = "на него напал";
        phrase.withObject = true;
        break;
    case ChronicleKind::Avenged:
        phrase.verb = "отомстил";
        phrase.withObject = true;
        std::snprintf(phrase.tail, sizeof(phrase.tail), "%s", event.a == 1 ? "— убил" : "— избил");
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
    if (phrase.middle[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextUnformatted(phrase.middle);
    }
    if (phrase.withThird && event.third != kInvalidId) {
        ImGui::SameLine();
        if (event.third == self) {
            ImGui::TextUnformatted(phrase.thirdSelf);
        } else if (idLink(event.third)) {
            clicked = event.third;
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
