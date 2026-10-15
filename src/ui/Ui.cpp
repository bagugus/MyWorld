#include "ui/Ui.h"

#include "render/CameraController.h"
#include "render/WorldRenderer.h"
#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/SimDriver.h"
#include "sim/World.h"
#include "ui/Format.h"

#include <imgui.h>
#include <raylib.h>
#include <rlImGui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace lw::ui {

namespace {

// В стандартном шрифте ImGui нет кириллицы, поэтому берём системный.
// Глифы подгружаются по мере надобности, диапазоны задавать не нужно.
constexpr const char* kFontPath = "C:/Windows/Fonts/segoeui.ttf";
constexpr float kFontSize = 18.0f;

constexpr int kSpeeds[] = {1, 10, 100};

const char* seasonName(Season season) {
    constexpr const char* kNames[] = {"весна", "лето", "осень", "зима"};
    return kNames[static_cast<int>(season)];
}

const char* weatherName(const Weather& weather, bool snow) {
    switch (weather.kind) {
    case WeatherKind::Clear:
        return "ясно";
    case WeatherKind::Cloudy:
        return "переменная облачность";
    case WeatherKind::Overcast:
        return "пасмурно";
    case WeatherKind::Fog:
        return "туман";
    case WeatherKind::Rain:
        return snow ? "снегопад" : "дождь";
    case WeatherKind::Storm:
        return snow ? "метель" : "гроза";
    case WeatherKind::Count:
        break;
    }
    return "—";
}

// Откуда дует ветер — по восьми румбам.
const char* windName(float fromDegrees) {
    constexpr const char* kNames[] = {"северный", "северо-восточный", "восточный", "юго-восточный",
                                      "южный", "юго-западный", "западный", "северо-западный"};
    const int index = static_cast<int>(fromDegrees / 45.0f + 0.5f) % 8;
    return kNames[index];
}

const char* dayPhaseName(const SunState& sun) {
    if (sun.elevation > 6.0f) {
        return "день";
    }
    if (sun.elevation > -6.0f) {
        return "сумерки";
    }
    return "ночь";
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
        if (map.cover.at(cell.x, cell.y) == Cover::Forest) {
            ImGui::Text("Ягоды и грибы в округе: %d из %d", world.resources().forestAt(cell),
                        Resources::kForestMax);
        }
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
        if (const auto* supplies = world.registry().try_get<Supplies>(entity)) {
            ImGui::Text("Припасы: еды %d, воды %d", supplies->food, supplies->water);
        }
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

void drawTimePanel(const World& world, const Environment& environment, SimDriver& driver) {
    ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_FirstUseEver);
    // Высота по содержимому: панель растёт вместе с индикаторами.
    ImGui::SetNextWindowSizeConstraints({320.0f, 0.0f}, {FLT_MAX, FLT_MAX});
    if (ImGui::Begin("Время###time", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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

        const CalendarTime& time = environment.time;
        const SunState& sun = environment.sun;
        const Weather& weather = world.weather();
        const bool snow = isSnowing(environment.temperature);

        ImGui::Separator();
        ImGui::Text("%02d:%02d — %s", time.hour, time.minute, dayPhaseName(sun));
        ImGui::Text("День %d: %s, %d-й день из %d, год %d", time.day, seasonName(time.season),
                    time.dayOfSeason + 1, kGameDaysPerSeason, time.year);
        ImGui::Text("Рассвет %02d:%02d, закат %02d:%02d", sun.sunriseMinute / 60,
                    sun.sunriseMinute % 60, sun.sunsetMinute / 60, sun.sunsetMinute % 60);
        meter("Свет", environment.light);
        meter("Луна", sun.moonFullness);

        ImGui::Separator();
        ImGui::Text("Погода: %s", weatherName(weather, snow));
        ImGui::Text("Температура: %+.1f °C (у моря)", environment.temperature);
        ImGui::Text("Ветер: %s, %.0f м/с", windName(weather.windFrom), weather.windSpeed);
        meter("Облачность", weather.cloudCover);
        meter(snow ? "Снегопад" : "Осадки", weather.precipitation);
        meter("Туман", weather.fog);
        meter("Снег на земле", weather.snowCover);

        const auto totalSeconds = static_cast<unsigned long long>(world.seconds());
        ImGui::Separator();
        ImGui::Text("Тик: %llu", static_cast<unsigned long long>(world.tick()));
        ImGui::Text("Время симуляции: %llu ч %02llu мин %02llu с", totalSeconds / 3600,
                    totalSeconds / 60 % 60, totalSeconds % 60);
        ImGui::TextDisabled("Игровой час — 5 мин симуляции");
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

void drawStatsPanel(const World& world, const Environment& environment, const SimDriver& driver,
                    const CameraController& camera, const WorldRenderer& renderer) {
    // Справа: слева под временем места нет.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 12.0f, 12.0f}, ImGuiCond_FirstUseEver, {1.0f, 0.0f});
    ImGui::SetNextWindowSize({320.0f, 0.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Статистика###stats")) {
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
        std::size_t walking = 0;
        for (const auto [entity, walker] : world.registry().view<const Walker>().each()) {
            walking += walker.moving() ? 1 : 0;
        }
        std::size_t asleep = 0;
        for (const auto [entity, person] : world.registry().view<const Person>().each()) {
            asleep += person.asleep ? 1 : 0;
        }
        std::size_t fleeing = 0;
        for (const auto [entity, mind] : world.registry().view<const Mind>().each()) {
            fleeing += mind.action == Action::Flee ? 1 : 0;
        }
        ImGui::Text("Людей: %zu, в пути: %zu, спят: %zu, бегут: %zu",
                    world.registry().view<const Person>().size(), walking, asleep, fleeing);
        std::size_t chasing = 0;
        std::size_t inHordes = 0;
        for (const auto [entity, zombie] : world.registry().view<const Zombie>().each()) {
            chasing += zombie.state == ZombieState::Chase ? 1 : 0;
            inHordes += zombie.horde != kInvalidId ? 1 : 0;
        }
        ImGui::Text("Зомби: %zu, в ордах: %zu, орд: %zu, преследуют: %zu",
                    world.registry().view<const Zombie>().size(), inHordes,
                    world.registry().view<const Horde>().size(), chasing);
        std::size_t animals = 0;
        std::size_t animalsFleeing = 0;
        for (const auto [entity, animal] : world.registry().view<const Animal>().each()) {
            ++animals;
            animalsFleeing += animal.state == AnimalState::Flee ? 1 : 0;
        }
        std::size_t flying = 0;
        for (const auto [entity, flock] : world.registry().view<const Flock>().each()) {
            flying += flock.flying ? 1 : 0;
        }
        ImGui::Text("Животных: %zu, убегают: %zu; стай птиц: %zu, в воздухе: %zu", animals,
                    animalsFleeing, world.registry().view<const Flock>().size(), flying);
        std::size_t bodies[3] = {};
        for (const auto [entity, corpse] : world.registry().view<const Corpse>().each()) {
            ++bodies[static_cast<std::size_t>(corpse.kind)];
        }
        std::size_t fighting = 0;
        std::size_t hunting = 0;
        for (const auto [entity, mind] : world.registry().view<const Mind>().each()) {
            fighting += mind.action == Action::Fight ? 1 : 0;
            hunting += mind.action == Action::Hunt ? 1 : 0;
        }
        ImGui::Text("Дерутся: %zu, охотятся: %zu; тела: людей %zu, зомби %zu, туш %zu", fighting,
                    hunting, bodies[0], bodies[1], bodies[2]);
        ImGui::Text("Чанков карты в кадре: %d из %d", renderer.map().chunksDrawn(),
                    renderer.map().chunkCount());

        ImGui::Separator();
        const Vector2 mouse = GetScreenToWorld2D(GetMousePosition(), camera.camera());
        ImGui::Text("Зум: %.2f пикс/м", camera.zoom());
        ImGui::Text("Курсор: %.0f, %.0f м", mouse.x, mouse.y);
        // Проверка пространственного индекса: соседи курсора.
        constexpr float kNeighbourRadius = 50.0f;
        int neighbours = 0;
        world.spatial().forEachInRadius(mouse.x, mouse.y, kNeighbourRadius,
                                        [&neighbours](const SpatialHit&) { ++neighbours; });
        if (const auto nearest = world.spatial().nearest(mouse.x, mouse.y, 1000.0f)) {
            ImGui::Text("В радиусе %.0f м: %d, ближайший #%llu в %.0f м", kNeighbourRadius,
                        neighbours, static_cast<unsigned long long>(nearest->id),
                        std::sqrt(nearest->distanceSq));
        } else {
            ImGui::Text("В радиусе %.0f м: %d, ближе 1 км никого", kNeighbourRadius, neighbours);
        }
        // Человек или зомби под курсором: не дальше нескольких пикселей.
        constexpr float kPickPixels = 8.0f;
        const float pickRadius = std::max(1.0f, kPickPixels / camera.zoom());
        if (const auto picked = world.spatial().nearest(
                mouse.x, mouse.y, pickRadius, [&world](const SpatialHit& hit) {
                    return world.registry().any_of<Person, Zombie, Animal, Flock, Corpse>(hit.entity);
                })) {
            if (const auto* animal = world.registry().try_get<Animal>(picked->entity)) {
                ImGui::Text("Животное #%llu: %s, %s", static_cast<unsigned long long>(picked->id),
                            animalKindName(animal->kind), animalStateName(animal->state));
            } else if (const auto* corpse = world.registry().try_get<Corpse>(picked->entity)) {
                ImGui::Text("Тело #%llu: %s", static_cast<unsigned long long>(picked->id),
                            corpse->kind == CorpseKind::Animal ? animalKindName(corpse->animal)
                                                               : corpseKindName(corpse->kind));
            } else if (const auto* flock = world.registry().try_get<Flock>(picked->entity)) {
                ImGui::Text("Стая #%llu: %d ворон%s", static_cast<unsigned long long>(picked->id),
                            flock->birds, flock->flying ? ", в воздухе" : "");
            } else if (const auto* zombie = world.registry().try_get<Zombie>(picked->entity)) {
                ImGui::Text("Зомби #%llu: %s", static_cast<unsigned long long>(picked->id),
                            zombieStateName(zombie->state));
            } else {
                const auto* mind = world.registry().try_get<Mind>(picked->entity);
                ImGui::Text("Человек #%llu: %s", static_cast<unsigned long long>(picked->id),
                            mind != nullptr ? actionName(mind->action) : "—");
            }
        }
        const Cell cell = Map::cellAt(mouse.x, mouse.y);
        if (map.contains(cell)) {
            ImGui::Text("Клетка: %d, %d, высота %.1f м, %+.1f °C", cell.x, cell.y,
                        map.elevationMeters(cell),
                        temperatureAtElevation(environment.temperature, map.elevationMeters(cell)));
            drawCellInfo(world, cell);
        } else {
            ImGui::TextDisabled("Клетка: вне карты");
        }
        ImGui::TextDisabled("Колесо — зум, ПКМ или WASD — панорама,\n"
                            "Home — весь мир, ЛКМ — инспектор");
    }
    ImGui::End();
}

} // namespace lw::ui
