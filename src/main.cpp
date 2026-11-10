// Living World — приложение с окном.
//   living_world [--seed N] [--event NAME] [--hours H] [--view X Y ZOOM] [--path X1 Y1 X2 Y2]
//                [--nav-graph] [--select ID] [--3d] [--orbit YAW PITCH] [--foliage F]
//                [--camera fly|shoulder] [--outbreak DAYS] [--phase NAME] [--zombies N]
//                [--no-peace] [--director] [--screenshot FILE.png] [--screenshot-delay S]
//                [--pose-gallery [ROW]] [--interior ID [YAW]] [--compose SENSE [ID [OTHER]]]
//                [--map-size CELLS] [--people N] [--animals N] [--flocks N] [--threads N]
//
// --event       начать ивент на старте (drop — сброс груза, crash — крушение вертолёта,
//               convoy — застрявший конвой) и открыть его в инспекторе;
//               --hours прокручивает мир уже после него
//
// --hours       перед открытием окна прогнать мир на H игровых часов (посмотреть ночь, погоду)
// --view        начальное положение камеры: центр в метрах и зум в пикселях на метр
// --path        проверить поиск пути из точки (X1, Y1) в (X2, Y2), метры
// --nav-graph   показать граф навигации (дороги и местность)
// --select      открыть объект в инспекторе и следить за ним камерой
// --3d          начать с 3D-вида (V переключает вид); --view задаёт точку и зум и в 3D
// --orbit       3D-вид под углом: YAW — с какой стороны (0 — с юга, 90 — с востока),
//               PITCH — наклон над горизонтом, градусы
// --camera      начать с полёта (с той же точки, что и 3D-вид) или из-за плеча
//               выбранного (--select)
// --foliage     облик сезона в 3D (листва и снег) — как на долю года F (0 — 1 марта,
//               0,3 — лето, 0,6 — осень, 0,9 — зима), а не по календарю; только облик
// --outbreak    вспышка через DAYS игровых суток от начала мира (0 — не по расписанию)
// --phase       сразу перевести мир в фазу: outbreak, panic, collapse, survival (этап 7.4);
//               --hours прокручивает мир уже после этого
// --director    режим «Сериал» (этап 12а.7): кино в 3D во весь экран — камеру ведёт режиссёр
//               по самой интересной сюжетной линии, окна спрятаны; Esc — выход, F11 — снова
// --zombies, --no-peace, --map-size, --people, --animals, --flocks, --threads — как у
//               lw_headless (--map-size 4096 — карта 16 × 16 км, этап 2а.1)
// --screenshot  нарисовать несколько кадров (в 3D — пока не достроятся деревья),
//               сохранить снимок окна и выйти
// --screenshot-delay  снять не сразу, а через S реальных секунд работы окна (режиссёр)
// --pose-gallery витрина поз в 3D (этап 12а.11): все движения людей, зомби и зверей
//               рядами в точке взгляда камеры — посмотреть, не дожидаясь их в мире;
//               ROW — только этот ряд из восьми (крупно), последний ряд — звери
// --interior    комнаты здания в 3D (этап 12а.14): кинокамера внутри здания ID (или дома
//               человека ID; 0 — дом, где сейчас больше всего людей не спит), курс YAW
//               (градусы, как у --orbit), жильцы — по своим местам; посмотреть
//               планировку, мебель и свет, не дожидаясь сцены дома в сериале
// --compose     приём кинокамеры (этап 12а.12) на человека ID (и второго OTHER), не
//               дожидаясь такой сцены в сериале: SENSE — daily, pan, talk, chase, fight,
//               ambush, threat, menace, grief, helpless, family; без ID — самая близкая
//               пара на улице (для ambush, threat, chase — человек и ближайший зомби)

#include "render/CameraController.h"
#include "render/Palette.h"
#include "render/WeatherRenderer.h"
#include "render/WorldRenderer.h"
#include "render/WorldRenderer3D.h"
#include "sim/Climate.h"
#include "sim/Parallel.h"
#include "sim/Profiler.h"
#include "sim/SimDriver.h"
#include "sim/Events.h"
#include "sim/Scenario.h"
#include "sim/World.h"
#include "sim/systems/EventSystem.h"
#include "sim/systems/ScenarioSystem.h"
#include "ui/ChronicleView.h"
#include "ui/Director.h"
#include "ui/Format.h"
#include "ui/Inspector.h"
#include "ui/LodOverlay.h"
#include "ui/CinemaCamera.h"
#include "ui/Composition.h"
#include "ui/PathTool.h"
#include "ui/Showrunner.h"
#include "ui/Ui.h"
#include "util/ProcessMemory.h"

#include <raylib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

namespace {

// --interior: какое здание открыть и за кем смотреть. id — здание или человек;
// 0 — жилой дом, где сейчас больше всего людей дома и не спят (ужин — лучше).
std::pair<lw::EntityId, lw::EntityId> interiorTarget(const lw::World& world, lw::EntityId id) {
    const entt::registry& registry = world.registry();
    if (id != 0) {
        const entt::entity entity = world.find(id);
        if (entity != entt::null && registry.all_of<lw::Person>(entity)) {
            return {registry.get<lw::Person>(entity).home, id};
        }
        lw::EntityId focus = lw::kInvalidId;
        for (const auto [other, person, otherId] : registry.view<const lw::Person, const lw::Id>().each()) {
            if (person.home == id && (focus == lw::kInvalidId || otherId.value < focus)) {
                focus = otherId.value;
            }
        }
        return {id, focus};
    }
    std::unordered_map<lw::EntityId, float> score;
    std::unordered_map<lw::EntityId, lw::EntityId> focus;
    for (const auto [entity, person, otherId] : registry.view<const lw::Person, const lw::Id>().each()) {
        if (!person.indoors || person.home == lw::kInvalidId) {
            continue;
        }
        const auto* mind = registry.try_get<lw::Mind>(entity);
        const bool eating = mind != nullptr && mind->action == lw::Action::Eat;
        float& value = score[person.home];
        value += person.asleep ? 0.6f : eating ? 2.5f : 1.5f;
        if (!person.asleep && (focus.find(person.home) == focus.end() || eating)) {
            focus[person.home] = otherId.value;
        } else if (focus.find(person.home) == focus.end()) {
            focus[person.home] = otherId.value;
        }
    }
    lw::EntityId best = lw::kInvalidId;
    float bestScore = 0.0f;
    for (const auto& [building, value] : score) {
        if (value > bestScore || (value == bestScore && building < best)) {
            bestScore = value;
            best = building;
        }
    }
    return {best, best != lw::kInvalidId ? focus[best] : lw::kInvalidId};
}

// Кинокамера внутри открытого здания: на того, за кем смотрим, курсом yaw.
void showInterior(const lw::World& world, lw::WorldRenderer3D& renderer, lw::CameraController& camera,
                  std::pair<lw::EntityId, lw::EntityId> target, float yaw, bool first) {
    lw::InteriorStage& stage = renderer.interiors();
    stage.open(target.first, target.second);
    stage.update(world);
    const lw::InteriorLayout* layout = stage.layout();
    if (layout == nullptr) {
        return;
    }
    lw::CinemaShot shot;
    shot.indoor = true;
    shot.floor = layout->floor;
    shot.ceiling = layout->ceiling;
    shot.bounds = {layout->x0, layout->z0, layout->x1, layout->z1};
    if (const lw::Placement* placed = stage.placementOf(target.second)) {
        shot.anchorId = target.second;
        shot.anchor = {placed->x, placed->z};
        shot.height = placed->what == lw::Occupation::Sleep ? 0.75f : placed->height > 0.3f ? 1.0f : 1.3f;
    } else {
        shot.anchor = {(layout->x0 + layout->x1) * 0.5f, (layout->z0 + layout->z1) * 0.5f};
        shot.height = 1.2f;
    }
    shot.yaw = yaw;
    shot.pitch = 14.0f;
    shot.distance = 4.0f;
    shot.fovY = 62.0f;
    shot.smoothing = 0.0f;
    shot.cut = first;
    if (camera.mode() != lw::ViewMode::Cinema) {
        camera.setMode(lw::ViewMode::Cinema);
    }
    camera.setCinemaShot(shot);
}

// --compose: смысл сцены, который снимает кинокамера.
struct ComposeRequest {
    lw::ui::SceneSense sense = lw::ui::SceneSense::Daily;
    bool pan = false;
    bool menace = false;
    lw::EntityId hero = 0;
    lw::EntityId other = 0;
};

std::optional<ComposeRequest> parseCompose(const char* name) {
    using lw::ui::SceneSense;
    const std::pair<const char*, SceneSense> names[] = {
        {"daily", SceneSense::Daily},       {"pan", SceneSense::Daily},     {"talk", SceneSense::Talk},
        {"chase", SceneSense::Chase},       {"fight", SceneSense::Fight},   {"ambush", SceneSense::Ambush},
        {"threat", SceneSense::Threat},     {"menace", SceneSense::Threat}, {"grief", SceneSense::Grief},
        {"helpless", SceneSense::Helpless}, {"family", SceneSense::Family}};
    for (const auto& [text, sense] : names) {
        if (std::strcmp(name, text) == 0) {
            ComposeRequest request;
            request.sense = sense;
            request.pan = std::strcmp(name, "pan") == 0;
            request.menace = std::strcmp(name, "menace") == 0;
            return request;
        }
    }
    return std::nullopt;
}

// Без ID: самая близкая пара на улице — двое живых людей, а для засады, угрозы и
// погони — человек и зомби; для панорамы — идущий.
void pickComposeCast(const lw::World& world, ComposeRequest& request) {
    const entt::registry& registry = world.registry();
    const bool zombie = request.sense == lw::ui::SceneSense::Ambush || request.sense == lw::ui::SceneSense::Chase ||
                        (request.sense == lw::ui::SceneSense::Threat && !request.menace);
    const bool alone = request.pan || request.sense == lw::ui::SceneSense::Daily ||
                       request.sense == lw::ui::SceneSense::Grief || request.sense == lw::ui::SceneSense::Helpless;
    const lw::EntityId wantedHero = request.hero;
    float best = 1e9f;
    for (const auto [entity, person, id, at] :
         registry.view<const lw::Person, const lw::Id, const lw::Position>().each()) {
        if (person.indoors || person.asleep || registry.any_of<lw::Zombie, lw::Corpse>(entity)) {
            continue;
        }
        if (wantedHero != 0 && id.value != wantedHero) {
            continue;
        }
        if (alone) {
            // Для панорамы — идущий, для остального — стоящий; иначе первый.
            const auto* velocity = registry.try_get<lw::Velocity>(entity);
            const bool moving = velocity != nullptr && std::hypot(velocity->x, velocity->y) > 0.5f;
            const float score = moving == request.pan ? 0.0f : 1.0f;
            if (score < best) {
                best = score;
                request.hero = id.value;
            }
            continue;
        }
        world.spatial().forEachInRadius(at.x, at.y, 60.0f, [&](const lw::SpatialHit& hit) {
            if (hit.id == id.value || registry.all_of<lw::Corpse>(hit.entity)) {
                return;
            }
            const bool isZombie = registry.all_of<lw::Zombie>(hit.entity);
            const auto* other = registry.try_get<lw::Person>(hit.entity);
            if (zombie ? !isZombie : (isZombie || other == nullptr || other->indoors)) {
                return;
            }
            if (request.other != 0 && hit.id != request.other) {
                return;
            }
            const float d = std::sqrt(hit.distanceSq);
            if (d < best) {
                best = d;
                request.hero = id.value;
                request.other = hit.id;
            }
        });
    }
    TraceLog(LOG_INFO, "COMPOSE: %s, hero %llu, other %llu (%.1f)", lw::ui::sceneSenseTitle(request.sense),
             static_cast<unsigned long long>(request.hero), static_cast<unsigned long long>(request.other), best);
}

// Кадр кинокамеры по приёму из --compose.
void showCompose(const lw::World& world, const ComposeRequest& request, lw::ui::CinemaCamera& cinema,
                 lw::CameraController& camera, std::optional<float> heading, float alpha, float dt) {
    lw::ui::ShotPlan plan;
    if (!lw::ui::positionOf(world, request.hero, alpha, plan.subject)) {
        return;
    }
    plan.valid = true;
    plan.subjectId = request.hero;
    plan.sense = request.sense;
    plan.size = lw::ui::ShotSize::Close;
    if (request.other != 0 && lw::ui::positionOf(world, request.other, alpha, plan.other)) {
        plan.otherId = request.other;
        plan.size = lw::ui::ShotSize::Medium;
        plan.extra.push_back(plan.other);
    }
    const lw::ui::SceneReading reading = lw::ui::readScene(world, request.hero);
    plan.moving = reading.moving || request.pan;
    plan.move = reading.moving ? Vector2{reading.moveX, reading.moveY} : Vector2{1.0f, 0.0f};
    plan.panic = request.sense == lw::ui::SceneSense::Chase || reading.panic;
    plan.menacing = request.menace;
    if (camera.mode() != lw::ViewMode::Cinema) {
        camera.setMode(lw::ViewMode::Cinema);
    }
    cinema.update(camera, plan, heading, false, false, dt);
}

// Замер для журнала при снимке (этап 2а.1): старт, кадр, память, сборка 3D —
// чтобы сравнивать размеры карты, не глядя в окно.
void logStats(const lw::World& world, const lw::WorldRenderer& renderer, const lw::WorldRenderer3D& renderer3d,
              const lw::ui::AppStats& stats) {
    const auto mb = [](std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    std::size_t structures = 0;
    for (const lw::MemoryItem& item : world.memoryReport()) {
        structures += item.bytes;
    }
    const lw::ProcessMemory memory = lw::processMemory();
    const lw::Terrain3D& terrain = renderer3d.terrain();
    const lw::Trees3D& trees = renderer3d.trees();
    const lw::Props3D& props = renderer3d.props();
    std::printf("STATS: map %dx%d, start: world %.2f s, map textures %.2f s, 3d %.2f s\n", world.map().width(),
                world.map().height(), stats.worldSeconds, stats.map2dSeconds, stats.renderer3dSeconds);
    std::printf("STATS: process memory after start steps: world %.0f MB, map textures %.0f MB, 3d %.0f MB\n",
                mb(stats.worldMemory), mb(stats.map2dMemory), mb(stats.renderer3dMemory));
    std::printf("STATS: frame on CPU %.1f ms average, %.1f ms worst in a second (simulation %.1f ms), "
                "%d FPS, threads %d\n",
                stats.frameMsAverage, stats.frameMsWorst, stats.simMs, stats.fps, lw::parallelThreads());
    std::printf("STATS: memory: process %.0f MB (peak %.0f), world structures %.0f MB, map textures %.0f MB\n",
                mb(memory.workingSet), mb(memory.peakWorkingSet), mb(structures), mb(renderer.map().textureBytes()));
    std::printf("STATS: 3d meshes built: terrain %d (%.2f s, loaded %d), trees %d (%.2f s, loaded %d), "
                "props %d (%.2f s, loaded %d), pending %d\n",
                terrain.meshesBuilt(), terrain.buildSeconds(), terrain.meshesLoaded(), trees.meshesBuilt(),
                trees.buildSeconds(), trees.meshesLoaded(), props.meshesBuilt(), props.buildSeconds(),
                props.meshesLoaded(), renderer3d.pending());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    double hours = 0.0;
    const char* screenshot = nullptr;
    float screenshotDelay = 0.0f;
    bool hasView = false;
    float viewX = 0.0f;
    float viewY = 0.0f;
    float viewZoom = 1.0f;
    bool hasPath = false;
    float path[4] = {};
    bool navGraph = false;
    bool director = false;
    bool noUi = false;    // --no-ui: без окон и подписей (снимки мира)
    bool noProps = false; // --no-props: без мелочи вне леса (сравнить облик)
    std::optional<int> poseGallery; // --pose-gallery [ROW]: витрина поз
    std::optional<lw::EntityId> interiorId; // --interior ID [YAW]: комнаты здания
    float interiorYaw = 30.0f;
    std::optional<ComposeRequest> compose; // --compose SENSE [ID [OTHER]]: приём кинокамеры
    std::uint64_t selectId = 0;
    bool view3d = false;
    std::optional<std::pair<float, float>> orbit;
    std::optional<float> foliage;
    std::optional<lw::ViewMode> startView; // --camera: полёт или из-за плеча
    std::optional<lw::EventKind> event;
    std::optional<lw::WorldPhase> phase; // --phase: сразу в эту фазу мира
    lw::WorldConfig config;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--hours") == 0 && i + 1 < argc) {
            hours = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--screenshot-delay") == 0 && i + 1 < argc) {
            screenshotDelay = std::strtof(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--view") == 0 && i + 3 < argc) {
            viewX = std::strtof(argv[++i], nullptr);
            viewY = std::strtof(argv[++i], nullptr);
            viewZoom = std::strtof(argv[++i], nullptr);
            hasView = true;
        } else if (std::strcmp(argv[i], "--path") == 0 && i + 4 < argc) {
            for (float& value : path) {
                value = std::strtof(argv[++i], nullptr);
            }
            hasPath = true;
        } else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            selectId = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--event") == 0 && i + 1 < argc) {
            event = lw::eventKindByName(argv[++i]);
        } else if (std::strcmp(argv[i], "--3d") == 0) {
            view3d = true;
        } else if (std::strcmp(argv[i], "--orbit") == 0 && i + 2 < argc) {
            const float yaw = std::strtof(argv[++i], nullptr);
            orbit = {{yaw, std::strtof(argv[++i], nullptr)}};
            view3d = true;
        } else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            const char* name = argv[++i];
            if (std::strcmp(name, "fly") == 0) {
                startView = lw::ViewMode::Fly;
            } else if (std::strcmp(name, "shoulder") == 0) {
                startView = lw::ViewMode::Shoulder;
            }
            view3d = true;
        } else if (std::strcmp(argv[i], "--foliage") == 0 && i + 1 < argc) {
            foliage = std::strtof(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--no-ui") == 0) {
            noUi = true;
        } else if (std::strcmp(argv[i], "--no-props") == 0) {
            noProps = true;
        } else if (std::strcmp(argv[i], "--pose-gallery") == 0) {
            poseGallery = -1;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])) != 0) {
                poseGallery = std::atoi(argv[++i]);
            }
            view3d = true;
        } else if (std::strcmp(argv[i], "--compose") == 0 && i + 1 < argc) {
            compose = parseCompose(argv[++i]);
            if (compose && i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                compose->hero = std::strtoull(argv[++i], nullptr, 10);
                if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                    compose->other = std::strtoull(argv[++i], nullptr, 10);
                }
            }
        } else if (std::strcmp(argv[i], "--interior") == 0 && i + 1 < argc) {
            interiorId = std::strtoull(argv[++i], nullptr, 10);
            if (i + 1 < argc && (std::isdigit(static_cast<unsigned char>(argv[i + 1][0])) != 0 || argv[i + 1][0] == '-') &&
                argv[i + 1][1] != '-') {
                interiorYaw = std::strtof(argv[++i], nullptr);
            }
            view3d = true;
        } else if (std::strcmp(argv[i], "--director") == 0) {
            director = true;
        } else if (std::strcmp(argv[i], "--nav-graph") == 0) {
            navGraph = true;
        } else if (std::strcmp(argv[i], "--zombies") == 0 && i + 1 < argc) {
            config.zombies = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--map-size") == 0 && i + 1 < argc) {
            config.mapWidth = config.mapHeight = std::max(64, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--people") == 0 && i + 1 < argc) {
            config.people = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--animals") == 0 && i + 1 < argc) {
            config.animals = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--flocks") == 0 && i + 1 < argc) {
            config.flocks = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            lw::setParallelThreads(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-peace") == 0) {
            config.peacetime = false;
        } else if (std::strcmp(argv[i], "--outbreak") == 0 && i + 1 < argc) {
            config.outbreakDays = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--phase") == 0 && i + 1 < argc) {
            phase = lw::phaseByName(argv[++i]);
        } else if (std::strcmp(argv[i], "--lod") == 0 && i + 1 < argc) {
            config.lod = std::strcmp(argv[++i], "off") != 0; // LOD симуляции (этап 6а)
        } else if (std::strcmp(argv[i], "--lod-map") == 0) {
            lw::ui::setLodOverlay(true); // слой детализации в виде сверху
        }
    }

    // Мир создаётся до окна: генерация карты занимает пару секунд, и пустое
    // окно в это время выглядело бы зависшим.
    lw::ui::AppStats stats;
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point from) {
        return std::chrono::duration<double>(Clock::now() - from).count();
    };
    Clock::time_point mark = Clock::now();
    lw::World world(seed, config);
    stats.worldSeconds = seconds(mark);
    stats.worldMemory = lw::processMemory().workingSet;
    // Замер систем тика (этап 2а.1) — всегда: два десятка засечек часов на тик.
    world.setProfiling(true);
    if (phase) {
        lw::enterPhase(world, *phase);
    }
    if (event) {
        const lw::EntityId id = lw::startEvent(world, *event);
        if (selectId == 0) {
            selectId = id;
        }
    }
    const auto skipTicks = static_cast<std::uint64_t>(std::max(0.0, hours) * lw::kTicksPerGameHour);
    for (std::uint64_t i = 0; i < skipTicks; ++i) {
        world.step();
    }
    lw::SimDriver driver(world);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(1600, 900, "Living World");
    SetExitKey(KEY_NULL); // Esc не закрывает окно

    {
        // Текстуры карты должны быть освобождены до CloseWindow.
        mark = Clock::now();
        lw::WorldRenderer renderer(world);
        stats.map2dSeconds = seconds(mark);
        stats.map2dMemory = lw::processMemory().workingSet;
        mark = Clock::now();
        lw::WorldRenderer3D renderer3d(renderer.map(), world.map());
        stats.renderer3dSeconds = seconds(mark);
        stats.renderer3dMemory = lw::processMemory().workingSet;
        double worstWindow = 0.0; // худший кадр текущей секунды
        double worstSince = 0.0;
        int framesWindow = 0;
        lw::WeatherRenderer weatherRenderer;
        lw::CameraController camera(world.width(), world.height());
        camera.setTerrain(&renderer3d.terrain());
        renderer3d.updateBuildingBounds(world);
        camera.setObstacles(&renderer3d.buildingBounds()); // кинокамера не заходит в стены
        lw::ui::loadCameraSettings(camera.settings());
        renderer3d.setFoliageYear(foliage);
        renderer3d.setPropsEnabled(!noProps);
        renderer3d.setPoseGallery(poseGallery);
        if (orbit) {
            camera.setOrbitAngles(orbit->first, orbit->second);
        }
        if (view3d) {
            camera.setMode(lw::ViewMode::Orbit);
        }
        if (hasView) {
            camera.lookAt(viewX, viewY, viewZoom);
        }
        camera.snap(); // с командной строки — сразу, без плавного перехода
        lw::ui::init();
        lw::ui::PathTool pathTool(world);
        pathTool.setShowGraph(navGraph);
        if (hasPath) {
            pathTool.setEndpoints(path[0], path[1], path[2], path[3]);
        }
        lw::ui::Inspector inspector(world);
        if (selectId != 0) {
            inspector.select(selectId);
            inspector.setFollow(true);
        }
        lw::ui::Director storyDirector(world);
        storyDirector.setInteriors(&renderer3d.interiors()); // сцены дома — в комнатах (12а.14)
        storyDirector.setFullscreenAllowed(screenshot == nullptr); // снимок — в окне того же размера
        if (director) {
            storyDirector.requestSeries(true); // сериал сам включает режиссёра, Esc — выключает
        }

        lw::ui::CinemaCamera composer; // --compose
        constexpr int kScreenshotFrame = 3; // первые кадры уходят на загрузку шрифта
        constexpr int kScreenshotWait = 600; // в 3D — ждать, пока достроятся деревья, но не дольше
        for (int frame = 0; !WindowShouldClose(); ++frame) {
            const float dt = GetFrameTime();
            const Clock::time_point frameStart = Clock::now();

            lw::ui::handleHotkeys(driver);
            storyDirector.handleHotkeys(!lw::ui::wantsKeyboard()); // F11 — сериал, Esc — выход
            // В сериале мир и камера ввода не получают: смотрим кино.
            if (!storyDirector.series()) {
                // Щелчок — по тому, что было на экране в прошлом кадре.
                pathTool.handleInput(camera, !lw::ui::wantsMouse());
                inspector.handleInput(camera, !lw::ui::wantsMouse());
            }
            const Clock::time_point simStart = Clock::now();
            driver.advance(dt);
            stats.simMs = 1000.0 * seconds(simStart);
            const float alpha = driver.interpolationAlpha();
            storyDirector.update(inspector, driver, dt);
            // Камера — после шага мира: слежение и вид из-за плеча идут вровень
            // с отрисовкой этого кадра.
            inspector.updateCamera(camera, alpha);
            std::optional<lw::CameraSubject> subject = inspector.cameraSubject(alpha);
            if (subject) {
                if (const std::optional<float> heading = renderer3d.drawnHeading(inspector.selected())) {
                    subject->heading = *heading;
                }
            }
            // План кадра: пролёты в виде сверху, кинокамера в сериале.
            storyDirector.updateCamera(camera, subject ? std::optional<float>(subject->heading) : std::nullopt,
                                       alpha, dt);
            const bool series = storyDirector.series();
            if (interiorId && !series) {
                static std::pair<lw::EntityId, lw::EntityId> target = interiorTarget(world, *interiorId);
                showInterior(world, renderer3d, camera, target, interiorYaw, frame == 0);
            }
            if (compose && !series) {
                if (frame == 0) {
                    pickComposeCast(world, *compose);
                    composer.reset();
                }
                showCompose(world, *compose, composer, camera, renderer3d.drawnHeading(compose->hero), alpha, dt);
            }
            camera.setSubject(subject);
            if (frame == 0 && startView && !series) {
                camera.setMode(*startView); // из-за плеча — когда выбранный уже известен
            }
            camera.update(dt, !series && !lw::ui::wantsMouse(), !series && !lw::ui::wantsKeyboard());
            if (frame == 0) {
                camera.snap(); // снимок с командной строки — без плавного подлёта
            }
            lw::ui::saveCameraSettings(camera.settings());
            weatherRenderer.update(dt, !driver.paused());
            const lw::Environment environment = lw::environmentAt(world);

            BeginDrawing();
            // Скачок времени в сериале (12а.10): кадр чёрный — мир не рисуем,
            // кадр короче и промотка идёт быстрее.
            const bool blind = storyDirector.blind();
            if (blind) {
                ClearBackground(BLACK);
            } else if (camera.is3d()) {
                ClearBackground(lw::kSkyColor);
                renderer3d.setHighlight(series ? lw::kInvalidId : inspector.selected()); // в кино меток нет
                // Цвет кадра по настроению сцены (12а.12): только в сериале.
                const lw::ui::ColorGrade grade =
                    compose && !series ? lw::ui::gradeOf(composer.sense(), false, 0.0f) : storyDirector.grade();
                renderer3d.setGrade({{grade.tintR, grade.tintG, grade.tintB}, grade.desaturate, grade.contrast,
                                     grade.vignette});
                renderer3d.draw(world, camera, alpha);
            } else {
                ClearBackground({16, 18, 22, 255});
                BeginMode2D(camera.camera());
                renderer.draw(world, camera.camera(), alpha);
                EndMode2D();
            }
            if (!blind) {
                weatherRenderer.draw(environment, world.weather(), camera.is3d(), renderer3d.interiorView(camera));
            }
            if (!series) {
                // Отладочные слои — поверх погоды, чтобы их было видно и ночью.
                camera.beginOverlay();
                lw::ui::drawLodOverlay(world, camera); // детализация (этап 6а), под остальными
                pathTool.drawOverlay(camera.camera());
                inspector.drawOverlay(camera.camera(), alpha);
                camera.endOverlay();
            }

            lw::ui::beginFrame();
            lw::ui::setNameSource(&world);
            if (!series && !noUi) {
                lw::ui::drawSettlementLabels(world, camera);
                lw::ui::drawTimePanel(world, environment, driver);
                lw::ui::drawStatsPanel(world, environment, driver, camera, renderer, renderer3d, stats);
                pathTool.drawPanel();
                inspector.drawPanel();
                lw::ui::drawKeyHelp(camera);
                lw::ui::drawEventsPanel(world, driver, inspector, camera);
                lw::ui::drawChroniclePanel(world, inspector);
                storyDirector.drawPanel(inspector);
            }
            storyDirector.drawCaption(inspector); // в сериале — широкий кадр и титры
            lw::ui::endFrame();
            // До EndDrawing: там ожидание вертикальной синхронизации.
            stats.frameMs = 1000.0 * seconds(frameStart);
            stats.frameMsAverage = stats.frameMsAverage == 0.0
                                       ? stats.frameMs
                                       : stats.frameMsAverage + (stats.frameMs - stats.frameMsAverage) *
                                                                    std::min(1.0, static_cast<double>(dt));
            worstWindow = std::max(worstWindow, stats.frameMs);
            worstSince += dt;
            ++framesWindow;
            if (worstSince >= 1.0) {
                stats.frameMsWorst = worstWindow;
                stats.fps = static_cast<int>(std::lround(framesWindow / worstSince));
                worstWindow = 0.0;
                worstSince = 0.0;
                framesWindow = 0;
            }

            EndDrawing();

            const bool settled = !camera.is3d() || renderer3d.pending() == 0;
            if (screenshot != nullptr && frame >= kScreenshotFrame && (settled || frame >= kScreenshotWait) &&
                GetTime() >= screenshotDelay) {
                TraceLog(LOG_INFO, "SCREENSHOT: frame %d, %d FPS", frame, GetFPS());
                logStats(world, renderer, renderer3d, stats);
                const Image image = LoadImageFromScreen();
                ExportImage(image, screenshot);
                UnloadImage(image);
                break;
            }
        }

        lw::ui::shutdown();
    }
    CloseWindow();
    return 0;
}
