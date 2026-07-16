cmake_minimum_required(VERSION 3.31)

project(SingSongBingBong VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SSBB_BUILD_STANDALONE_APP "Build the JUCE standalone desktop app." ${WIN32})

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
    )

    target_include_directories(SingSongBingBongApp
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    target_compile_definitions(SingSongBingBongApp
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
    )

    target_link_libraries(SingSongBingBongApp
        PRIVATE
            juce::juce_gui_extra
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )
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
endif()
