# Offline tests for the ENCORE. Audio Processing code. Build and run:
#   git submodule update --init --depth 1 external/JUCE
#   cmake -S tests -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(EncoreAudioProcessingTests VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The repo root is the include path, so "dsp/MasterChain.h" resolves the same
# way it does for the products that use this repo.
get_filename_component(ENCORE_AUDIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

enable_testing()

# The browser build's configuration (no JUCE: dsp/lite/JuceLite.h stands in), tested natively
foreach (suite LiteMaster Loudness Tuner)
    add_executable(${suite}Tests ${suite}Tests.cpp)
    target_include_directories(${suite}Tests PRIVATE ${ENCORE_AUDIO_ROOT})
    target_compile_definitions(${suite}Tests PRIVATE VS_NO_JUCE=1 _USE_MATH_DEFINES)
    if (MSVC)
        target_compile_options(${suite}Tests PRIVATE /O2 /fp:fast)
    endif()
    add_test(NAME ${suite}Tests COMMAND ${suite}Tests)
endforeach()

# The desktop build's configuration (JUCE's juce_dsp)
option(ENCORE_JUCE_TESTS "Build the tests that need JUCE (external/JUCE)" ON)
if (ENCORE_JUCE_TESTS)
    if (NOT EXISTS "${ENCORE_AUDIO_ROOT}/external/JUCE/CMakeLists.txt")
        message(FATAL_ERROR "external/JUCE is missing: run git submodule update --init --depth 1 external/JUCE")
    endif()
    add_subdirectory("${ENCORE_AUDIO_ROOT}/external/JUCE" "${CMAKE_BINARY_DIR}/JUCE")

    foreach (suite Pitch Verb Eq Master Strum)
        juce_add_console_app(${suite}Tests PRODUCT_NAME "${suite}Tests")
        target_sources(${suite}Tests PRIVATE ${suite}Tests.cpp)
        target_include_directories(${suite}Tests PRIVATE ${ENCORE_AUDIO_ROOT})
        target_compile_definitions(${suite}Tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(${suite}Tests PRIVATE juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
        add_test(NAME ${suite}Tests COMMAND ${suite}Tests)
    endforeach()
endif()
