#pragma once

#include "sim/Id.h"

#include <cstdint>

namespace lw {

// Что слышно. Значения попадают в хеш и сохранения: новые добавлять только в
// конец (перед Count).
enum class SoundKind : std::uint8_t {
    None,
    Footsteps, // шаги идущего
    Rummage,   // обыск здания: двери, шкафы, битое стекло
    Voice,     // разговор, окрик
    Count
};

// Разовый звук мира: выстрел, крик, взрыв — то, что не следует из состояния
// существа (шаги, обыск, разговор слышны и так, см. sim/Perception.h). Звук
// живёт kSoundLifetimeTicks (sim/Perception.h): этого хватает, чтобы каждый
// слушатель услышал его ровно при одном своём обновлении восприятия.
struct Sound {
    std::uint64_t tick = 0;  // когда прозвучал
    float x = 0.0f;          // где, метры
    float y = 0.0f;
    float loudness = 0.0f;   // слышен в стольких метрах в тихую погоду
    EntityId source = kInvalidId; // кто издал; kInvalidId — никто (гром)
    SoundKind kind = SoundKind::None;
};

} // namespace lw
