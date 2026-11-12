# Offline tests for the ENCORE. Audio Processing code. Build and run:
#   git submodule update --init --depth 1 external/JUCE
#   cmake -S tests -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(EncoreAudioProcessingTests VERSION 2.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The repo root is the include path, so "dsp/MasterChain.h" resolves the same
# way it does for the products that use this repo.
get_filename_component(ENCORE_AUDIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

enable_testing()

# The suites that need JUCE (juce_dsp); every other tests/*Tests.cpp is built without JUCE.
set(ENCORE_JUCE_SUITES Pitch Verb Eq Master Strum)

# The browser build's configuration (no JUCE: dsp/lite/JuceLite.h stands in), tested natively.
# A new JUCE-free suite is a new tests/<Name>Tests.cpp file; nothing else to add.
file(GLOB ENCORE_SUITE_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*Tests.cpp")
set(ENCORE_LITE_SUITES "")
foreach (file ${ENCORE_SUITE_FILES})
    string(REGEX REPLACE "Tests\\.cpp$" "" suite "${file}")
    if (NOT suite IN_LIST ENCORE_JUCE_SUITES)
        list(APPEND ENCORE_LITE_SUITES ${suite})
    endif()
endforeach()

foreach (suite ${ENCORE_LITE_SUITES})
    add_executable(${suite}Tests ${suite}Tests.cpp)
    target_include_directories(${suite}Tests PRIVATE ${ENCORE_AUDIO_ROOT})
    # ENCORE_AUDIO_ROOT_DIR: where ReferenceTests finds data/reference.json
    target_compile_definitions(${suite}Tests PRIVATE VS_NO_JUCE=1 _USE_MATH_DEFINES ENCORE_AUDIO_ROOT_DIR="${ENCORE_AUDIO_ROOT}")
    if (APPLE)
        # dsp/Fft.h runs on Accelerate's vDSP on Apple systems (the JUCE suites get it through juce_dsp)
        target_link_libraries(${suite}Tests PRIVATE "-framework Accelerate")
    endif()
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

    foreach (suite ${ENCORE_JUCE_SUITES})
        juce_add_console_app(${suite}Tests PRODUCT_NAME "${suite}Tests")
        target_sources(${suite}Tests PRIVATE ${suite}Tests.cpp)
        target_include_directories(${suite}Tests PRIVATE ${ENCORE_AUDIO_ROOT})
        target_compile_definitions(${suite}Tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(${suite}Tests PRIVATE juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
        add_test(NAME ${suite}Tests COMMAND ${suite}Tests)
    endforeach()

    # The reference set (ReferenceTests.cpp) once more, built with JUCE as the desktop products build the headers
    juce_add_console_app(ReferenceJuceTests PRODUCT_NAME "ReferenceJuceTests")
    target_sources(ReferenceJuceTests PRIVATE ReferenceTests.cpp)
    target_include_directories(ReferenceJuceTests PRIVATE ${ENCORE_AUDIO_ROOT})
    target_compile_definitions(ReferenceJuceTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 ENCORE_AUDIO_ROOT_DIR="${ENCORE_AUDIO_ROOT}")
    target_link_libraries(ReferenceJuceTests PRIVATE juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
    add_test(NAME ReferenceJuceTests COMMAND ReferenceJuceTests)
endif()
