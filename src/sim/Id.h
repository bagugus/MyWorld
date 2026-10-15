#pragma once

#include <cstdint>

namespace lw {

// Стабильный идентификатор объекта мира: уникален за всю жизнь мира и никогда
// не переиспользуется (в отличие от entt::entity). Летопись, память существ и
// инспектор ссылаются только на него — запись переживает смерть сущности.
using EntityId = std::uint64_t;

inline constexpr EntityId kInvalidId = 0;

// Компонент: есть у каждой сущности, созданной через World::create().
struct Id {
    EntityId value = kInvalidId;
};

} // namespace lw
