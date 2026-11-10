#include "ui/Ui.h"

#include "render/CameraController.h"
#include "render/WorldRenderer.h"
#include "render/WorldRenderer3D.h"
#include "sim/Climate.h"
#include "sim/Components.h"
#include "sim/Parallel.h"
#include "sim/Profiler.h"
#include "sim/Scenario.h"
#include "sim/SimDriver.h"
#include "sim/World.h"
#include "sim/systems/EventSystem.h"
#include "ui/Format.h"
#include "ui/Inspector.h"
#include "ui/LodOverlay.h"
#include "util/ProcessMemory.h"

#include <imgui.h>
#include <raylib.h>
#include <raymath.h>
#include <rlImGui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lw::ui {

namespace {

// В стандартном шрифте ImGui нет кириллицы, поэтому берём системный.
// Глифы подгружаются по мере надобности, диапазоны задавать не нужно.
constexpr const char* kFontPath = "C:/Windows/Fonts/segoeui.ttf";
constexpr float kFontSize = 18.0f;
// Титры режима «Сериал».
constexpr const char* kTitleFontPath = "C:/Windows/Fonts/georgia.ttf";
constexpr const char* kItalicFontPath = "C:/Windows/Fonts/georgiai.ttf";
constexpr const char* kCaptionFontPath = "C:/Windows/Fonts/segoeuil.ttf";
ImFont* gTitleFont = nullptr;
ImFont* gItalicFont = nullptr;
ImFont* gCaptionFont = nullptr;

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
        if (const auto* supplies = world.registry().try_get<Inventory>(entity)) {
            ImGui::Text("Припасы: еды %d, воды %d", supplies->food(),
                        supplies->count(ItemKind::Water));
        }
        break;
    }
}

// Настройки камеры — рядом с imgui.ini, строки «ключ=значение».
constexpr const char* kCameraIni = "camera.ini";
bool gShowKeys = false; // окно «Клавиши» (F1)
std::optional<CameraSettings> gSavedCamera; // что сейчас в файле

// Вид, высота глаз и настройки камеры — в панели «Статистика».
void drawCameraControls(CameraController& camera) {
    struct View {
        ViewMode mode;
        const char* label;
    };
    constexpr View kViews[] = {{ViewMode::TopDown, "сверху (V)"},
                               {ViewMode::Orbit, "3D"},
                               {ViewMode::Fly, "полёт (G)"},
                               {ViewMode::Shoulder, "из-за плеча (T)"}};
    ImGui::TextUnformatted("Вид:");
    for (const View& view : kViews) {
        ImGui::SameLine();
        const bool unavailable = view.mode == ViewMode::Shoulder && !camera.hasSubject();
        ImGui::BeginDisabled(unavailable);
        if (ImGui::RadioButton(view.label, camera.mode() == view.mode)) {
            camera.setMode(view.mode);
        }
        ImGui::EndDisabled();
        if (unavailable) {
            ImGui::SetItemTooltip("Выберите в инспекторе человека, зомби или зверя");
        }
    }
    if (camera.is3d()) {
        ImGui::Text("Глаза над землёй: %.1f м", camera.eyeHeight());
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Клавиши (F1)")) {
        gShowKeys = !gShowKeys;
    }

    if (!ImGui::TreeNode("Настройки камеры")) {
        return;
    }
    CameraSettings& settings = camera.settings();
    ImGui::SliderFloat("Плавность, с", &settings.smoothing, 0.0f, 0.5f, "%.2f");
    ImGui::SetItemTooltip("За сколько камера проходит большую часть пути к цели; 0 — сразу");
    ImGui::SliderFloat("Инерция, с", &settings.inertia, 0.0f, 1.5f, "%.2f");
    ImGui::SetItemTooltip("Сколько панорама и полёт катятся после отпускания; 0 — останавливаются сразу");
    ImGui::SliderFloat("Мышь", &settings.mouseSensitivity, 0.2f, 3.0f, "×%.2f");
    ImGui::SliderFloat("Клавиши", &settings.keyboardSpeed, 0.2f, 3.0f, "×%.2f");
    ImGui::SliderFloat("Полёт", &settings.flySpeed, 0.1f, 10.0f, "×%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Скорость полёта; в полёте меняется и колесом мыши");
    ImGui::Checkbox("Мышь по вертикали наоборот", &settings.invertY);
    ImGui::Checkbox("Полёт: держать высоту над рельефом", &settings.holdAltitude);
    if (ImGui::SmallButton("По умолчанию")) {
        settings = CameraSettings{};
    }
    ImGui::TreePop();
}

void keyRow(const char* keys, const char* action) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(keys);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(action);
}

} // namespace

void loadCameraSettings(CameraSettings& settings) {
    if (FILE* file = std::fopen(kCameraIni, "r")) {
        char key[64];
        float value = 0.0f;
        while (std::fscanf(file, " %63[^=]=%f", key, &value) == 2) {
            const std::string name = key;
            if (name == "smoothing") settings.smoothing = std::clamp(value, 0.0f, 0.5f);
            if (name == "inertia") settings.inertia = std::clamp(value, 0.0f, 1.5f);
            if (name == "mouse") settings.mouseSensitivity = std::clamp(value, 0.2f, 3.0f);
            if (name == "keyboard") settings.keyboardSpeed = std::clamp(value, 0.2f, 3.0f);
            if (name == "fly") settings.flySpeed = std::clamp(value, 0.1f, 10.0f);
            if (name == "invert_y") settings.invertY = value != 0.0f;
            if (name == "hold_altitude") settings.holdAltitude = value != 0.0f;
        }
        std::fclose(file);
    }
    gSavedCamera = settings;
}

void saveCameraSettings(const CameraSettings& settings) {
    if (gSavedCamera && *gSavedCamera == settings) {
        return;
    }
    if (FILE* file = std::fopen(kCameraIni, "w")) {
        std::fprintf(file, "smoothing=%.3f\ninertia=%.3f\nmouse=%.3f\nkeyboard=%.3f\nfly=%.3f\n",
                     settings.smoothing, settings.inertia, settings.mouseSensitivity, settings.keyboardSpeed,
                     settings.flySpeed);
        std::fprintf(file, "invert_y=%d\nhold_altitude=%d\n", settings.invertY ? 1 : 0,
                     settings.holdAltitude ? 1 : 0);
        std::fclose(file);
    }
    gSavedCamera = settings;
}

void drawKeyHelp(const CameraController& camera) {
    if (!gShowKeys) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    if (!ImGui::Begin("Клавиши###keys", &gShowKeys, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    const auto table = [](const char* title) {
        ImGui::SeparatorText(title);
        return ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit);
    };
    if (table("Сейчас")) {
        switch (camera.mode()) {
        case ViewMode::TopDown:
            keyRow("колесо", "зум к курсору");
            keyRow("ПКМ / СКМ + мышь", "панорама (отпустить на ходу — покатится)");
            keyRow("WASD / стрелки", "панорама");
            break;
        case ViewMode::Orbit:
            keyRow("колесо", "ближе / дальше, к курсору");
            keyRow("ПКМ + мышь", "вращение и наклон");
            keyRow("СКМ + мышь", "панорама «за землю»");
            keyRow("WASD / стрелки", "панорама относительно взгляда");
            keyRow("Q / E, R / F", "вращение, наклон");
            break;
        case ViewMode::Fly:
            keyRow("WASD / стрелки", "вперёд, назад, вбок");
            keyRow("E / Q", "выше / ниже");
            keyRow("Shift", "вчетверо быстрее");
            keyRow("ПКМ + мышь", "осмотреться");
            keyRow("колесо", "скорость полёта");
            break;
        case ViewMode::Shoulder:
            keyRow("ПКМ + мышь", "осмотреться вокруг");
            keyRow("колесо", "ближе / дальше");
            break;
        case ViewMode::Cinema:
            keyRow("Esc", "выйти из сериала");
            break;
        }
        ImGui::EndTable();
    }
    if (table("Виды")) {
        keyRow("V", "сверху / 3D");
        keyRow("G", "свободный полёт / орбита");
        keyRow("T", "из-за плеча выбранного / орбита");
        keyRow("Home", "весь мир");
        keyRow("F11", "сериал: кино в 3D во весь экран, Esc — выход");
        ImGui::EndTable();
    }
    if (table("Мир")) {
        keyRow("ЛКМ", "выбрать в инспекторе");
        keyRow("Ctrl+ЛКМ", "путь: начало, Ctrl+Shift+ЛКМ — конец");
        keyRow("Пробел", "пауза");
        keyRow("1 / 2 / 3", "скорость x1 / x10 / x100");
        keyRow("F1", "это окно");
        ImGui::EndTable();
    }
    ImGui::End();
}

void init() {
    rlImGuiBeginInitImGui();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // rlImGui уже добавил стандартный шрифт — он остаётся запасным вариантом.
    if (FileExists(kFontPath)) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(kFontPath, kFontSize);
    }
    gTitleFont = FileExists(kTitleFontPath) ? io.Fonts->AddFontFromFileTTF(kTitleFontPath, kFontSize) : nullptr;
    gItalicFont = FileExists(kItalicFontPath) ? io.Fonts->AddFontFromFileTTF(kItalicFontPath, kFontSize) : nullptr;
    gCaptionFont =
        FileExists(kCaptionFontPath) ? io.Fonts->AddFontFromFileTTF(kCaptionFontPath, kFontSize) : nullptr;

    rlImGuiEndInitImGui();
}

void shutdown() {
    rlImGuiShutdown();
}

ImFont* titleFont() {
    return gTitleFont != nullptr ? gTitleFont : ImGui::GetIO().FontDefault;
}

ImFont* italicFont() {
    return gItalicFont != nullptr ? gItalicFont : titleFont();
}

ImFont* captionFont() {
    return gCaptionFont != nullptr ? gCaptionFont : ImGui::GetIO().FontDefault;
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
    if (IsKeyPressed(KEY_F1)) {
        gShowKeys = !gShowKeys;
    }
    constexpr int keys[] = {KEY_ONE, KEY_TWO, KEY_THREE};
    for (int i = 0; i < 3; ++i) {
        if (IsKeyPressed(keys[i])) {
            driver.setSpeed(kSpeeds[i]);
            driver.setPaused(false);
        }
    }
}

namespace {

// Промотка времени на произвольный срок: сутки, часы, минуты игрового времени.
void drawSkipControls(SimDriver& driver) {
    static int days = 0;
    static int hours = 1;
    static int minutes = 0;

    ImGui::Separator();
    ImGui::TextUnformatted("Промотать вперёд (игровое время):");
    if (driver.skipping()) {
        const std::uint64_t left = driver.skipRemaining();
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "осталось %llu ч %02llu мин",
                      static_cast<unsigned long long>(left / kTicksPerGameHour),
                      static_cast<unsigned long long>(left % kTicksPerGameHour / kTicksPerGameMinute));
        ImGui::ProgressBar(driver.skipProgress(), {-1.0f, 0.0f}, overlay);
        if (ImGui::Button("Остановить промотку")) {
            driver.cancelSkip();
        }
        return;
    }

    ImGui::PushItemWidth(70.0f);
    ImGui::InputInt("сут###skipDays", &days, 0);
    ImGui::SameLine();
    ImGui::InputInt("ч###skipHours", &hours, 0);
    ImGui::SameLine();
    ImGui::InputInt("мин###skipMinutes", &minutes, 0);
    ImGui::PopItemWidth();
    days = std::clamp(days, 0, 100000);
    hours = std::clamp(hours, 0, 100000);
    minutes = std::clamp(minutes, 0, 100000);

    const std::uint64_t ticks = static_cast<std::uint64_t>(days) * kTicksPerGameDay +
                                static_cast<std::uint64_t>(hours) * kTicksPerGameHour +
                                static_cast<std::uint64_t>(minutes) * kTicksPerGameMinute;
    ImGui::BeginDisabled(ticks == 0);
    if (ImGui::Button("Промотать")) {
        driver.requestSkip(ticks);
    }
    ImGui::EndDisabled();

    struct Preset {
        const char* label;
        std::uint64_t ticks;
    };
    const Preset presets[] = {{"+1 ч", kTicksPerGameHour},
                              {"+6 ч", 6 * kTicksPerGameHour},
                              {"+1 сут", kTicksPerGameDay},
                              {"+1 сезон", kGameDaysPerSeason * kTicksPerGameDay},
                              {"+1 год", kTicksPerGameYear}};
    for (const Preset& preset : presets) {
        ImGui::SameLine();
        if (ImGui::Button(preset.label)) {
            driver.requestSkip(preset.ticks);
        }
    }
}

} // namespace

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

        drawSkipControls(driver);

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
        // Фаза мира (этап 7.4): управление — в панели «Сценарий и ивенты».
        if (const Scenario* scenario = scenarioOf(world)) {
            char duration[32];
            formatGameDuration(duration, sizeof(duration),
                               world.tick() - scenario->began[static_cast<std::size_t>(scenario->phase)]);
            ImGui::Text("Фаза мира: %s, %s", phaseName(scenario->phase), duration);
        } else {
            ImGui::TextDisabled("Фаза мира: выживание (без сценария)");
        }
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
    constexpr float kFarLabel = 2500.0f; // метров: в 3D дальше не подписываем,
    constexpr float kFarLabelHeights = 8.0f; // если глаз не выше 1/8 этого
    constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 235);
    constexpr ImU32 kShadowColor = IM_COL32(0, 0, 0, 200);

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    for (const auto [entity, settlement] : world.registry().view<const Settlement>().each()) {
        const Vector2 at = {Map::cellCenter(settlement.center.x), Map::cellCenter(settlement.center.y)};
        if (camera.is3d()) {
            // Низко над землёй дальние подписи сбились бы в ряд у горизонта.
            const Vector3 eye = camera.camera3d().position;
            const float limit = std::max(kFarLabel, kFarLabelHeights * camera.eyeHeight());
            if (Vector2Distance(at, {eye.x, eye.z}) > limit) {
                continue;
            }
        }
        const std::optional<Vector2> center = camera.worldToScreen(at);
        if (!center) {
            continue;
        }
        const float size = settlement.kind == SettlementKind::Town ? kTownSize : kVillageSize;
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, settlement.name.c_str());
        // Подпись — над поселением.
        const float radiusPx = static_cast<float>(settlement.radius) * Map::kCellSize * camera.zoomAt(at);
        const ImVec2 position = {center->x - extent.x * 0.5f,
                                 center->y - std::min(radiusPx, kMaxLift) - extent.y};
        if (position.x + extent.x < 0.0f || position.y + extent.y < 0.0f ||
            position.x > screen.x || position.y > screen.y) {
            continue;
        }
        drawList->AddText(font, size, {position.x + 1.0f, position.y + 1.0f}, kShadowColor,
                          settlement.name.c_str());
        drawList->AddText(font, size, position, kTextColor, settlement.name.c_str());
    }
}

namespace {

double megabytes(std::size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// Замер (этап 2а.1): мс на тик по системам, старт, память, сборка чанков 3D.
void drawMeasurements(const World& world, const WorldRenderer& renderer, const WorldRenderer3D& renderer3d,
                      const AppStats& stats) {
    // Окно копит всегда, даже свёрнутое: развернул — сразу свежие числа.
    static ProfileWindow window;
    window.sample(world.profiler(), ImGui::GetIO().DeltaTime);
    if (!ImGui::CollapsingHeader("Замер: системы, старт, память")) {
        return;
    }
    ImGui::Text("Кадр на процессоре: %.1f мс в среднем, худший за секунду %.1f мс; симуляция %.1f мс; %d кадров/с",
                stats.frameMsAverage, stats.frameMsWorst, stats.simMs, stats.fps);
    ImGui::Text("Тик: %.0f мкс (%.0f тиков/с чистой симуляции), потоков: %d", 1000.0 * window.msPerTick(),
                window.msPerTick() > 0.0 ? 1000.0 / window.msPerTick() : 0.0, parallelThreads());
    if (!world.profiler().enabled()) {
        ImGui::TextDisabled("Замер систем выключен");
    } else if (ImGui::BeginTable("##profile", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("система");
        ImGui::TableSetupColumn("мкс/тик");
        ImGui::TableSetupColumn("доля");
        ImGui::TableHeadersRow();
        std::vector<double> perTick(static_cast<std::size_t>(profileSectionCount()), -1.0);
        std::vector<double> share(perTick.size(), 0.0);
        for (const ProfileWindow::Row& row : window.rows()) {
            perTick[static_cast<std::size_t>(row.section)] = row.msPerTick;
            share[static_cast<std::size_t>(row.section)] = row.share;
        }
        for (const int section : profileSectionsInTreeOrder()) {
            const auto i = static_cast<std::size_t>(section);
            if (i >= perTick.size() || perTick[i] < 0.0005) {
                continue; // меньше полмикросекунды — не показывать
            }
            const std::string& name = profileSectionName(section);
            const std::size_t slash = name.rfind('/');
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent(12.0f * static_cast<float>(profileSectionDepth(section)) + 0.001f);
            ImGui::TextUnformatted(slash == std::string::npos ? name.c_str() : name.c_str() + slash + 1);
            ImGui::Unindent(12.0f * static_cast<float>(profileSectionDepth(section)) + 0.001f);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", 1000.0 * perTick[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f %%", 100.0 * share[i]);
        }
        ImGui::EndTable();
    }

    if (ImGui::TreeNode("Старт")) {
        ImGui::Text("Мир %.2f с, текстуры карты %.2f с, 3D %.2f с", stats.worldSeconds, stats.map2dSeconds,
                    stats.renderer3dSeconds);
        ImGui::Text("Память процесса после них: %.0f, %.0f, %.0f МБ", megabytes(stats.worldMemory),
                    megabytes(stats.map2dMemory), megabytes(stats.renderer3dMemory));
        for (const Profiler::StartupStep& step : world.profiler().startup()) {
            if (step.depth == 0) {
                continue;
            }
            const std::size_t slash = step.name.rfind('/');
            ImGui::Text("%*s%s — %.3f с", 2 * (step.depth - 1), "",
                        slash == std::string::npos ? step.name.c_str() : step.name.c_str() + slash + 1,
                        step.seconds);
        }
        ImGui::TreePop();
    }

    const ProcessMemory memory = processMemory();
    std::size_t structures = 0;
    const std::vector<MemoryItem> report = world.memoryReport();
    for (const MemoryItem& item : report) {
        structures += item.bytes;
    }
    if (ImGui::TreeNode("##memory", "Память: процесс %.0f МБ (пик %.0f), мир %.0f МБ, текстуры карты %.0f МБ",
                        megabytes(memory.workingSet), megabytes(memory.peakWorkingSet), megabytes(structures),
                        megabytes(renderer.map().textureBytes()))) {
        for (const MemoryItem& item : report) {
            ImGui::Text("%s — %.1f МБ", item.name.c_str(), megabytes(item.bytes));
        }
        ImGui::TreePop();
    }

    const Terrain3D& terrain = renderer3d.terrain();
    const Trees3D& trees = renderer3d.trees();
    const Props3D& props = renderer3d.props();
    ImGui::Text("3D, построено сеток: рельеф %d (%.2f с), деревья %d (%.2f с), мелочи %d (%.2f с)",
                terrain.meshesBuilt(), terrain.buildSeconds(), trees.meshesBuilt(), trees.buildSeconds(),
                props.meshesBuilt(), props.buildSeconds());
    ImGui::Text("В этом кадре: деревья %.1f мс, мелочи %.1f мс; в очереди %d", trees.buildMsLastFrame(),
                props.buildMsLastFrame(), renderer3d.pending());
    // Облик по требованию (этап 2а.2): что из карты сейчас в памяти.
    const Buildings3D& buildings = renderer3d.buildings();
    ImGui::Text("Здания: сеток чанков %d (%.2f с), в кадре чанков %d", buildings.meshesBuilt(),
                buildings.buildSeconds(), buildings.chunksDrawn());
    ImGui::Text("Подробных текстур карты: %d (испечено %d за %.2f с)", renderer.map().fineChunks(),
                renderer.map().fineBaked(), renderer.map().fineSeconds());
    ImGui::Text("Блоков высот: %d (считались %d раз), раскладка вне леса %.1f МБ", terrain.heightBlocks(),
                terrain.heightComputes(), megabytes(renderer3d.land().memoryBytes()));
}

// Уровни детализации симуляции (этап 6а): сколько существ на каждом и флажок
// слоя карты активности (только вид сверху).
void drawLodControls(const World& world) {
    if (!world.config().lod) {
        ImGui::TextDisabled("Детализация (LOD): выключена — все на полном уровне");
        return;
    }
    std::size_t counts[static_cast<std::size_t>(Detail::Count)] = {};
    for (const auto [entity, lod] : world.registry().view<const Lod>().each()) {
        ++counts[static_cast<std::size_t>(lod.level)];
    }
    ImGui::Text("Детализация: полная %zu, спокойная %zu, дремлющая %zu", counts[0], counts[1], counts[2]);
    bool overlay = lodOverlay();
    if (ImGui::Checkbox("Слой: детализация (вид сверху)", &overlay)) {
        setLodOverlay(overlay);
    }
    if (overlay) {
        ImGui::TextDisabled("красным — полная, жёлтым — спокойная, без заливки — дремлющая");
    }
}

} // namespace

void drawStatsPanel(const World& world, const Environment& environment, const SimDriver& driver,
                    CameraController& camera, const WorldRenderer& renderer,
                    WorldRenderer3D& renderer3d, const AppStats& stats) {
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
        // Группы и социальные действия (этапы 6.4–6.5).
        std::size_t inGroups = 0;
        for (const auto [entity, group] : world.registry().view<const Group>().each()) {
            inGroups += group.members.size();
        }
        std::size_t helping = 0;
        std::size_t avenging = 0;
        for (const auto [entity, mind] : world.registry().view<const Mind>().each()) {
            helping += mind.action == Action::Share || mind.action == Action::Trade ? 1 : 0;
            avenging += mind.action == Action::Avenge ? 1 : 0;
        }
        ImGui::Text("Групп: %zu (людей в них: %zu); делятся и меняются: %zu, мстят: %zu",
                    world.registry().view<const Group>().size(), inGroups, helping, avenging);
        // Мирная жизнь (этап 7.2).
        std::size_t working = 0;
        std::size_t shopping = 0;
        for (const auto [entity, mind] : world.registry().view<const Mind>().each()) {
            working += mind.action == Action::Work ? 1 : 0;
            shopping += mind.action == Action::Buy ? 1 : 0;
        }
        std::size_t shops = 0;
        std::size_t open = 0;
        for (const auto [entity, building, workplace] :
             world.registry().view<const Building, const Workplace>().each()) {
            if (building.kind == BuildingKind::Shop) {
                ++shops;
                open += workplace.onDuty > 0 ? 1 : 0;
            }
        }
        std::size_t employed = 0;
        for (const auto [entity, person, job] : world.registry().view<const Person, const Job>().each()) {
            employed += job.profession != Profession::None ? 1 : 0;
        }
        ImGui::Text("Работают: %zu (с профессией: %zu); за покупками: %zu; магазинов открыто: %zu из %zu",
                    working, employed, shopping, open, shops);
        drawMeasurements(world, renderer, renderer3d, stats);

        ImGui::Separator();
        drawCameraControls(camera);
        if (camera.is3d()) {
            const Terrain3D& terrain = renderer3d.terrain();
            ImGui::Text("Зданий в кадре: %d, существ: %d", renderer3d.buildingsDrawn(),
                        renderer3d.creaturesDrawn());
            ImGui::Text("Плиток рельефа: %d, треугольников: %d тыс.", terrain.tilesDrawn(),
                        terrain.trianglesDrawn() / 1000);
            ImGui::Text("Сеток в памяти: %d", terrain.meshesLoaded());
            bool trees = renderer3d.treesEnabled();
            if (ImGui::Checkbox("Деревья", &trees)) {
                renderer3d.setTreesEnabled(trees);
            }
            if (trees) {
                const Trees3D& forest = renderer3d.trees();
                ImGui::SameLine();
                ImGui::Text("в кадре %d, сеток %d (в памяти %d)", forest.treesDrawn(), forest.meshesDrawn(),
                            forest.meshesLoaded());
                // Облик листвы и снега на любой сезон — без промотки времени.
                bool preview = renderer3d.foliageYear().has_value();
                if (ImGui::Checkbox("Сезон вручную", &preview)) {
                    renderer3d.setFoliageYear(preview ? std::optional<float>(static_cast<float>(
                                                            environment.time.yearFraction))
                                                      : std::nullopt);
                }
                if (preview) {
                    float year = *renderer3d.foliageYear();
                    ImGui::SameLine();
                    if (ImGui::SliderFloat("##foliage", &year, 0.0f, 0.999f, "доля года %.2f")) {
                        renderer3d.setFoliageYear(year);
                    }
                }
            }
            bool props = renderer3d.propsEnabled();
            if (ImGui::Checkbox("Мелочи", &props)) {
                renderer3d.setPropsEnabled(props);
            }
            if (props) {
                ImGui::SameLine();
                ImGui::Text("посевы, трава, заборы, столбы: в кадре %d, сеток %d", renderer3d.props().itemsDrawn(),
                            renderer3d.props().meshesLoaded());
            }
            const SceneShading& shading = renderer3d.shading();
            const SceneLight& light = shading.light();
            ImGui::Text("Свет: %s %.2f, рассеянный %.2f", light.moonlit ? "луна" : "солнце",
                        std::max({light.lightColor.x, light.lightColor.y, light.lightColor.z}),
                        light.ambientSky.y);
            bool shadows = renderer3d.shadowsEnabled();
            if (ImGui::Checkbox("Тени", &shadows)) {
                renderer3d.setShadowsEnabled(shadows);
            }
            ImGui::SameLine();
            if (shading.shadowsOn()) {
                ImGui::Text("квадрат %.0f м", 2.0f * shading.shadowRadius());
            } else {
                ImGui::TextDisabled("в этом кадре нет");
            }
            float range = renderer3d.drawDistance();
            if (ImGui::SliderFloat("Дальность, м", &range, WorldRenderer3D::kMinDrawDistance,
                                   WorldRenderer3D::kMaxDrawDistance, "%.0f", ImGuiSliderFlags_Logarithmic)) {
                renderer3d.setDrawDistance(range);
            }
        } else {
            ImGui::Text("Чанков карты в кадре: %d из %d, подробных в памяти: %d", renderer.map().chunksDrawn(),
                        renderer.map().chunkCount(), renderer.map().fineChunks());
        }
        drawLodControls(world);
        ImGui::Text("Зум: %.2f пикс/м", camera.zoom());
        const std::optional<Vector2> cursor = camera.screenToWorld(GetMousePosition());
        if (!cursor) {
            ImGui::TextUnformatted("Курсор: над горизонтом");
            ImGui::End();
            return;
        }
        const Vector2 mouse = *cursor;
        ImGui::Text("Курсор: %.0f, %.0f м", mouse.x, mouse.y);
        if (camera.is3d()) {
            const Terrain3D& terrain = renderer3d.terrain();
            const float ground = terrain.groundAt(mouse.x, mouse.y);
            const float surface = terrain.surfaceAt(mouse.x, mouse.y);
            if (surface > ground + 0.05f) {
                ImGui::Text("Вода: %.1f м, глубина %.1f м", surface, surface - ground);
            } else {
                ImGui::Text("Высота: %.1f м", ground);
            }
        }
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
        const float pickRadius = std::max(1.0f, kPickPixels / camera.zoomAt(mouse));
        if (const auto picked = world.spatial().nearest(
                mouse.x, mouse.y, pickRadius, [&world](const SpatialHit& hit) {
                    return world.registry().any_of<Person, Zombie, Animal, Flock, Corpse>(hit.entity);
                })) {
            if (const auto* animal = world.registry().try_get<Animal>(picked->entity)) {
                ImGui::Text("Животное #%llu: %s, %s", static_cast<unsigned long long>(picked->id),
                            animalKindName(animal->kind), animalStateName(animal->state));
            } else if (const auto* corpse = world.registry().try_get<Corpse>(picked->entity)) {
                ImGui::Text("Тело %s: %s", idLabel(picked->id).c_str(),
                            corpse->kind == CorpseKind::Animal ? animalKindName(corpse->animal)
                                                               : corpseKindName(corpse->kind));
            } else if (const auto* flock = world.registry().try_get<Flock>(picked->entity)) {
                ImGui::Text("Стая #%llu: %d ворон%s", static_cast<unsigned long long>(picked->id),
                            flock->birds, flock->flying ? ", в воздухе" : "");
            } else if (const auto* zombie = world.registry().try_get<Zombie>(picked->entity)) {
                ImGui::Text("Зомби %s: %s", idLabel(picked->id).c_str(),
                            zombieStateName(zombie->state));
            } else {
                const auto* mind = world.registry().try_get<Mind>(picked->entity);
                ImGui::Text("Человек %s: %s", idLabel(picked->id).c_str(),
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

void drawEventsPanel(const World& world, SimDriver& driver, Inspector& inspector,
                     const CameraController& camera) {
    // Начатый по кнопке ивент — открыть в инспекторе, как только мир его начнёт.
    static EntityId opened = kInvalidId;
    if (driver.lastEvent() != kInvalidId && driver.lastEvent() != opened) {
        opened = driver.lastEvent();
        inspector.select(opened);
        inspector.setFollow(true);
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 12.0f, display.y * 0.5f}, ImGuiCond_FirstUseEver,
                            {1.0f, 0.0f});
    ImGui::SetNextWindowSize({320.0f, 0.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Сценарий и ивенты###events")) {
        ImGui::End();
        return;
    }
    const entt::registry& registry = world.registry();

    // Сценарий (этап 7.4): фаза мира и переход к следующей.
    ImGui::SeparatorText("Сценарий");
    if (const Scenario* scenario = scenarioOf(world)) {
        const EntityId id = scenarioIdOf(world);
        if (idLink(id)) {
            inspector.select(id);
        }
        ImGui::SameLine();
        char duration[32];
        formatGameDuration(duration, sizeof(duration),
                           world.tick() - scenario->began[static_cast<std::size_t>(scenario->phase)]);
        ImGui::Text("%s: %s, %s", scenarioKindName(scenario->kind), phaseName(scenario->phase),
                    duration);
        if (scenario->phase != WorldPhase::Survival) {
            char next[320];
            formatNextPhase(next, sizeof(next), world, *scenario);
            ImGui::TextWrapped("Дальше: %s", next);
            const WorldPhase following = nextPhase(scenario->phase);
            char label[96];
            if (scenario->phase == WorldPhase::Peace) {
                std::snprintf(label, sizeof(label), "Начать вспышку###nextPhase");
            } else {
                std::snprintf(label, sizeof(label), "Следующая фаза: %s###nextPhase",
                              phaseName(following));
            }
            if (ImGui::Button(label)) {
                driver.requestPhase(following);
            }
        }
        // Реакция общества (этап 7.6): что передают, где карантин, сколько уехало.
        if (scenario->broadcast != 0) {
            ImGui::TextWrapped("По радио: «%s»", broadcastText(static_cast<BroadcastKind>(scenario->broadcast)));
        }
        const ScenarioStats stats = scenarioStats(world);
        int alarmed = 0;
        for (const auto [entity, person, alarm] : registry.view<const Person, const Alarm>().each()) {
            alarmed += alarm.level >= kAlarmedLevel ? 1 : 0;
        }
        if (scenario->phase != WorldPhase::Peace) {
            ImGui::Text("Встревожены: %d; карантин: %zu; эвакуированы: %d; солдат: %d", alarmed,
                        registry.view<const Quarantine>().size(), stats.evacuated, stats.soldiers);
        }
    } else {
        ImGui::TextDisabled("Нет: мир начат без мирной жизни\nи живёт по правилам выживания");
    }

    ImGui::SeparatorText("Ивенты");
    if (world.config().events) {
        ImGui::TextWrapped("По расписанию (пока нет рассказчика): в среднем %.1f в сутки, днём",
                           kEventsPerDay);
    } else {
        ImGui::TextDisabled("Временное расписание выключено");
    }
    if (world.config().phaseEvents && scenarioOf(world) != nullptr) {
        // Этап 7.7: частоты ивентов в нынешней фазе.
        const PhaseRules& rules = rulesOf(world);
        float total = 0.0f;
        for (const float rate : rules.eventsPerDay) {
            total += rate;
        }
        if (total > 0.0f) {
            ImGui::TextWrapped("По фазе «%s», в сутки: груз %.2f, вертолёт %.2f, конвой %.2f",
                               phaseName(worldPhase(world)), rules.eventsPerDay[0], rules.eventsPerDay[1],
                               rules.eventsPerDay[2]);
        } else {
            ImGui::TextDisabled("В фазе «%s» ивентов нет", phaseName(worldPhase(world)));
        }
    }
    for (std::size_t i = 0; i < kEventKinds; ++i) {
        const auto kind = static_cast<EventKind>(i);
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%s:", eventKindName(kind));
        ImGui::SameLine();
        if (ImGui::Button("где выберет мир")) {
            driver.requestEvent(kind);
        }
        ImGui::SameLine();
        if (ImGui::Button("у центра экрана")) {
            const Vector2 center = camera.focus();
            driver.requestEvent(kind, world.map().clampedCellAt(center.x, center.y));
        }
        ImGui::PopID();
    }

    std::vector<std::pair<EntityId, const WorldEvent*>> events;
    for (const auto [entity, id, event] : registry.view<const Id, const WorldEvent>().each()) {
        events.emplace_back(id.value, &event);
    }
    std::sort(events.begin(), events.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    ImGui::SeparatorText("Идут");
    if (events.empty()) {
        ImGui::TextDisabled("нет");
    }
    char duration[32];
    for (const auto& [id, event] : events) {
        ImGui::PushID(static_cast<int>(id));
        if (idLink(id)) {
            inspector.select(id);
            inspector.setFollow(true);
        }
        ImGui::SameLine();
        int loaded = 0;
        for (const EntityId cache : event->caches) {
            const entt::entity crate = world.find(cache);
            const auto* stock = crate != entt::null ? registry.try_get<Inventory>(crate) : nullptr;
            loaded += stock != nullptr && !stock->empty() ? 1 : 0;
        }
        formatGameDuration(duration, sizeof(duration), world.tick() - event->began);
        ImGui::Text("%s, %s назад; грузов с добром %d из %zu%s", eventKindName(event->kind),
                    duration, loaded, event->caches.size(),
                    world.tick() < event->signalUntil ? ", дым" : "");
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace lw::ui
