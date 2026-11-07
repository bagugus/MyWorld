cmake_minimum_required(VERSION 3.24)
project(LivingWorld VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Без окна (сервер, CI) можно собрать только симуляцию: -DLW_BUILD_APP=OFF
option(LW_BUILD_APP "Build the windowed application (raylib + ImGui)" ON)
option(LW_BUILD_TESTS "Build tests" ON)

if(MSVC)
    add_compile_options(/MP)
endif()

include(cmake/Dependencies.cmake)

# Общие флаги нашего кода (на сторонние библиотеки не распространяются).
# /fp:fast и аналоги не включать: симуляция должна быть детерминированной.
add_library(lw_options INTERFACE)
if(MSVC)
    target_compile_options(lw_options INTERFACE /utf-8 /W4 /permissive-)
    target_compile_definitions(lw_options INTERFACE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(lw_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

# --- Симуляция -------------------------------------------------------------
# Зависит только от EnTT. raylib и ImGui сюда не попадают намеренно:
# include графики в src/sim/ не скомпилируется.
add_library(lw_sim STATIC
    src/sim/Calendar.cpp
    src/sim/Calendar.h
    src/sim/Cast.cpp
    src/sim/Cast.h
    src/sim/Chronicle.cpp
    src/sim/Chronicle.h
    src/sim/Climate.cpp
    src/sim/Climate.h
    src/sim/Death.cpp
    src/sim/Death.h
    src/sim/Events.cpp
    src/sim/Events.h
    src/sim/Family.cpp
    src/sim/Family.h
    src/sim/Foresight.cpp
    src/sim/Foresight.h
    src/sim/Components.h
    src/sim/Grid.h
    src/sim/Hash.h
    src/sim/Id.h
    src/sim/Items.cpp
    src/sim/Items.h
    src/sim/Map.cpp
    src/sim/Map.h
    src/sim/Memory.cpp
    src/sim/Memory.h
    src/sim/Perception.cpp
    src/sim/Perception.h
    src/sim/Places.cpp
    src/sim/Places.h
    src/sim/Names.cpp
    src/sim/Names.h
    src/sim/Population.cpp
    src/sim/Population.h
    src/sim/Relations.cpp
    src/sim/Relations.h
    src/sim/Resources.cpp
    src/sim/Resources.h
    src/sim/Rng.cpp
    src/sim/Rng.h
    src/sim/Scenario.cpp
    src/sim/Scenario.h
    src/sim/SimDriver.cpp
    src/sim/SimDriver.h
    src/sim/Sound.h
    src/sim/SpatialIndex.cpp
    src/sim/SpatialIndex.h
    src/sim/Story.cpp
    src/sim/Story.h
    src/sim/TimeScale.h
    src/sim/Traits.cpp
    src/sim/Traits.h
    src/sim/Weapons.cpp
    src/sim/Weapons.h
    src/sim/Weather.h
    src/sim/Work.cpp
    src/sim/Work.h
    src/sim/World.cpp
    src/sim/World.h
    src/sim/gen/Civic.cpp
    src/sim/gen/GenContext.h
    src/sim/gen/MapGenerator.cpp
    src/sim/gen/MapGenerator.h
    src/sim/gen/Roads.cpp
    src/sim/gen/Settlements.cpp
    src/sim/gen/Streets.cpp
    src/sim/gen/Terrain.cpp
    src/sim/nav/NavGraph.cpp
    src/sim/nav/NavGraph.h
    src/sim/nav/NavGrid.cpp
    src/sim/nav/NavGrid.h
    src/sim/nav/Pathfinder.cpp
    src/sim/nav/Pathfinder.h
    src/sim/nav/SearchSpace.h
    src/sim/systems/AnimalSystem.cpp
    src/sim/systems/AnimalSystem.h
    src/sim/systems/CombatSystem.cpp
    src/sim/systems/CombatSystem.h
    src/sim/systems/EventSystem.cpp
    src/sim/systems/EventSystem.h
    src/sim/systems/FamilySystem.cpp
    src/sim/systems/FamilySystem.h
    src/sim/systems/GroupSystem.cpp
    src/sim/systems/GroupSystem.h
    src/sim/systems/MemorySystem.cpp
    src/sim/systems/MemorySystem.h
    src/sim/systems/MovementSystem.cpp
    src/sim/systems/MovementSystem.h
    src/sim/systems/NeedsSystem.cpp
    src/sim/systems/NeedsSystem.h
    src/sim/systems/PerceptionSystem.cpp
    src/sim/systems/PerceptionSystem.h
    src/sim/systems/ResourceSystem.cpp
    src/sim/systems/ResourceSystem.h
    src/sim/systems/ScenarioSystem.cpp
    src/sim/systems/ScenarioSystem.h
    src/sim/systems/SocialSystem.cpp
    src/sim/systems/SocialSystem.h
    src/sim/systems/UtilityAiSystem.cpp
    src/sim/systems/UtilityAiSystem.h
    src/sim/systems/WeatherSystem.cpp
    src/sim/systems/WeatherSystem.h
    src/sim/systems/WorkSystem.cpp
    src/sim/systems/WorkSystem.h
    src/sim/systems/ZombieSystem.cpp
    src/sim/systems/ZombieSystem.h
)
target_include_directories(lw_sim PUBLIC src)
target_link_libraries(lw_sim PUBLIC EnTT::EnTT PRIVATE lw_options lw_fastnoiselite)

add_executable(lw_headless src/headless_main.cpp)
target_link_libraries(lw_headless PRIVATE lw_sim lw_options)

# --- Приложение с окном ----------------------------------------------------
if(LW_BUILD_APP)
    add_executable(living_world
        src/main.cpp
        src/render/Animator.cpp
        src/render/Animator.h
        src/render/CameraController.cpp
        src/render/CameraController.h
        src/render/FieldLayout.cpp
        src/render/FieldLayout.h
        src/render/Interior.cpp
        src/render/Interior.h
        src/render/InteriorStage.cpp
        src/render/InteriorStage.h
        src/render/LandUse.cpp
        src/render/LandUse.h
        src/render/Lighting.cpp
        src/render/Lighting.h
        src/render/Lighting3D.cpp
        src/render/Lighting3D.h
        src/render/MapRenderer.cpp
        src/render/MapRenderer.h
        src/render/Models3D.cpp
        src/render/Models3D.h
        src/render/Palette.cpp
        src/render/Palette.h
        src/render/Poses.cpp
        src/render/Poses.h
        src/render/Props3D.cpp
        src/render/Props3D.h
        src/render/Terrain3D.cpp
        src/render/Terrain3D.h
        src/render/TreeLayout.cpp
        src/render/TreeLayout.h
        src/render/Trees3D.cpp
        src/render/Trees3D.h
        src/render/WeatherRenderer.cpp
        src/render/WeatherRenderer.h
        src/render/WorldRenderer.cpp
        src/render/WorldRenderer.h
        src/render/WorldRenderer3D.cpp
        src/render/WorldRenderer3D.h
        src/ui/Camerawork.cpp
        src/ui/Camerawork.h
        src/ui/Captions.cpp
        src/ui/Captions.h
        src/ui/CinemaCamera.cpp
        src/ui/CinemaCamera.h
        src/ui/Composition.cpp
        src/ui/Composition.h
        src/ui/ChronicleView.cpp
        src/ui/ChronicleView.h
        src/ui/Director.cpp
        src/ui/Director.h
        src/ui/Format.cpp
        src/ui/Format.h
        src/ui/Inspector.cpp
        src/ui/Inspector.h
        src/ui/Narrator.cpp
        src/ui/Narrator.h
        src/ui/PathTool.cpp
        src/ui/PathTool.h
        src/ui/SeriesFrame.cpp
        src/ui/SeriesFrame.h
        src/ui/Showrunner.cpp
        src/ui/Showrunner.h
        src/ui/StoryText.cpp
        src/ui/StoryText.h
        src/ui/Ui.cpp
        src/ui/Ui.h
    )
    target_link_libraries(living_world PRIVATE lw_sim lw_imgui raylib lw_options)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT living_world)
endif()

# --- Тесты -----------------------------------------------------------------
if(LW_BUILD_TESTS)
    enable_testing()
    add_executable(lw_tests tests/determinism_test.cpp)
    target_link_libraries(lw_tests PRIVATE lw_sim lw_options)
    add_test(NAME sim_core COMMAND lw_tests)
    # Раскладка облика вне леса (этап 11.9) — без графики, только карта и здания.
    add_executable(lw_land_use_tests
        tests/land_use_test.cpp
        src/render/FieldLayout.cpp
        src/render/LandUse.cpp
        src/render/TreeLayout.cpp
    )
    target_link_libraries(lw_land_use_tests PRIVATE lw_sim lw_options)
    add_test(NAME land_use COMMAND lw_land_use_tests)
    # Позы и выбор движений (этап 12а.11) — без графики, только мир.
    add_executable(lw_animation_tests
        tests/animation_test.cpp
        src/render/Animator.cpp
        src/render/Poses.cpp
    )
    target_link_libraries(lw_animation_tests PRIVATE lw_sim lw_options)
    add_test(NAME animation COMMAND lw_animation_tests)
    # Комнаты внутри зданий (этап 12а.14) — без графики: планировка и рассадка.
    add_executable(lw_interior_tests
        tests/interior_test.cpp
        src/render/Interior.cpp
    )
    target_link_libraries(lw_interior_tests PRIVATE lw_sim lw_options)
    add_test(NAME interior COMMAND lw_interior_tests)
    # Ракурсы и композиция (этап 12а.12) — без графики: смысл сцены, правило
    # 180°, правило третей, цвет по настроению.
    add_executable(lw_composition_tests
        tests/composition_test.cpp
        src/ui/Composition.cpp
    )
    target_link_libraries(lw_composition_tests PRIVATE lw_sim lw_options)
    add_test(NAME composition COMMAND lw_composition_tests)
    if(TARGET lw_imgui)
        # Рассказчик (этап 12а.13) — без графики: знакомство, мотив сцены, связки,
        # реплики о характере, очередь голоса за кадром. Format.cpp тянет ImGui.
        add_executable(lw_narrator_tests
            tests/narrator_test.cpp
            src/ui/Captions.cpp
            src/ui/Format.cpp
            src/ui/Narrator.cpp
        )
        target_link_libraries(lw_narrator_tests PRIVATE lw_sim lw_imgui lw_options)
        add_test(NAME narrator COMMAND lw_narrator_tests)
    endif()
endif()
