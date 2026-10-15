#include "ui/Inspector.h"

#include "render/CameraController.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/Perception.h"
#include "sim/TimeScale.h"
#include "sim/Weapons.h"
#include "sim/World.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/ResourceSystem.h"
#include "sim/systems/UtilityAiSystem.h"
#include "sim/systems/ZombieSystem.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace lw::ui {

namespace {

constexpr float kPickPixels = 8.0f;
constexpr Color kSelectionColor = {255, 255, 255, 255};
constexpr Color kPathColor = {255, 236, 140, 220};
constexpr Color kTargetColor = {255, 120, 90, 255};
constexpr Color kHomeColor = {120, 220, 255, 255};
// Восприятие: дальность зрения и слуха, линии к замеченным.
constexpr Color kSightColor = {170, 255, 170, 70};
constexpr Color kHearingColor = {255, 220, 120, 70};
constexpr Color kSeenColor = {120, 255, 120, 220};
constexpr Color kHeardColor = {255, 210, 90, 220};
constexpr Color kRememberedColor = {170, 170, 170, 160};
// Орда: участники и вожак.
constexpr Color kHordeColor = {214, 96, 80, 200};
// Опасные места, которые помнит человек.
constexpr Color kDangerColor = {230, 70, 60, 110};
constexpr Color kLeaderColor = {255, 170, 120, 255};

// Позиция, в которой сущность сейчас нарисована: между началом тика и текущей
// (как в WorldRenderer).
Vector2 drawnPosition(const entt::registry& registry, entt::entity entity, float alpha) {
    const Position& position = registry.get<Position>(entity);
    Vector2 at = {position.x, position.y};
    if (const auto* velocity = registry.try_get<Velocity>(entity)) {
        const float back = (1.0f - std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
        at.x -= velocity->x * back;
        at.y -= velocity->y * back;
    }
    return at;
}

Vector2 cellCenter(Cell cell) {
    return {Map::cellCenter(cell.x), Map::cellCenter(cell.y)};
}

Rectangle buildingRect(const Building& building) {
    return {static_cast<float>(building.origin.x) * Map::kCellSize,
            static_cast<float>(building.origin.y) * Map::kCellSize,
            static_cast<float>(building.width) * Map::kCellSize,
            static_cast<float>(building.height) * Map::kCellSize};
}

bool contains(const Building& building, Cell cell) {
    return cell.x >= building.origin.x && cell.y >= building.origin.y &&
           cell.x < building.origin.x + building.width &&
           cell.y < building.origin.y + building.height;
}

EntityId idOf(const entt::registry& registry, entt::entity entity) {
    return registry.get<Id>(entity).value;
}

void drawSupplies(const entt::registry& registry, entt::entity building) {
    if (const auto* supplies = registry.try_get<Supplies>(building)) {
        ImGui::Text("Припасы: еды %d, воды %d", supplies->food, supplies->water);
        if (const auto* info = registry.try_get<Building>(building);
            info != nullptr && rainBarrel(info->kind) > 0) {
            ImGui::Text("Бочка для дождя: до %d порций воды", rainBarrel(info->kind));
        }
    }
}

} // namespace

Inspector::Inspector(const World& world) : world_(world) {}

void Inspector::select(EntityId id) {
    if (id != selected_) {
        follow_ = false;
    }
    selected_ = id;
}

EntityId Inspector::pick(float x, float y, float zoom) const {
    const entt::registry& registry = world_.registry();
    const float radius = std::max(1.0f, kPickPixels / zoom);
    if (const auto person = world_.spatial().nearest(x, y, radius, [&registry](const SpatialHit& hit) {
            return registry.any_of<Person, Zombie, Animal, Flock, Corpse>(hit.entity);
        })) {
        return person->id;
    }
    const Cell cell = Map::cellAt(x, y);
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        if (contains(building, cell)) {
            return id.value;
        }
    }
    return kInvalidId;
}

void Inspector::handleInput(const CameraController& camera, bool mouseFree) {
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!mouseFree || ctrl || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return;
    }
    const Vector2 point = GetScreenToWorld2D(GetMousePosition(), camera.camera());
    select(pick(point.x, point.y, camera.zoom()));
}

void Inspector::updateCamera(CameraController& camera, float alpha) {
    if (!follow_) {
        return;
    }
    const entt::registry& registry = world_.registry();
    const entt::entity entity = world_.find(selected_);
    if (entity == entt::null) {
        follow_ = false;
        return;
    }
    if (registry.all_of<Position>(entity)) {
        const Vector2 at = drawnPosition(registry, entity, alpha);
        camera.lookAt(at.x, at.y, camera.zoom());
    } else if (const auto* building = registry.try_get<Building>(entity)) {
        const Rectangle rect = buildingRect(*building);
        camera.lookAt(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f, camera.zoom());
    } else if (const auto* horde = registry.try_get<Horde>(entity)) {
        // За вожаком: центр орды пересчитывается раз в минуту и прыгает.
        if (const entt::entity leader = world_.find(horde->leader); leader != entt::null) {
            const Vector2 at = drawnPosition(registry, leader, alpha);
            camera.lookAt(at.x, at.y, camera.zoom());
        }
    }
}

void Inspector::drawOverlay(const Camera2D& camera, float alpha) const {
    const entt::registry& registry = world_.registry();
    const entt::entity entity = world_.find(selected_);
    if (entity == entt::null) {
        return;
    }
    const float px = 1.0f / camera.zoom;
    const float line = 2.0f * px;

    if (const auto* building = registry.try_get<Building>(entity)) {
        const Rectangle rect = buildingRect(*building);
        DrawRectangleLinesEx({rect.x - line, rect.y - line, rect.width + 2.0f * line,
                              rect.height + 2.0f * line},
                             line, kSelectionColor);
        DrawCircleV(cellCenter(building->door), std::max(Map::kCellSize * 0.3f, 3.0f * px),
                    kSelectionColor);
        return;
    }
    if (const auto* settlement = registry.try_get<Settlement>(entity)) {
        DrawRing(cellCenter(settlement->center),
                 static_cast<float>(settlement->radius) * Map::kCellSize - line,
                 static_cast<float>(settlement->radius) * Map::kCellSize, 0.0f, 360.0f, 96,
                 kSelectionColor);
        return;
    }
    if (const auto* horde = registry.try_get<Horde>(entity)) {
        // Кольцо у каждого участника, у вожака — ярче; к цели — линия от вожака.
        const float radius = std::max(1.2f, 6.0f * px);
        for (const EntityId member : horde->members) {
            const entt::entity other = world_.find(member);
            if (other == entt::null) {
                continue;
            }
            const bool leader = member == horde->leader;
            const Vector2 at = drawnPosition(registry, other, alpha);
            DrawRing(at, radius, radius + line, 0.0f, 360.0f, 24, leader ? kLeaderColor : kHordeColor);
            if (leader && horde->travelling) {
                DrawLineEx(at, cellCenter(horde->goal), line, kTargetColor);
            }
        }
        return;
    }
    if (!registry.all_of<Position>(entity)) {
        return;
    }

    const Vector2 at = drawnPosition(registry, entity, alpha);
    if (const auto* flock = registry.try_get<Flock>(entity)) {
        if (flock->flying) {
            DrawLineEx(at, {flock->toX, flock->toY}, line, kTargetColor);
        }
        const float radius = std::max(8.0f, 8.0f * px);
        DrawRing(at, radius, radius + line, 0.0f, 360.0f, 32, kSelectionColor);
        return;
    }
    // Восприятие: круги — дальность зрения (идущего по открытому месту) и
    // слышимость шагов, линии — к замеченным: зелёные — видит, жёлтые —
    // слышит, серые — помнит, где видел в последний раз.
    if (const auto* senses = registry.try_get<Senses>(entity)) {
        const SenseConditions conditions = senseConditions(world_);
        const bool asleep = isAsleep(registry, entity);
        const float sight = sightRange(*senses, asleep, conditions);
        if (sight > 0.0f) {
            DrawRing(at, sight - line, sight, 0.0f, 360.0f, 96, kSightColor);
        }
        const float footsteps = kFootstepsLoudness * hearingFactor(*senses, asleep, conditions);
        DrawRing(at, footsteps - line, footsteps, 0.0f, 360.0f, 64, kHearingColor);
    }
    if (const auto* awareness = registry.try_get<Awareness>(entity)) {
        const std::uint64_t tick = world_.tick();
        for (std::size_t i = 0; i < awareness->count; ++i) {
            const Contact& contact = awareness->contacts[i];
            Vector2 to = {contact.x, contact.y};
            Color color = kRememberedColor;
            if (isFresh(contact, tick)) {
                color = contact.seen ? kSeenColor : kHeardColor;
                // Видимого — к тому месту, где он нарисован сейчас.
                if (const entt::entity other = world_.find(contact.id);
                    contact.seen && other != entt::null && registry.all_of<Position>(other)) {
                    to = drawnPosition(registry, other, alpha);
                }
            }
            DrawLineEx(at, to, line, color);
        }
    }
    // Опасные места — красными кругами.
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        for (const Danger& danger : mind->dangers) {
            if (danger.until > world_.tick()) {
                DrawRing(cellCenter(danger.cell), kDangerRadius - line, kDangerRadius, 0.0f, 360.0f,
                         64, kDangerColor);
            }
        }
    }
    // Дом — рамкой, место текущего действия — крестиком, оставшийся путь — линией.
    if (const auto* person = registry.try_get<Person>(entity)) {
        if (const entt::entity home = world_.find(person->home); home != entt::null) {
            DrawRectangleLinesEx(buildingRect(registry.get<Building>(home)), line, kHomeColor);
        }
    }
    if (const auto* walker = registry.try_get<Walker>(entity); walker != nullptr && walker->moving()) {
        Vector2 from = at;
        for (std::size_t i = walker->next; i < walker->waypoints.size(); ++i) {
            const Vector2 to = cellCenter(walker->waypoints[i]);
            DrawLineEx(from, to, line, kPathColor);
            from = to;
        }
    }
    if (const auto* animal = registry.try_get<Animal>(entity)) {
        // Куда идёт — крестиком, родные места — кольцом.
        const Vector2 spot = cellCenter(animal->goal);
        const float arm = std::max(Map::kCellSize * 0.5f, 5.0f * px);
        DrawLineEx({spot.x - arm, spot.y - arm}, {spot.x + arm, spot.y + arm}, line, kTargetColor);
        DrawLineEx({spot.x - arm, spot.y + arm}, {spot.x + arm, spot.y - arm}, line, kTargetColor);
        DrawRing(cellCenter(animal->anchor), arm, arm + line, 0.0f, 360.0f, 24, kHomeColor);
    }
    if (const auto* zombie = registry.try_get<Zombie>(entity)) {
        // Куда идёт — крестиком, родные места — кольцом, вожак орды — линией.
        const Vector2 spot = cellCenter(zombie->goal);
        const float arm = std::max(Map::kCellSize * 0.5f, 5.0f * px);
        DrawLineEx({spot.x - arm, spot.y - arm}, {spot.x + arm, spot.y + arm}, line, kTargetColor);
        DrawLineEx({spot.x - arm, spot.y + arm}, {spot.x + arm, spot.y - arm}, line, kTargetColor);
        if (zombie->horde == kInvalidId) {
            const float anchor = std::max(Map::kCellSize * 0.5f, 5.0f * px);
            DrawRing(cellCenter(zombie->anchor), anchor, anchor + line, 0.0f, 360.0f, 24, kHomeColor);
        } else if (const entt::entity horde = world_.find(zombie->horde); horde != entt::null) {
            const entt::entity leader = world_.find(registry.get<Horde>(horde).leader);
            if (leader != entt::null && leader != entity) {
                DrawLineEx(at, drawnPosition(registry, leader, alpha), line, kHordeColor);
            }
        }
    }
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        if (const entt::entity target = world_.find(mind->target); target != entt::null) {
            if (const auto* building = registry.try_get<Building>(target)) {
                DrawRectangleLinesEx(buildingRect(*building), line, kTargetColor);
            }
        }
        const Vector2 spot = cellCenter(mind->spot);
        const float arm = std::max(Map::kCellSize * 0.5f, 5.0f * px);
        DrawLineEx({spot.x - arm, spot.y - arm}, {spot.x + arm, spot.y + arm}, line, kTargetColor);
        DrawLineEx({spot.x - arm, spot.y + arm}, {spot.x + arm, spot.y - arm}, line, kTargetColor);
    }
    const float radius = std::max(0.9f, 8.0f * px);
    DrawRing(at, radius, radius + line, 0.0f, 360.0f, 32, kSelectionColor);
}

void Inspector::drawPanel() {
    // Слева под панелью времени, до низа окна.
    constexpr float kTop = 520.0f;
    const float height = std::max(200.0f, ImGui::GetIO().DisplaySize.y - kTop - 12.0f);
    ImGui::SetNextWindowPos({12.0f, kTop}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({360.0f, height}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Инспектор###inspector")) {
        ImGui::SetNextItemWidth(120.0f);
        const bool entered = ImGui::InputInt("##goto", &goToId_, 0, 0,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Найти по id") || entered) {
            select(static_cast<EntityId>(std::max(goToId_, 0)));
            follow_ = selected_ != kInvalidId;
        }
        ImGui::Separator();

        const entt::registry& registry = world_.registry();
        const entt::entity entity = world_.find(selected_);
        if (selected_ == kInvalidId) {
            ImGui::TextDisabled("ЛКМ по существу, стае птиц или зданию — выбрать");
        } else if (entity == entt::null) {
            ImGui::Text("Объекта #%llu нет", static_cast<unsigned long long>(selected_));
        } else {
            if (registry.any_of<Position, Building, Horde>(entity)) {
                ImGui::Checkbox("Следить камерой", &follow_);
            }
            if (registry.all_of<Person>(entity)) {
                drawPerson(entity);
            } else if (registry.all_of<Zombie>(entity)) {
                drawZombie(entity);
            } else if (registry.all_of<Animal>(entity)) {
                drawAnimal(entity);
            } else if (registry.all_of<Flock>(entity)) {
                drawFlock(entity);
            } else if (registry.all_of<Corpse>(entity)) {
                drawCorpse(entity);
            } else if (registry.all_of<Horde>(entity)) {
                drawHorde(entity);
            } else if (registry.all_of<Building>(entity)) {
                drawBuilding(entity);
            } else if (registry.all_of<Settlement>(entity)) {
                drawSettlement(entity);
            } else {
                ImGui::Text("Объект #%llu", static_cast<unsigned long long>(selected_));
            }
        }
        if (selected_ != kInvalidId) {
            drawBiography();
        }
    }
    ImGui::End();
}

void Inspector::drawBiography() {
    const Chronicle& chronicle = world_.chronicle();
    const std::span<const std::uint32_t> indices = chronicle.eventsOf(selected_);
    char header[64];
    std::snprintf(header, sizeof(header), "Летопись: %zu###biography", indices.size());
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (indices.empty()) {
        ImGui::TextDisabled("событий нет");
        return;
    }
    EntityId clicked = kInvalidId;
    // Новые сверху; рисуются только видимые строки.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(indices.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::uint32_t index = indices[indices.size() - 1 - static_cast<std::size_t>(row)];
            ImGui::PushID(static_cast<int>(index));
            const EntityId id = drawChronicleEvent(chronicle.events()[index], selected_);
            ImGui::PopID();
            if (id != kInvalidId) {
                clicked = id;
            }
        }
    }
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawPerson(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    char duration[32];

    ImGui::Text("Человек #%llu", static_cast<unsigned long long>(selected_));
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        ImGui::Text("Сейчас: %s%s%s", actionName(mind->action),
                    mind->phase == ActionPhase::Travel ? " (в пути)" : "",
                    registry.get<Person>(entity).indoors ? " (дома, в укрытии)" : "");
        if (mind->target != kInvalidId) {
            ImGui::SameLine();
            ImGui::TextUnformatted("цель");
            ImGui::SameLine();
            if (idLink(mind->target)) {
                select(mind->target);
                return;
            }
        }
        if (mind->action == Action::Forage) {
            ImGui::Text("Ягоды и грибы на месте сбора: %d из %d",
                        world_.resources().forestAt(mind->spot), Resources::kForestMax);
        }
        ImGui::Text("Оценка при выборе: %.2f", mind->score);
        if (mind->phase == ActionPhase::Perform &&
            mind->until != std::numeric_limits<std::uint64_t>::max() && mind->until > tick) {
            formatGameDuration(duration, sizeof(duration), mind->until - tick);
            ImGui::Text("До конца: %s", duration);
        }
        formatGameDuration(duration, sizeof(duration),
                           mind->nextThink > tick ? mind->nextThink - tick : 0);
        ImGui::Text("Пересмотр решения через %s", duration);
        if (mind->failures > 0) {
            ImGui::TextColored({1.0f, 0.55f, 0.3f, 1.0f}, "Не нашлось пути: %u раз подряд",
                               mind->failures);
        }
        if (mind->emptySearches > 0) {
            ImGui::Text("Обысков без еды подряд: %d", mind->emptySearches);
        }
        if (mind->fledAt > 0) {
            formatGameDuration(duration, sizeof(duration), tick - mind->fledAt);
            ImGui::Text("Бежал от зомби %s назад", duration);
        }
        int dangers = 0;
        float nearest = 0.0f;
        const Position& at = registry.get<Position>(entity);
        for (const Danger& danger : mind->dangers) {
            if (danger.until > tick) {
                const float d = std::hypot(Map::cellCenter(danger.cell.x) - at.x,
                                           Map::cellCenter(danger.cell.y) - at.y);
                nearest = dangers++ == 0 ? d : std::min(nearest, d);
            }
        }
        if (dangers > 0) {
            ImGui::Text("Помнит опасных мест: %d, ближайшее в %.0f м", dangers, nearest);
        }
    }

    const Person& person = registry.get<Person>(entity);
    if (const entt::entity home = world_.find(person.home); home != entt::null) {
        ImGui::TextUnformatted("Дом:");
        ImGui::SameLine();
        if (idLink(person.home)) {
            select(person.home);
            return;
        }
        ImGui::SameLine();
        const auto* supplies = registry.try_get<Supplies>(home);
        ImGui::Text("%s, еды %d, воды %d", buildingName(registry.get<Building>(home).kind),
                    supplies != nullptr ? supplies->food : 0,
                    supplies != nullptr ? supplies->water : 0);
    } else {
        ImGui::TextUnformatted("Дом: нет");
    }
    if (const auto* carry = registry.try_get<Carry>(entity)) {
        ImGui::Text("Несёт: еды %d, воды %d", carry->food, carry->water);
    }
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        if (isFirearm(weapon->kind)) {
            ImGui::Text("Оружие: %s, патронов %d", weaponName(weapon->kind), weapon->ammo);
        } else {
            ImGui::Text("Оружие: %s", weaponName(weapon->kind));
        }
    }
    if (const auto* combat = registry.try_get<Combat>(entity);
        combat != nullptr && combat->target != kInvalidId) {
        ImGui::TextUnformatted("Бьёт или целится в");
        ImGui::SameLine();
        if (idLink(combat->target)) {
            select(combat->target);
            return;
        }
    }
    if (const auto* wounds = registry.try_get<Wounds>(entity)) {
        if (wounds->bleeding > 0.0f) {
            ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "Кровотечение: %.0f%% здоровья в час",
                               wounds->bleeding * 100.0f);
        }
        if (wounds->bites > 0) {
            ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "Укушен: %d раз", wounds->bites);
        }
    }

    if (const auto* needs = registry.try_get<Needs>(entity)) {
        ImGui::SeparatorText("Нужды");
        meter("Голод", needs->hunger);
        meter("Жажда", needs->thirst);
        meter("Усталость", needs->fatigue);
        meter("Страх", needs->fear);
        meter("Одиночество", needs->loneliness);
        meter("Здоровье", needs->health);
    }

    if (!drawPerception(entity)) {
        return;
    }

    if (const auto* traits = registry.try_get<Traits>(entity);
        traits != nullptr && ImGui::CollapsingHeader("Черты характера", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::pair<const char*, float> rows[] = {
            {"Смелость", traits->bravery},       {"Жадность", traits->greed},
            {"Альтруизм", traits->altruism},     {"Агрессия", traits->aggression},
            {"Лидерство", traits->leadership},   {"Общительность", traits->sociability},
            {"Любопытство", traits->curiosity},  {"Запасливость", traits->prudence},
            {"Сова", traits->chronotype},
        };
        if (ImGui::BeginTable("traits", 4, ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& [name, value] : rows) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", value * 100.0f);
            }
            ImGui::EndTable();
        }
    }

    if (registry.all_of<Mind, Needs>(entity) && ImGui::CollapsingHeader("Оценки действий")) {
        const ActionScores scores = evaluateActions(world_, entity);
        const Action current = registry.get<Mind>(entity).action;
        for (std::size_t i = 0; i < scores.size(); ++i) {
            const auto action = static_cast<Action>(i);
            ImGui::Text("%s%s: %.2f", action == current ? "> " : "  ", actionName(action),
                        scores[i]);
        }
    }

    drawMovement(entity);

    if (const auto* mind = registry.try_get<Mind>(entity);
        mind != nullptr && ImGui::CollapsingHeader("Память: обысканные здания")) {
        int shown = 0;
        // От последнего обысканного к первому.
        for (std::size_t k = 0; k < Mind::kSearchedMemory; ++k) {
            const std::size_t index =
                (mind->searchedNext + Mind::kSearchedMemory - 1 - k) % Mind::kSearchedMemory;
            const EntityId building = mind->searched[index];
            if (building == kInvalidId) {
                continue;
            }
            if (shown++ % 6 != 0) {
                ImGui::SameLine();
            }
            if (idLink(building)) {
                select(building);
                return;
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("пусто");
        }
    }
}

void Inspector::drawMovement(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    if (ImGui::CollapsingHeader("Движение")) {
        const Position& position = registry.get<Position>(entity);
        const Cell cell = Map::cellAt(position.x, position.y);
        ImGui::Text("Позиция: %.1f, %.1f м (клетка %d, %d)", position.x, position.y, cell.x, cell.y);
        if (const auto* velocity = registry.try_get<Velocity>(entity)) {
            ImGui::Text("Скорость: %.2f м/с", std::hypot(velocity->x, velocity->y));
        }
        if (const auto* walker = registry.try_get<Walker>(entity)) {
            if (walker->moving()) {
                float left = 0.0f;
                Vector2 from = {position.x, position.y};
                for (std::size_t i = walker->next; i < walker->waypoints.size(); ++i) {
                    const Vector2 to = cellCenter(walker->waypoints[i]);
                    left += std::hypot(to.x - from.x, to.y - from.y);
                    from = to;
                }
                ImGui::Text("Путь: осталось %.0f м, точек %zu из %zu", left,
                            walker->waypoints.size() - walker->next, walker->waypoints.size());
            } else {
                ImGui::TextUnformatted("Путь: стоит на месте");
            }
            ImGui::Text("Шаг по дороге: %.2f м/с", walker->speed);
        }
    }
}

bool Inspector::drawPerception(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const auto* senses = registry.try_get<Senses>(entity);
    const auto* awareness = registry.try_get<Awareness>(entity);
    if (senses == nullptr || awareness == nullptr ||
        !ImGui::CollapsingHeader("Восприятие", ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }
    const SenseConditions conditions = senseConditions(world_);
    const bool asleep = isAsleep(registry, entity);
    const float sight = sightRange(*senses, asleep, conditions);
    const float hearing = hearingFactor(*senses, asleep, conditions);
    if (sight > 0.0f) {
        ImGui::Text("Видит идущего до %.0f м (стоящего — до %.0f м)", sight,
                    sight * kStillVisibility);
    } else {
        ImGui::TextUnformatted("Спит: не видит, слышит хуже");
    }
    ImGui::Text("Слышит шаги до %.0f м, обыск до %.0f м", kFootstepsLoudness * hearing,
                kRummageLoudness * hearing);
    if (awareness->count == 0) {
        ImGui::TextDisabled("никого не замечает");
        return true;
    }
    const Position& position = registry.get<Position>(entity);
    const std::uint64_t tick = world_.tick();
    if (!ImGui::BeginTable("contacts", 3, ImGuiTableFlags_SizingFixedFit)) {
        return true;
    }
    EntityId clicked = kInvalidId;
    char ago[32];
    for (std::size_t i = 0; i < awareness->count; ++i) {
        const Contact& contact = awareness->contacts[i];
        ImGui::TableNextColumn();
        if (idLink(contact.id)) {
            clicked = contact.id;
        }
        ImGui::TableNextColumn();
        if (!isFresh(contact, tick)) {
            formatGameDuration(ago, sizeof(ago), tick - contact.tick);
            ImGui::TextDisabled("помнит, %s назад", ago);
        } else if (contact.seen && contact.heard != SoundKind::None) {
            ImGui::Text("видит, слышит: %s", soundName(contact.heard));
        } else if (contact.seen) {
            ImGui::TextUnformatted("видит");
        } else {
            ImGui::Text("слышит: %s", soundName(contact.heard));
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.0f м", std::hypot(contact.x - position.x, contact.y - position.y));
    }
    ImGui::EndTable();
    if (clicked != kInvalidId) {
        select(clicked);
        return false;
    }
    return true;
}

void Inspector::drawZombie(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Zombie& zombie = registry.get<Zombie>(entity);
    char duration[32];

    ImGui::Text("Зомби #%llu", static_cast<unsigned long long>(selected_));
    ImGui::Text("Сейчас: %s", zombieStateName(zombie.state));
    if (zombie.target != kInvalidId) {
        ImGui::SameLine();
        ImGui::TextUnformatted("добыча");
        ImGui::SameLine();
        if (idLink(zombie.target)) {
            select(zombie.target);
            return;
        }
    }
    if (const entt::entity horde = world_.find(zombie.horde); horde != entt::null) {
        ImGui::TextUnformatted("Орда:");
        ImGui::SameLine();
        if (idLink(zombie.horde)) {
            select(zombie.horde);
            return;
        }
        const Horde& info = registry.get<Horde>(horde);
        ImGui::SameLine();
        ImGui::Text("%zu зомби%s", info.members.size(), info.leader == selected_ ? ", вожак" : "");
    } else {
        const Position& position = registry.get<Position>(entity);
        ImGui::Text("Одиночка, родные места в %.0f м",
                    std::hypot(Map::cellCenter(zombie.anchor.x) - position.x,
                               Map::cellCenter(zombie.anchor.y) - position.y));
    }
    if ((zombie.state == ZombieState::Idle || zombie.state == ZombieState::Investigate) &&
        zombie.until > tick) {
        formatGameDuration(duration, sizeof(duration), zombie.until - tick);
        ImGui::Text("Стоит ещё %s", duration);
    }
    if (zombie.alerted > 0) {
        formatGameDuration(duration, sizeof(duration), tick - zombie.alerted);
        ImGui::Text("Добычу замечал %s назад", duration);
    }
    if (const auto* walker = registry.try_get<Walker>(entity)) {
        ImGui::Text("Погоня: %.2f м/с, сейчас темп %.0f%%", walker->speed, walker->pace * 100.0f);
    }
    meter("Здоровье", zombie.health);
    if (!drawPerception(entity)) {
        return;
    }
    drawMovement(entity);
}

void Inspector::drawHorde(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Horde& horde = registry.get<Horde>(entity);
    char duration[32];

    ImGui::Text("Орда #%llu: %zu зомби", static_cast<unsigned long long>(selected_),
                horde.members.size());
    ImGui::TextUnformatted("Вожак:");
    ImGui::SameLine();
    if (idLink(horde.leader)) {
        select(horde.leader);
        return;
    }
    if (const entt::entity leader = world_.find(horde.leader); leader != entt::null) {
        if (const auto* zombie = registry.try_get<Zombie>(leader)) {
            ImGui::SameLine();
            ImGui::TextUnformatted(zombieStateName(zombie->state));
        }
    }
    const float toGoal = std::hypot(Map::cellCenter(horde.goal.x) - horde.x,
                                    Map::cellCenter(horde.goal.y) - horde.y);
    if (horde.travelling) {
        ImGui::Text("Идёт к клетке %d, %d: %.0f м", horde.goal.x, horde.goal.y, toGoal);
    } else if (horde.until > tick) {
        formatGameDuration(duration, sizeof(duration), horde.until - tick);
        ImGui::Text("Стоит ещё %s", duration);
    } else {
        ImGui::TextUnformatted("Сейчас тронется");
    }
    if (const entt::entity settlement = world_.find(horde.settlement); settlement != entt::null) {
        ImGui::Text("В поселении %s", registry.get<Settlement>(settlement).name.c_str());
        ImGui::SameLine();
        if (idLink(horde.settlement)) {
            select(horde.settlement);
            return;
        }
    } else {
        ImGui::TextUnformatted("Вне поселений");
    }
    ImGui::SeparatorText("Участники");
    EntityId clicked = kInvalidId;
    for (std::size_t i = 0; i < horde.members.size(); ++i) {
        if (i % 6 != 0) {
            ImGui::SameLine();
        }
        if (idLink(horde.members[i])) {
            clicked = horde.members[i];
        }
    }
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawAnimal(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Animal& animal = registry.get<Animal>(entity);
    const AnimalSpec& spec = animalSpec(animal.kind);
    char duration[32];

    ImGui::Text("Животное #%llu: %s", static_cast<unsigned long long>(selected_),
                animalKindName(animal.kind));
    ImGui::Text("Сейчас: %s", animalStateName(animal.state));
    if (animal.state == AnimalState::Flee && animal.threat != kInvalidId) {
        ImGui::SameLine();
        ImGui::TextUnformatted("от");
        ImGui::SameLine();
        if (idLink(animal.threat)) {
            select(animal.threat);
            return;
        }
    }
    if (animal.state == AnimalState::Graze && animal.until > tick) {
        formatGameDuration(duration, sizeof(duration), animal.until - tick);
        ImGui::Text("Пасётся ещё %s", duration);
    }
    const Position& position = registry.get<Position>(entity);
    ImGui::Text("Родные места в %.0f м", std::hypot(Map::cellCenter(animal.anchor.x) - position.x,
                                                    Map::cellCenter(animal.anchor.y) - position.y));
    ImGui::Text("Убегает от увиденного ближе %.0f м, от услышанного — ближе %.0f м",
                spec.fleeSeen, spec.fleeHeard);
    ImGui::Text("Бегом %.1f м/с, шагом %.1f м/с", spec.runSpeed, spec.walkSpeed);
    meter("Здоровье", animal.health);
    if (!drawPerception(entity)) {
        return;
    }
    drawMovement(entity);
}

void Inspector::drawFlock(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Flock& flock = registry.get<Flock>(entity);
    char duration[32];
    ImGui::Text("Стая птиц #%llu: %d ворон", static_cast<unsigned long long>(selected_),
                flock.birds);
    const Position& position = registry.get<Position>(entity);
    if (flock.flying) {
        ImGui::Text("Летит: ещё %.0f м", std::hypot(flock.toX - position.x, flock.toY - position.y));
    } else if (flock.until > tick) {
        formatGameDuration(duration, sizeof(duration), flock.until - tick);
        ImGui::Text("Сидит, сама перелетит через %s", duration);
    } else {
        ImGui::TextUnformatted("Сейчас взлетит");
    }
    ImGui::TextDisabled("Вспугнутая взлетает с криком: его слышат зомби");
}

void Inspector::drawCorpse(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Corpse& corpse = registry.get<Corpse>(entity);
    char duration[32];
    if (corpse.kind == CorpseKind::Animal) {
        ImGui::Text("Туша #%llu: %s", static_cast<unsigned long long>(selected_),
                    animalKindName(corpse.animal));
        ImGui::Text("Мяса осталось: %d порций", corpse.meat);
    } else {
        ImGui::Text("Тело #%llu: %s", static_cast<unsigned long long>(selected_),
                    corpseKindName(corpse.kind));
    }
    formatGameDuration(duration, sizeof(duration), world_.tick() - corpse.diedAt);
    ImGui::Text("Погиб %s назад: %s", duration, deathCauseName(corpse.cause));
    if (corpse.killer != kInvalidId) {
        ImGui::TextUnformatted("Кто убил:");
        ImGui::SameLine();
        if (idLink(corpse.killer)) {
            select(corpse.killer);
            return;
        }
    }
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        ImGui::Text("При теле: %s", weaponName(weapon->kind));
        if (isFirearm(weapon->kind)) {
            ImGui::SameLine();
            ImGui::Text("(патронов %d)", weapon->ammo);
        }
    }
    if (const auto* carry = registry.try_get<Carry>(entity); carry != nullptr &&
                                                            (carry->food > 0 || carry->water > 0)) {
        ImGui::Text("При теле: еды %d, воды %d", carry->food, carry->water);
    }
}

void Inspector::drawBuilding(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Building& building = registry.get<Building>(entity);
    ImGui::Text("Здание #%llu: %s", static_cast<unsigned long long>(selected_),
                buildingName(building.kind));
    if (const entt::entity settlement = world_.find(building.settlement); settlement != entt::null) {
        ImGui::Text("Поселение: %s", registry.get<Settlement>(settlement).name.c_str());
        ImGui::SameLine();
        if (idLink(building.settlement)) {
            select(building.settlement);
            return;
        }
    }
    ImGui::Text("Размер: %.0f × %.0f м, вход в клетке %d, %d",
                static_cast<float>(building.width) * Map::kCellSize,
                static_cast<float>(building.height) * Map::kCellSize, building.door.x,
                building.door.y);
    drawSupplies(registry, entity);

    // Жильцы и те, кто идёт сюда или что-то здесь делает.
    std::vector<EntityId> residents;
    std::vector<std::pair<EntityId, Action>> visitors;
    for (const auto [other, id, person] : registry.view<const Id, const Person>().each()) {
        if (person.home == selected_) {
            residents.push_back(id.value);
        }
        if (const auto* mind = registry.try_get<Mind>(other); mind != nullptr && mind->target == selected_) {
            visitors.emplace_back(id.value, mind->action);
        }
    }
    std::sort(residents.begin(), residents.end());
    std::sort(visitors.begin(), visitors.end());

    // Один и тот же человек может быть в обоих списках: у ссылок разные области id.
    EntityId clicked = kInvalidId;
    ImGui::SeparatorText("Жильцы");
    if (residents.empty()) {
        ImGui::TextDisabled("никого");
    }
    ImGui::PushID("residents");
    for (std::size_t i = 0; i < residents.size(); ++i) {
        if (i % 6 != 0) {
            ImGui::SameLine();
        }
        if (idLink(residents[i])) {
            clicked = residents[i];
        }
    }
    ImGui::PopID();
    ImGui::SeparatorText("Направляются сюда");
    if (visitors.empty()) {
        ImGui::TextDisabled("никто");
    }
    ImGui::PushID("visitors");
    for (const auto& [id, action] : visitors) {
        if (idLink(id)) {
            clicked = id;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(actionName(action));
    }
    ImGui::PopID();
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawSettlement(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Settlement& settlement = registry.get<Settlement>(entity);
    ImGui::Text("%s %s (#%llu)", settlement.kind == SettlementKind::Town ? "Город" : "Деревня",
                settlement.name.c_str(), static_cast<unsigned long long>(selected_));
    ImGui::Text("Центр: клетка %d, %d, радиус %.0f м", settlement.center.x, settlement.center.y,
                static_cast<float>(settlement.radius) * Map::kCellSize);

    int buildings = 0;
    int food = 0;
    int water = 0;
    std::vector<EntityId> homes;
    for (const auto [other, id, building] : registry.view<const Id, const Building>().each()) {
        if (building.settlement != selected_) {
            continue;
        }
        ++buildings;
        homes.push_back(id.value);
        if (const auto* supplies = registry.try_get<Supplies>(other)) {
            food += supplies->food;
            water += supplies->water;
        }
    }
    std::sort(homes.begin(), homes.end());
    int people = 0;
    for (const auto [other, person] : registry.view<const Person>().each()) {
        people += std::binary_search(homes.begin(), homes.end(), person.home) ? 1 : 0;
    }
    ImGui::Text("Зданий: %d, жителей: %d", buildings, people);
    ImGui::Text("Припасы во всех зданиях: еды %d, воды %d", food, water);
}

} // namespace lw::ui
