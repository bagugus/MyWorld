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
    src/sim/Components.h
    src/sim/Grid.h
    src/sim/Hash.h
    src/sim/Id.h
    src/sim/Map.cpp
    src/sim/Map.h
    src/sim/Rng.cpp
    src/sim/Rng.h
    src/sim/SimDriver.cpp
    src/sim/SimDriver.h
    src/sim/World.cpp
    src/sim/World.h
    src/sim/gen/GenContext.h
    src/sim/gen/MapGenerator.cpp
    src/sim/gen/MapGenerator.h
    src/sim/gen/Roads.cpp
    src/sim/gen/Settlements.cpp
    src/sim/gen/Streets.cpp
    src/sim/gen/Terrain.cpp
    src/sim/systems/WanderSystem.cpp
    src/sim/systems/WanderSystem.h
)
target_include_directories(lw_sim PUBLIC src)
target_link_libraries(lw_sim PUBLIC EnTT::EnTT PRIVATE lw_options lw_fastnoiselite)

add_executable(lw_headless src/headless_main.cpp)
target_link_libraries(lw_headless PRIVATE lw_sim lw_options)

# --- Приложение с окном ----------------------------------------------------
if(LW_BUILD_APP)
    add_executable(living_world
        src/main.cpp
        src/render/CameraController.cpp
        src/render/CameraController.h
        src/render/MapRenderer.cpp
        src/render/MapRenderer.h
        src/render/WorldRenderer.cpp
        src/render/WorldRenderer.h
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
endif()
