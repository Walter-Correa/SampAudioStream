include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# 1. nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# 2. Standalone Asio
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW TRUE
)

# 3. libogg
FetchContent_Declare(
    ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE
)

# 4. libopus
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)

# 5. Fraunhofer FDK AAC
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(FDK_AAC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fdk-aac
    GIT_REPOSITORY https://github.com/mstorsjo/fdk-aac.git
    GIT_TAG v2.0.3
    GIT_SHALLOW TRUE
)

# 6. libmp3lame (CMake wrapper)
FetchContent_Declare(
    mp3lame
    GIT_REPOSITORY https://github.com/Iunusov/libmp3lame-CMAKE.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

# 7. dr_libs (header-only dr_mp3, dr_wav, dr_flac)
FetchContent_Declare(
    dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

# 8. stb (stb_vorbis)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json ogg opus fdk-aac dr_libs stb)

# Fetch Asio (header-only)
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
endif()

# Fetch and build mp3lame static library cleanly without Dll subproject
FetchContent_GetProperties(mp3lame)
if(NOT mp3lame_POPULATED)
    FetchContent_Populate(mp3lame)
    # Build mp3lame directly to avoid WIN32 Dll subdirectory issues
    file(GLOB LAME_SOURCES
        "${mp3lame_SOURCE_DIR}/libmp3lame/*.c"
        "${mp3lame_SOURCE_DIR}/libmp3lame/vector/*.c"
        "${mp3lame_SOURCE_DIR}/mpglib/*.c"
    )
    add_library(mp3lame_static STATIC ${LAME_SOURCES})
    target_compile_definitions(mp3lame_static PRIVATE
        HAVE_STDINT_H
        HAVE_MPGLIB
        DECODE_ON_THE_FLY
        USE_FAST_LOG
        TAKEHIRO_IEEE754_HACK
    )
    if(MSVC)
        configure_file("${mp3lame_SOURCE_DIR}/configMS.h" "${mp3lame_BINARY_DIR}/config.h" COPYONLY)
        target_compile_definitions(mp3lame_static PRIVATE
            USE_LAYER_2
            HAVE_CONFIG_H
            _CRT_SECURE_NO_WARNINGS
            _CRT_SECURE_NO_DEPRECATED
        )
        target_include_directories(mp3lame_static PRIVATE "${mp3lame_BINARY_DIR}")
    else()
        target_compile_definitions(mp3lame_static PRIVATE
            STDC_HEADERS
            ieee754_float32_t=float
        )
    endif()
    target_include_directories(mp3lame_static
        PUBLIC "${mp3lame_SOURCE_DIR}/include"
        PRIVATE "${mp3lame_SOURCE_DIR}" "${mp3lame_SOURCE_DIR}/libmp3lame" "${mp3lame_SOURCE_DIR}/mpglib"
    )
    set_property(TARGET mp3lame_static PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
