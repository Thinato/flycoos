cmake_minimum_required(VERSION 3.28)
project(flycoos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 3.2 REQUIRED CONFIG)

# Dependencies are fetched as pinned tarballs. SOURCE_SUBDIR points at a directory that doesn't
# exist, so FetchContent only downloads the sources and never runs their CMake.
include(FetchContent)
FetchContent_Declare(glm
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
  URL_HASH SHA256=6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001
  SOURCE_SUBDIR _headers_only DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(fastnoiselite
  URL https://github.com/Auburn/FastNoiseLite/archive/refs/tags/v1.1.1.tar.gz
  URL_HASH SHA256=8b1d439af1ae88eaf8d3d0838540796dfc92ddd216230b2816f0c5bcbb3c9824
  SOURCE_SUBDIR _headers_only DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
  SOURCE_SUBDIR _headers_only DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glm fastnoiselite imgui)

# Dear ImGui (debug UI) with its SDL3 platform and SDL_GPU renderer backends.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# Engine core: no GPU code, so it is unit-testable.
add_library(flycoos_core STATIC src/world.cpp src/mesher.cpp src/player.cpp)
target_include_directories(flycoos_core PUBLIC src)
target_include_directories(flycoos_core SYSTEM PUBLIC ${glm_SOURCE_DIR})
target_include_directories(flycoos_core SYSTEM PRIVATE ${fastnoiselite_SOURCE_DIR}/Cpp)
target_compile_options(flycoos_core PUBLIC $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall -Wextra>)

add_executable(flycoos src/main.cpp src/renderer.cpp src/debug_ui.cpp)
target_link_libraries(flycoos PRIVATE flycoos_core imgui SDL3::SDL3)

# MSL shaders are compiled at runtime by Metal; ship them next to the binary.
add_custom_target(shaders ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:flycoos>/shaders)
add_dependencies(flycoos shaders)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE flycoos_core)
add_test(NAME core_test COMMAND core_test)
