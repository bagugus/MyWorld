#include "ui/Inspector.h"

#include "render/CameraController.h"
#include "sim/Components.h"
#include "sim/Map.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "sim/systems/UtilityAiSystem.h"
#include "ui/Format.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace lw::ui {

namespace {

constexpr float kPickPixels = 8.0f;
constexpr Color kSelectionColor = {255, 255, 255, 255};
constexpr Color kPathColor = {255, 236, 140, 220};
constexpr Color kTargetColor = {255, 120, 90, 255};
constexpr Color kHomeColor = {120, 220, 255, 255};

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
            return registry.all_of<Person>(hit.entity);
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
    if (!registry.all_of<Position>(entity)) {
        return;
    }

    const Vector2 at = drawnPosition(registry, entity, alpha);
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
            ImGui::TextDisabled("ЛКМ по человеку или зданию — выбрать");
        } else if (entity == entt::null) {
            ImGui::Text("Объекта #%llu нет", static_cast<unsigned long long>(selected_));
        } else {
            if (registry.any_of<Position, Building>(entity)) {
                ImGui::Checkbox("Следить камерой", &follow_);
            }
            if (registry.all_of<Person>(entity)) {
                drawPerson(entity);
            } else if (registry.all_of<Building>(entity)) {
                drawBuilding(entity);
            } else if (registry.all_of<Settlement>(entity)) {
                drawSettlement(entity);
            } else {
                ImGui::Text("Объект #%llu", static_cast<unsigned long long>(selected_));
            }
        }
    }
    ImGui::End();
}

void Inspector::drawPerson(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    char duration[32];

    ImGui::Text("Человек #%llu", static_cast<unsigned long long>(selected_));
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        ImGui::Text("Сейчас: %s%s", actionName(mind->action),
                    mind->phase == ActionPhase::Travel ? " (в пути)" : "");
        if (mind->target != kInvalidId) {
            ImGui::SameLine();
            ImGui::TextUnformatted("цель");
            ImGui::SameLine();
            if (idLink(mind->target)) {
                select(mind->target);
                return;
            }
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

    if (const auto* needs = registry.try_get<Needs>(entity)) {
        ImGui::SeparatorText("Нужды");
        meter("Голод", needs->hunger);
        meter("Жажда", needs->thirst);
        meter("Усталость", needs->fatigue);
        meter("Страх", needs->fear);
        meter("Одиночество", needs->loneliness);
        meter("Здоровье", needs->health);
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
