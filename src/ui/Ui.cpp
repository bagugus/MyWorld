#include "ui/Ui.h"

#include "render/CameraController.h"
#include "render/WorldRenderer.h"
#include "sim/Components.h"
#include "sim/SimDriver.h"
#include "sim/World.h"

#include <imgui.h>
#include <raylib.h>
#include <rlImGui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace lw::ui {

namespace {

// В стандартном шрифте ImGui нет кириллицы, поэтому берём системный.
// Глифы подгружаются по мере надобности, диапазоны задавать не нужно.
constexpr const char* kFontPath = "C:/Windows/Fonts/segoeui.ttf";
constexpr float kFontSize = 18.0f;

constexpr int kSpeeds[] = {1, 10, 100};

const char* buildingName(BuildingKind kind) {
    switch (kind) {
    case BuildingKind::House:
        return "дом";
    case BuildingKind::Barn:
        return "сарай";
    case BuildingKind::Apartment:
        return "многоэтажка";
    case BuildingKind::Shop:
        return "магазин";
    }
    return "здание";
}

// Что находится в клетке: местность, вода, дорога, здание.
void drawCellInfo(const World& world, Cell cell) {
    const Map& map = world.map();
    constexpr const char* kGround[] = {"трава", "песок", "камень", "пашня"};
    constexpr const char* kWater[] = {"", "море", "озеро", "река"};

    const Water water = map.water.at(cell.x, cell.y);
    if (water != Water::None) {
        // Слой depth насыщается на 25,5 м; глубину моря точнее дать по дну.
        const float depth = water == Water::Sea ? -map.elevationMeters(cell) : map.depthMeters(cell);
        ImGui::Text("Вода: %s, глубина %.1f м", kWater[static_cast<int>(water)], depth);
    } else {
        ImGui::Text("Местность: %s%s", kGround[static_cast<int>(map.ground.at(cell.x, cell.y))],
                    map.cover.at(cell.x, cell.y) == Cover::Forest ? ", лес" : "");
    }

    const Road road = map.road.at(cell.x, cell.y);
    if (road != Road::None) {
        ImGui::Text("Дорога: %s%s", road == Road::Asphalt ? "асфальт" : "грунтовая",
                    water != Water::None ? " (мост)" : "");
    }

    for (const auto [entity, building] : world.registry().view<const Building>().each()) {
        if (cell.x < building.origin.x || cell.y < building.origin.y ||
            cell.x >= building.origin.x + building.width ||
            cell.y >= building.origin.y + building.height) {
            continue;
        }
        const entt::entity owner = world.find(building.settlement);
        ImGui::Text("Здание #%llu: %s, %s",
                    static_cast<unsigned long long>(world.registry().get<Id>(entity).value),
                    buildingName(building.kind),
                    owner != entt::null ? world.registry().get<Settlement>(owner).name.c_str() : "—");
        break;
    }
}

} // namespace

void init() {
    rlImGuiBeginInitImGui();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // rlImGui уже добавил стандартный шрифт — он остаётся запасным вариантом.
    if (FileExists(kFontPath)) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(kFontPath, kFontSize);
    }

    rlImGuiEndInitImGui();
}

void shutdown() {
    rlImGuiShutdown();
}

void beginFrame() {
    rlImGuiBegin();
}

void endFrame() {
    rlImGuiEnd();
}

bool wantsMouse() {
    return ImGui::GetIO().WantCaptureMouse;
}

bool wantsKeyboard() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void handleHotkeys(SimDriver& driver) {
    if (wantsKeyboard()) {
        return;
    }
    if (IsKeyPressed(KEY_SPACE)) {
        driver.togglePause();
    }
    constexpr int keys[] = {KEY_ONE, KEY_TWO, KEY_THREE};
    for (int i = 0; i < 3; ++i) {
        if (IsKeyPressed(keys[i])) {
            driver.setSpeed(kSpeeds[i]);
            driver.setPaused(false);
        }
    }
}

void drawTimePanel(const World& world, SimDriver& driver) {
    ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({300.0f, 0.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Время")) {
        if (ImGui::Button(driver.paused() ? "Продолжить###pause" : "Пауза###pause",
                          {110.0f, 0.0f})) {
            driver.togglePause();
        }

        for (const int speed : kSpeeds) {
            ImGui::SameLine();
            const bool active = !driver.paused() && driver.speed() == speed;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            ImGui::PushID(speed);
            char label[16];
            std::snprintf(label, sizeof(label), "x%d", speed);
            if (ImGui::Button(label)) {
                driver.setSpeed(speed);
                driver.setPaused(false);
            }
            ImGui::PopID();
            if (active) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::BeginDisabled(!driver.paused());
        if (ImGui::Button("Шаг на один тик")) {
            driver.requestStep();
        }
        ImGui::EndDisabled();

        const auto totalSeconds = static_cast<unsigned long long>(world.seconds());
        ImGui::Separator();
        ImGui::Text("Тик: %llu", static_cast<unsigned long long>(world.tick()));
        ImGui::Text("Время мира: %llu ч %02llu мин %02llu с", totalSeconds / 3600,
                    totalSeconds / 60 % 60, totalSeconds % 60);
        ImGui::TextDisabled("Space — пауза, 1 / 2 / 3 — скорость");
    }
    ImGui::End();
}

void drawSettlementLabels(const World& world, const CameraController& camera) {
    constexpr float kTownSize = 24.0f;
    constexpr float kVillageSize = 17.0f;
    constexpr float kMaxLift = 70.0f; // пикселей: подпись не уезжает далеко от центра вблизи
    constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 235);
    constexpr ImU32 kShadowColor = IM_COL32(0, 0, 0, 200);

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    for (const auto [entity, settlement] : world.registry().view<const Settlement>().each()) {
        const Vector2 center = GetWorldToScreen2D(
            {Map::cellCenter(settlement.center.x), Map::cellCenter(settlement.center.y)},
            camera.camera());
        const float size = settlement.kind == SettlementKind::Town ? kTownSize : kVillageSize;
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, settlement.name.c_str());
        // Подпись — над поселением.
        const float radiusPx = static_cast<float>(settlement.radius) * Map::kCellSize * camera.zoom();
        const ImVec2 position = {center.x - extent.x * 0.5f,
                                 center.y - std::min(radiusPx, kMaxLift) - extent.y};
        if (position.x + extent.x < 0.0f || position.y + extent.y < 0.0f ||
            position.x > screen.x || position.y > screen.y) {
            continue;
        }
        drawList->AddText(font, size, {position.x + 1.0f, position.y + 1.0f}, kShadowColor,
                          settlement.name.c_str());
        drawList->AddText(font, size, position, kTextColor, settlement.name.c_str());
    }
}

void drawStatsPanel(const World& world, const SimDriver& driver, const CameraController& camera,
                    const WorldRenderer& renderer) {
    ImGui::SetNextWindowPos({12.0f, 210.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({300.0f, 0.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Статистика")) {
        const int targetTicks = driver.paused() ? 0 : driver.speed() * World::kTicksPerSecond;
        ImGui::Text("FPS: %d", GetFPS());
        ImGui::Text("Тиков в секунду: %.0f из %d", driver.measuredTicksPerSecond(), targetTicks);
        if (driver.behind()) {
            ImGui::TextColored({1.0f, 0.55f, 0.3f, 1.0f}, "Симуляция не успевает за скоростью");
        }
        ImGui::Text("Сущностей: %zu", world.entityCount());
        ImGui::Text("Seed: %llu", static_cast<unsigned long long>(world.seed()));

        const Map& map = world.map();
        ImGui::Text("Карта: %d × %d клеток по %.0f м", map.width(), map.height(),
                    Map::kCellSize);
        ImGui::Text("Поселений: %zu, зданий: %zu",
                    world.registry().view<const Settlement>().size(),
                    world.registry().view<const Building>().size());
        ImGui::Text("Чанков карты в кадре: %d из %d", renderer.map().chunksDrawn(),
                    renderer.map().chunkCount());

        ImGui::Separator();
        const Vector2 mouse = GetScreenToWorld2D(GetMousePosition(), camera.camera());
        ImGui::Text("Зум: %.2f пикс/м", camera.zoom());
        ImGui::Text("Курсор: %.0f, %.0f м", mouse.x, mouse.y);
        const Cell cell = Map::cellAt(mouse.x, mouse.y);
        if (map.contains(cell)) {
            ImGui::Text("Клетка: %d, %d, высота %.1f м", cell.x, cell.y,
                        map.elevationMeters(cell));
            drawCellInfo(world, cell);
        } else {
            ImGui::TextDisabled("Клетка: вне карты");
        }
        ImGui::TextDisabled("Колесо — зум, ПКМ или WASD — панорама,\nHome — весь мир");
    }
    ImGui::End();
}

} // namespace lw::ui
