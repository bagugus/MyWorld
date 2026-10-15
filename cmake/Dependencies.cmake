include(FetchContent)

# Версии зафиксированы: обновлять осознанно, одной правкой здесь.
set(LW_ENTT_VERSION    v3.16.0)
set(LW_FASTNOISELITE_VERSION v1.1.1)
set(LW_RAYLIB_VERSION  6.0)
set(LW_IMGUI_VERSION   v1.92.9b-docking)
set(LW_RLIMGUI_COMMIT  1550009359ad975927f7f0e4a3f47e3f27123ea9)

# --- EnTT (ECS, только заголовки) ------------------------------------------
FetchContent_Declare(entt
    URL https://github.com/skypjack/entt/archive/refs/tags/${LW_ENTT_VERSION}.tar.gz
)
FetchContent_MakeAvailable(entt)

# --- FastNoiseLite (шум для генерации карты, один заголовок) -----------------
# Подключается как SYSTEM: предупреждения чужого кода нас не интересуют.
FetchContent_Declare(fastnoiselite
    URL https://github.com/Auburn/FastNoiseLite/archive/refs/tags/${LW_FASTNOISELITE_VERSION}.tar.gz
    SOURCE_SUBDIR _no_cmake
)
FetchContent_MakeAvailable(fastnoiselite)
add_library(lw_fastnoiselite INTERFACE)
target_include_directories(lw_fastnoiselite SYSTEM INTERFACE ${fastnoiselite_SOURCE_DIR}/Cpp)

if(NOT LW_BUILD_APP)
    return()
endif()

# --- raylib ----------------------------------------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/${LW_RAYLIB_VERSION}.tar.gz
)
FetchContent_MakeAvailable(raylib)

# --- Dear ImGui + rlImGui --------------------------------------------------
# Своего CMake у них нет (SOURCE_SUBDIR указывает в никуда, чтобы CMake
# не пытался добавить их как подпроекты) — собираем одной библиотекой.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/${LW_IMGUI_VERSION}.tar.gz
    SOURCE_SUBDIR _no_cmake
)
FetchContent_Declare(rlimgui
    URL https://github.com/raylib-extras/rlImGui/archive/${LW_RLIMGUI_COMMIT}.tar.gz
    SOURCE_SUBDIR _no_cmake
)
FetchContent_MakeAvailable(imgui rlimgui)

add_library(lw_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp
)
target_include_directories(lw_imgui PUBLIC ${imgui_SOURCE_DIR} ${rlimgui_SOURCE_DIR})
target_compile_definitions(lw_imgui PUBLIC NO_FONT_AWESOME)
target_link_libraries(lw_imgui PUBLIC raylib)
set_target_properties(lw_imgui PROPERTIES FOLDER "third_party")
