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
    # main.cpp would go here
)

# miniaudio is header-only, so just make sure it's in include/
# For external libraries (nlohmann_json, etc.), use find_package or FetchContent

if(WIN32)
    target_link_libraries(metronome_engine PRIVATE winmm)
elseif(APPLE)
    target_link_libraries(metronome_engine PRIVATE "-framework CoreFoundation" "-framework AudioToolbox")
else()
    target_link_libraries(metronome_engine PRIVATE pthread dl)
endif()

# Compiler optimization for DSP
if(MSVC)
    target_compile_options(metronome_engine PRIVATE /O2 /Oi /Ot /fp:fast)
else()
    target_compile_options(metronome_engine PRIVATE -O3 -ffast-math -msse4.2)
endif()
