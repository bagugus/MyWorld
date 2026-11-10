#pragma once

#include "render/CameraController.h"
#include "sim/Id.h"

#include <raylib.h>

#include <entt/entt.hpp>

#include <optional>

namespace lw {

class CameraController;
class World;

namespace ui {

// Инспектор: ЛКМ по существу, стае птиц или зданию выбирает его, в окне — его
// компоненты, текущее действие, связанные объекты (ссылки «#id» переключают
// выбор) и биография из летописи.
// Выбор хранится по EntityId: исчезнет объект — выбор просто станет пустым.
// Мир инспектор только читает.
class Inspector {
public:
    explicit Inspector(const World& world);

    void select(EntityId id);
    EntityId selected() const { return selected_; }
    void setFollow(bool follow) { follow_ = follow; }

    // ЛКМ без Ctrl (Ctrl+ЛКМ — у инструмента пути): выбрать то, что под курсором,
    // щелчок по пустому месту снимает выбор.
    void handleInput(const CameraController& camera, bool mouseFree);
    // Камера следует за выбранным, если включено слежение. alpha — доля тика
    // (SimDriver::interpolationAlpha), чтобы камера шла вровень с отрисовкой.
    void updateCamera(CameraController& camera, float alpha);
    // Выбранное существо (или тело) для вида из-за плеча: где оно на экране
    // (как у отрисовки), курс по скорости и высота глаз; нет — если выбрано не оно.
    std::optional<CameraSubject> cameraSubject(float alpha) const;
    // Подсветка выбранного, его путь и цель; между BeginMode2D и EndMode2D.
    void drawOverlay(const Camera2D& camera, float alpha) const;
    void drawPanel();

private:
    // Существо или стая под точкой (с запасом в несколько пикселей), иначе
    // здание; kInvalidId — ничего.
    EntityId pick(float x, float y, float zoom) const;
    void drawPerson(entt::entity entity);
    void drawZombie(entt::entity entity);
    void drawHorde(entt::entity entity);
    void drawGroup(entt::entity entity);
    void drawAnimal(entt::entity entity);
    void drawFlock(entt::entity entity);
    void drawCorpse(entt::entity entity);
    // Ивент и его груз (этап 7.1).
    void drawEvent(entt::entity entity);
    void drawCache(entt::entity entity);
    // Сценарий и фазы мира (этап 7.4).
    void drawScenario(entt::entity entity);
    // Позиция, скорость, оставшийся путь.
    void drawMovement(entt::entity entity);
    // Дальность зрения и слуха, кого существо заметило. false — щёлкнули по
    // ссылке и выбор сменился.
    bool drawPerception(entt::entity entity);
    // Уровень детализации симуляции (этап 6а): какой, почему, как часто
    // существо смотрит и решает.
    void drawDetail(entt::entity entity);
    // Кого человек знает и как к нему относится. false — щёлкнули по ссылке.
    bool drawRelations(entt::entity entity);
    // Семья (этап 7.3): родня, опекун; false — выбран другой объект.
    bool drawFamily(entt::entity entity);
    // Что человек помнит о других. false — щёлкнули по ссылке.
    bool drawMemories(entt::entity entity);
    // Что человек знает об ивентах. false — щёлкнули по ссылке.
    bool drawLeads(entt::entity entity);
    // Тревога: что человек знает о беде (этап 7.6). false — щёлкнули по ссылке.
    bool drawAlarm(entt::entity entity);
    // Уехал из края с колонной эвакуации (этап 7.6).
    void drawEvacuated(entt::entity entity);
    void drawBuilding(entt::entity entity);
    void drawSettlement(entt::entity entity);
    // События летописи с участием выбранного; есть и у исчезнувших объектов.
    void drawBiography();

    const World& world_;
    EntityId selected_ = kInvalidId;
    bool follow_ = false;
    // Поле «перейти к id».
    int goToId_ = 0;
};

} // namespace ui
} // namespace lw
