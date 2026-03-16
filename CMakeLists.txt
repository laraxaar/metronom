cmake_minimum_required(VERSION 3.15)
project(AdvancedMetronome LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(include)

# Audio Engine Source
set(ENGINE_SOURCES
    src/AudioEngine.cpp
    src/MetronomeCore.cpp
    src/PreciseTuner.cpp
    src/SmartOnsetDetector.cpp
    src/FreePlayTracker.cpp
    src/TapDetector.cpp
    src/TempoCoach.cpp
    src/PolyrhythmEngine.cpp
    src/OfflineRenderer.cpp
    src/SettingsManager.cpp
    src/MidiSyncManager.cpp
)

# Plugins
set(PLUGIN_SOURCES
    plugins/AccuracyCalculator.cpp
    plugins/TrainingModules.cpp
)

# Main executable (Example)
add_executable(metronome_engine
    ${ENGINE_SOURCES}
    ${PLUGIN_SOURCES}
    src/MainGUI.cpp
    # main.cpp would go here
)

# Fetch ImGui
include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# Fetch GLFW
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.3.8
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Update target sources to include Main and ImGui backends
target_sources(metronome_engine PRIVATE
    src/main.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(metronome_engine PRIVATE 
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# miniaudio is header-only, so just make sure it's in include/
# For external libraries (nlohmann_json, etc.), use find_package or FetchContent

if(WIN32)
    target_link_libraries(metronome_engine PRIVATE winmm glfw OpenGL::GL)
elseif(APPLE)
    target_link_libraries(metronome_engine PRIVATE glfw "-framework CoreFoundation" "-framework AudioToolbox" "-framework OpenGL")
else()
    target_link_libraries(metronome_engine PRIVATE glfw GL pthread dl)
endif()

find_package(OpenGL REQUIRED)
target_link_libraries(metronome_engine PRIVATE OpenGL::GL)

# Compiler optimization for DSP
if(MSVC)
    target_compile_options(metronome_engine PRIVATE /O2 /Oi /Ot /fp:fast)
else()
    target_compile_options(metronome_engine PRIVATE -O3 -ffast-math -msse4.2)
endif()
