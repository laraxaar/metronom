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
    src/UIController.cpp
    src/InputProcessor.cpp
    src/DeviceManager.cpp
    src/TunerWorker.cpp
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
    src/UIRenderer.cpp
)

include(FetchContent)
find_package(OpenGL REQUIRED)

# Fetch Glad
set(GLAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/glad" CACHE INTERNAL "")
set(GLAD_PROFILE "compatibility" CACHE STRING "" FORCE)
set(GLAD_API "gl=3.3" CACHE STRING "" FORCE)
set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)

FetchContent_Declare(
  glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG v0.1.36
)
FetchContent_MakeAvailable(glad)

# Fetch HarfBuzz (for text shaping)
FetchContent_Declare(
  harfbuzz
  GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
  GIT_TAG 8.3.0 # Use a recent stable tag
)
FetchContent_MakeAvailable(harfbuzz)

# Fetch FreeType
FetchContent_Declare(
  freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-13-2 # Use a recent stable tag
)
FetchContent_MakeAvailable(freetype)

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

# Fetch RtAudio
FetchContent_Declare(
  rtaudio
  GIT_REPOSITORY https://github.com/thestk/rtaudio.git
  GIT_TAG 6.0.1
)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtaudio)

# Update target sources
target_sources(metronome_engine PRIVATE
    src/main.cpp
    src/FontRenderer.cpp
)

# Link libraries
target_include_directories(metronome_engine PRIVATE 
    ${harfbuzz_SOURCE_DIR}/src
    ${freetype_SOURCE_DIR}/include
    ${rtaudio_SOURCE_DIR}
)

# dr_wav is header-only, already in include/
# RingBuffer, DCOffsetFilter, PitchResult are header-only, already in include/

if(WIN32)
    target_link_libraries(metronome_engine PRIVATE winmm glfw OpenGL::GL freetype harfbuzz glad rtaudio)
elseif(APPLE)
    target_link_libraries(metronome_engine PRIVATE glfw "-framework CoreFoundation" "-framework AudioToolbox" "-framework OpenGL" freetype harfbuzz rtaudio)
else()
    target_link_libraries(metronome_engine PRIVATE glfw GL pthread dl freetype harfbuzz rtaudio)
endif()

find_package(OpenGL REQUIRED)
target_link_libraries(metronome_engine PRIVATE OpenGL::GL)

# Compiler optimization for DSP
if(MSVC)
    target_compile_options(metronome_engine PRIVATE /O2 /Oi /Ot /fp:fast)
else()
    target_compile_options(metronome_engine PRIVATE -O3 -ffast-math -msse4.2)
endif()
