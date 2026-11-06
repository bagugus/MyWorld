#pragma once

#include "render/Interior.h"
#include "render/Poses.h"
#include "sim/Id.h"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lw {

class World;

// Как выглядит существо в этом кадре (этап 12а.11): куда обращено, поза,
// что в руках, значок эмоции. Только облик, в симуляцию не входит.
struct Figure {
    enum class Body : std::uint8_t { Human, Zombie, Animal, HumanCorpse, AnimalCorpse };
    Body body = Body::Human;
    float heading = 0.0f;  // куда обращено: atan2(dx, dy), 0 — на юг
    // Сдвиг от позиции симуляции, метры: прислонился к стене, подошёл к
    // воде, к двери, к телу. Плавный и не больше пары метров.
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    Gait gait;
    HumanPose human;
    AnimalPose animal;
    Prop prop = Prop::None;
    bool bag = false;    // сумка на боку (собирает в лесу)
    bool aiming = false; // оружие в руках, а не за спиной
    float fall = 1.0f;   // туша: 0 — ещё стоит, 1 — лежит на боку
    Emote emote = Emote::None;
    float emoteAge = 0.0f; // секунды с появления значка
};

// Аниматор: для каждого существа выбирает занятие по состоянию мира —
// действию Utility AI, профессии, бою, ранам, страху, горю, состоянию зомби
// и зверя, — вариант по id и чертам характера, смену позы раз в несколько
// секунд и плавные переходы. Удары и выстрелы узнаёт по Combat::ready,
// падение и подъём зомби — по переходу «живой → тело → зомби», значки
// эмоций — по свежим событиям летописи. Только читает мир; часы анимации
// идут, пока идёт симуляция (на паузе всё замирает). Без графики — её
// рисует render/Models3D.
class Animator {
public:
    static constexpr float kEmoteSeconds = 4.0f;

    // Раз в кадр до animate: часы, новые события летописи. alpha — доля тика.
    void beginFrame(const World& world, float dt, float alpha);
    // Облик существа в этом кадре: повторный вызов в том же кадре (тень и
    // кадр) возвращает уже посчитанный. x, y — где оно рисуется, vx, vy —
    // скорость симуляции, м/с. indoor — место в комнате (12а.14): человек дома
    // сидит на стуле, лежит в кровати, стоит у плиты — лицом, куда велит место.
    const Figure& animate(const World& world, entt::entity entity, EntityId id, float x, float y, float vx,
                          float vy, const Placement* indoor = nullptr);
    // Куда обращено существо, если оно рисовалось в прошлом кадре.
    std::optional<float> heading(EntityId id) const;
    // Секунды часов анимации.
    float clock() const { return clock_; }
    // Забыть тех, кого давно нет в кадре.
    void forgetStale();

private:
    struct Choice;
    struct State;
    struct Spot {
        float x = 0.0f;
        float y = 0.0f;
        EntityId id = kInvalidId;
        bool human = false;
    };
    struct Rect {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        EntityId id = kInvalidId;
    };
    struct Mark {
        Emote emote = Emote::None;
        float at = 0.0f;
    };

    void readChronicle(const World& world);
    void indexBuildings(const World& world);
    void gatherFrame(const World& world);
    std::optional<Rect> buildingRect(const World& world, EntityId building) const;
    std::optional<Rect> wallNear(const World& world, float x, float y) const;

    void chooseHuman(const World& world, entt::entity entity, State& state, float x, float y, float speed,
                     Choice& choice, const Placement* indoor);
    void chooseZombie(const World& world, entt::entity entity, State& state, float x, float y, float speed,
                      Choice& choice);
    void chooseAnimal(const World& world, entt::entity entity, State& state, float x, float y, float speed,
                      Choice& choice);

    std::unordered_map<EntityId, std::unique_ptr<State>> states_;
    std::unordered_map<EntityId, Mark> marks_;
    std::unordered_map<std::int64_t, EntityId> buildingCells_; // клетка → здание
    bool indexed_ = false;
    std::vector<Spot> corpses_;                 // тела в этом кадре
    std::unordered_set<EntityId> occupied_;     // здания, где кто-то внутри
    std::uint32_t gathered_ = 0;                // кадр, когда собраны
    std::size_t chronicleRead_ = 0;
    std::uint32_t frame_ = 0;
    float clock_ = 0.0f;
    float dt_ = 0.0f;   // часы анимации за кадр
    float rate_ = 1.0f; // во сколько раз симуляция быстрее реального времени
    double lastSim_ = -1.0;
    std::uint64_t tick_ = 0;
    float yearFraction_ = 0.0f;

public:
    Animator();
    Animator(const Animator&) = delete;
    Animator& operator=(const Animator&) = delete;
    ~Animator();
};

} // namespace lw
