cmake_minimum_required(VERSION 3.20)
project(SampAudioStream VERSION 2.3.1 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Static runtime on Windows MSVC for zero external VCRUNTIME dependency
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Enforce 32-bit target (SA-MP & open.mp plugins are 32-bit only)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SampAudioStream requires a 32-bit target (use -A Win32 on Windows or -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86.cmake on Linux).")
endif()

include(cmake/Dependencies.cmake)

# SDK paths
set(OMP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/open.mp-sdk")
set(SAMP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/samp-plugin-sdk")

if(EXISTS "${OMP_SDK_DIR}/CMakeLists.txt")
    add_subdirectory("${OMP_SDK_DIR}" "${CMAKE_BINARY_DIR}/omp-sdk")
else()
    message(FATAL_ERROR "open.mp-sdk not found in ${OMP_SDK_DIR}")
endif()

set(PLUGIN_SOURCES
    src/main.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/core/thread_pool.cpp
    src/network/http_client.cpp
    src/network/http_server.cpp
    src/extractor/binary_manager.cpp
    src/extractor/browser_cookies.cpp
    src/extractor/metadata_cache.cpp
    src/extractor/resolver.cpp
    src/audio/ring_buffer.cpp
    src/audio/transcoder.cpp
    src/audio/sound_file.cpp
    src/engine/audio_engine.cpp
    src/runtime/amx_thunks.cpp
    src/runtime/samp_host.cpp
    src/runtime/omp_host.cpp
    src/pawn/amx_natives.cpp
)

if(WIN32)
    list(APPEND PLUGIN_SOURCES src/sampaudiostream.def)
endif()

add_library(sampaudiostream SHARED ${PLUGIN_SOURCES})

# Remove 'lib' prefix on Linux so output is SampAudioStream.so
set_target_properties(sampaudiostream PROPERTIES
    PREFIX ""
    OUTPUT_NAME "SampAudioStream"
)

target_include_directories(sampaudiostream BEFORE PRIVATE
    "${SAMP_SDK_DIR}"
)

target_include_directories(sampaudiostream PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${asio_SOURCE_DIR}/asio/include"
    "${dr_libs_SOURCE_DIR}"
    "${stb_SOURCE_DIR}"
)

target_compile_definitions(sampaudiostream PRIVATE
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    HAVE_STDINT_H=1
)

if(WIN32)
    target_compile_definitions(sampaudiostream PRIVATE WIN32 _WIN32 _WIN32_WINNT=0x0601 HAVE_MALLOC_H=1)
    if(MSVC)
        target_compile_options(sampaudiostream PRIVATE /W3 /utf-8 /EHsc)
    endif()
    target_link_libraries(sampaudiostream PRIVATE
        ws2_32
        winhttp
        bcrypt
        iphlpapi
    )
else()
    target_compile_definitions(sampaudiostream PRIVATE LINUX HAVE_ALLOCA_H=1)
    target_compile_options(sampaudiostream PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
    target_link_options(sampaudiostream PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,--exclude-libs,ALL
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map"
    )
    target_link_libraries(sampaudiostream PRIVATE
        pthread
        dl
    )
endif()

target_link_libraries(sampaudiostream PRIVATE
    OMP-SDK
    nlohmann_json::nlohmann_json
    ogg
    opus
    fdk-aac
    mp3lame_static
)

option(SAMPAUDIO_BUILD_TESTS "Build core unit tests" ON)
if(SAMPAUDIO_BUILD_TESTS)
    enable_testing()
    add_executable(sampaudiostream_tests
        tests/test_core.cpp
        src/core/config.cpp
        src/core/logger.cpp
        src/core/thread_pool.cpp
        src/network/http_client.cpp
        src/network/http_server.cpp
        src/extractor/binary_manager.cpp
        src/extractor/browser_cookies.cpp
        src/extractor/metadata_cache.cpp
        src/extractor/resolver.cpp
        src/audio/ring_buffer.cpp
        src/audio/transcoder.cpp
        src/audio/sound_file.cpp
        src/engine/audio_engine.cpp
    )
    target_include_directories(sampaudiostream_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${asio_SOURCE_DIR}/asio/include"
        "${dr_libs_SOURCE_DIR}"
        "${stb_SOURCE_DIR}"
    )
    target_compile_definitions(sampaudiostream_tests PRIVATE
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        HAVE_STDINT_H=1
    )
    target_link_libraries(sampaudiostream_tests PRIVATE
        nlohmann_json::nlohmann_json
        ogg
        opus
        fdk-aac
        mp3lame_static
    )
    if(WIN32)
        target_compile_definitions(sampaudiostream_tests PRIVATE WIN32 _WIN32 _WIN32_WINNT=0x0601)
        target_link_libraries(sampaudiostream_tests PRIVATE ws2_32 winhttp bcrypt iphlpapi dbghelp)
        if(MSVC)
            target_compile_options(sampaudiostream_tests PRIVATE $<$<CONFIG:Release>:/Zi>)
            target_link_options(sampaudiostream_tests PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
        endif()
    else()
        target_link_libraries(sampaudiostream_tests PRIVATE pthread dl)
    endif()
    add_test(NAME core_tests COMMAND sampaudiostream_tests)
    set_tests_properties(core_tests PROPERTIES TIMEOUT 300)
endif()
