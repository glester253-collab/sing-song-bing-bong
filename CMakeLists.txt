cmake_minimum_required(VERSION 3.31)

project(SingSongBingBong VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SSBB_BUILD_STANDALONE_APP "Build the JUCE standalone desktop app." ${WIN32})
option(SSBB_ENABLE_ONNX "Enable the optional ONNX Runtime text-to-vocal backend." OFF)

set(SSBB_JUCE_VERSION "8.0.14")
set(SSBB_JUCE_COMMIT "2cdfca8feb300fb424002ba2c2751569e5bacb64")
set(SSBB_JUCE_ARCHIVE_SHA256 "ceb18e4ac9ab5ea71f3f20240d5852707767a1789ab43d06656a296da9e62f3e")
set(SSBB_JUCE_LICENSE "AGPLv3 for this public repository bootstrap; a commercial JUCE 8 licence is required before closed-source distribution.")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BootstrapConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/BootstrapConfig.h
    @ONLY
)

if(SSBB_BUILD_STANDALONE_APP)
    include(FetchContent)

    FetchContent_Declare(
        juce
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/${SSBB_JUCE_VERSION}.tar.gz
        URL_HASH SHA256=${SSBB_JUCE_ARCHIVE_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(juce)

    juce_add_gui_app(SingSongBingBongApp
        PRODUCT_NAME "Sing Song Bing Bong"
    )

    target_sources(SingSongBingBongApp
        PRIVATE
            src/main.cpp
            src/MainComponent.cpp
            src/AudioEngine.cpp
            src/Transport.cpp
            src/Metronome.cpp
            src/VocalTrack.cpp
            src/TakeManager.cpp
            src/WaveformCache.cpp
            src/SessionDocument.cpp
            src/pro/Song/SongWizard.cpp
            src/pro/AI/AiVocalEngine.cpp
            src/pro/DSP/BeatEngine.cpp
            src/pro/DSP/VocalDSPChain.cpp
            src/pro/DSP/MasteringDSPChain.cpp
            src/pro/IO/MemoryRecorder.cpp
            src/pro/State/AppState.cpp
            src/pro/State/PresetManager.cpp
            src/pro/UI/TransportBar.cpp
            src/pro/UI/SongWizardPanel.cpp
            src/pro/UI/BeatMakerPanel.cpp
            src/pro/UI/VocalPanel.cpp
            src/pro/UI/MasteringPanel.cpp
    )

    target_include_directories(SingSongBingBongApp
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/AI
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/DSP
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/IO
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/Song
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/State
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/UI
    )

    target_compile_definitions(SingSongBingBongApp
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
            SSBB_ENABLE_ONNX=$<BOOL:${SSBB_ENABLE_ONNX}>
    )

    target_link_libraries(SingSongBingBongApp
        PRIVATE
            juce::juce_audio_basics
            juce::juce_audio_devices
            juce::juce_audio_utils
            juce::juce_gui_extra
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )

    if(SSBB_ENABLE_ONNX)
        find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
            HINTS ${ONNXRUNTIME_ROOT}
            PATH_SUFFIXES include include/onnxruntime/core/session)
        find_library(ONNXRUNTIME_LIBRARY onnxruntime
            HINTS ${ONNXRUNTIME_ROOT}
            PATH_SUFFIXES lib lib64)
        if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
            message(FATAL_ERROR "Set ONNXRUNTIME_ROOT to an ONNX Runtime C/C++ SDK directory.")
        endif()
        target_include_directories(SingSongBingBongApp PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(SingSongBingBongApp PRIVATE ${ONNXRUNTIME_LIBRARY})
    endif()
endif()

if(BUILD_TESTING)
    add_executable(ssbb_bootstrap_tests
        tests/bootstrap_tests.cpp
    )

    target_include_directories(ssbb_bootstrap_tests
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    add_test(NAME ssbb.bootstrap COMMAND ssbb_bootstrap_tests)

    # Transport + Metronome tests: pure C++20 — no JUCE dependency.
    # Both Transport.cpp and Metronome.cpp carry no JUCE headers.
    add_executable(ssbb_transport_tests
        tests/transport_tests.cpp
        src/Transport.cpp
        src/Metronome.cpp
    )

    target_include_directories(ssbb_transport_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    add_test(NAME ssbb.transport COMMAND ssbb_transport_tests)

    # Vocal-track subsystem tests: pure C++20 — no JUCE dependency.
    # Links the recording subsystem sources that have no JUCE includes.
    add_executable(ssbb_vocal_track_tests
        tests/vocal_track_tests.cpp
        src/VocalTrack.cpp
        src/TakeManager.cpp
        src/WaveformCache.cpp
        src/SessionDocument.cpp
    )

    target_include_directories(ssbb_vocal_track_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    add_test(NAME ssbb.vocal_track COMMAND ssbb_vocal_track_tests)

    add_executable(ssbb_pro_core_tests
        tests/pro_core_tests.cpp
        src/pro/Song/SongWizard.cpp
        src/pro/DSP/BeatEngine.cpp
    )
    target_include_directories(ssbb_pro_core_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/Song
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pro/DSP
    )
    add_test(NAME ssbb.pro_core COMMAND ssbb_pro_core_tests)
endif()
