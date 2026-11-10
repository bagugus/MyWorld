#pragma once

namespace lw {
class CameraController;
class World;
} // namespace lw

namespace lw::ui {

// Слой «детализация симуляции» (этап 6а) в виде сверху: районы карты
// активности — где полная (красным) и спокойная (жёлтым), дремлющая без
// заливки, — и кольцо уровня у каждого существа вблизи. Только читает мир.
// Рисовать между beginOverlay и endOverlay; в 3D не рисуется.
void drawLodOverlay(const World& world, const CameraController& camera);

// Включён ли слой: флажок в панели «Статистика», --lod-map.
bool lodOverlay();
void setLodOverlay(bool enabled);

} // namespace lw::ui
