#pragma once

#include "sim/Foresight.h"
#include "sim/Id.h"
#include "sim/Story.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lw {

class World;

namespace ui {

class Inspector;

// Режиссёр (этап 12а): собирает сюжетные линии из летописи (StoryTracker) и,
// если включён, сам ведёт камеру — выбирает в инспекторе героя самой
// интересной линии и следит за ним. Пока только вид сверху: камера движется
// через слежение инспектора (CameraController::moveTo).
//
// Предвкушение (12а.2): режиссёр смотрит и на то, что вот-вот случится
// (foresee) — мститель идёт к обидчику, орда к деревне, двое к одному грузу.
// Предвестие поднимает интересность своей линии (или само становится
// кандидатом, если линии нет), а когда до кульминации остаётся несколько
// реальных секунд — перехватывает камеру, не дожидаясь срока, и держит её,
// пока всё не случится.
//
// Мир режиссёр только читает. Выбор пользователя (щелчок по другому объекту)
// выключает режиссёра.
class Director {
public:
    explicit Director(const World& world);

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    // Раз за кадр после шага мира, до Inspector::updateCamera: дочитать
    // летопись, при надобности сменить линию и героя. dt — реальные секунды.
    void update(Inspector& inspector, float dt);
    // Окно «Режиссёр»: включение, текущая линия, список линий, что вот-вот
    // случится, события выбранной линии.
    void drawPanel(Inspector& inspector);
    // Подпись в кадре сверху: какая линия, кто в ней, последнее событие и что
    // вот-вот случится.
    void drawCaption(Inspector& inspector);

private:
    // Кандидат на кадр: линия (с предвестием или без) или предвестие без линии.
    struct Pick {
        std::uint32_t story = 0; // Story::id; 0 — линии нет
        bool hasOmen = false;
        Foresight omen;          // предвестие, если hasOmen
        EntityId target = kInvalidId;
        float interest = 0.0f;

        bool valid() const { return story != 0 || hasOmen; }
        // Тот же кандидат: та же линия, а без линии — то же предвестие.
        bool same(const Pick& other) const;
    };

    // За кем следить в линии: герой, иначе то, о чём она (группа, ивент);
    // kInvalidId — ни того, ни другого уже нет в мире.
    EntityId targetOf(const Story& story) const;
    bool exists(EntityId id) const;
    // Кандидаты по убыванию интересности.
    std::vector<Pick> candidates() const;
    // Кандидат для линии или предвестия, выбранных в окне.
    Pick pickOf(const Story& story) const;
    Pick pickOf(const Foresight& omen) const;
    // Через сколько реальных секунд кульминация предвестия при нынешней
    // скорости времени (на паузе — очень нескоро).
    float realEta(const Foresight& omen) const;
    bool urgent(const Pick& pick) const;
    // Перевести камеру на кандидата.
    void show(const Pick& pick, Inspector& inspector);
    // «John Miller идёт мстить Peter Green — 120 м, ~2 мин».
    std::string omenLine(const Foresight& omen) const;

    const World& world_;
    StoryTracker stories_;
    std::vector<Foresight> omens_; // что вот-вот случится, по убыванию интересности
    bool enabled_ = false;
    Pick current_;               // кого ведёт режиссёр
    // Кандидат пропал из списка (предвестие без линии сбылось): режиссёр
    // досматривает, что вышло, а held_ считает с этого мига.
    bool lost_ = false;
    EntityId shown_ = kInvalidId; // кого он выбрал в инспекторе
    float held_ = 0.0f;          // секунд на этом кандидате (или с его кульминации)
    float sinceChoice_ = 0.0f;   // секунд с прошлого пересмотра
    // Секунд симуляции на реальную секунду (сглажено): x1 — 1, пауза — 0.
    float simRate_ = 1.0f;
    std::uint64_t lastTick_ = 0;
    std::uint32_t opened_ = 0;   // линия, открытая в окне
};

} // namespace ui
} // namespace lw
