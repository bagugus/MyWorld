#pragma once

#include "sim/Id.h"
#include "sim/Story.h"

#include <cstdint>

namespace lw {

class World;

namespace ui {

class Inspector;

// Режиссёр (этап 12а): собирает сюжетные линии из летописи (StoryTracker) и,
// если включён, сам ведёт камеру — выбирает в инспекторе героя самой
// интересной линии и следит за ним. Пока только вид сверху: камера движется
// через слежение инспектора (CameraController::moveTo).
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
    // Окно «Режиссёр»: включение, текущая линия, список линий, события
    // выбранной линии.
    void drawPanel(Inspector& inspector);
    // Подпись в кадре сверху: какая линия, кто в ней и последнее событие.
    void drawCaption(Inspector& inspector);

private:
    // За кем следить в линии: герой, иначе то, о чём она (группа, ивент);
    // kInvalidId — ни того, ни другого уже нет в мире.
    EntityId targetOf(const Story& story) const;
    // Перевести камеру на линию.
    void show(const Story& story, Inspector& inspector);

    const World& world_;
    StoryTracker stories_;
    bool enabled_ = false;
    std::uint32_t current_ = 0; // линия, которую ведёт режиссёр (Story::id)
    EntityId shown_ = kInvalidId; // кого он выбрал в инспекторе
    float held_ = 0.0f;           // секунд на этой линии
    float sinceChoice_ = 0.0f;    // секунд с прошлого пересмотра линий
    std::uint32_t opened_ = 0;    // линия, открытая в окне
};

} // namespace ui
} // namespace lw
