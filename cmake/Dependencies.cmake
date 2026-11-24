include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# 1. nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

# 2. Standalone Asio
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-39-0
    GIT_SHALLOW TRUE
)

# 3. libogg
FetchContent_Declare(
    ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.6
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
    GIT_TAG v1.6.1
    GIT_SHALLOW TRUE
)

# 5. Fraunhofer FDK AAC
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(FDK_AAC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fdk-aac
    GIT_REPOSITORY https://github.com/mstorsjo/fdk-aac.git
    GIT_TAG v3.0.0
    GIT_SHALLOW TRUE
)

# 6. libmp3lame (CMake wrapper, pinned archive + SHA-256)
FetchContent_Declare(
    mp3lame
    URL https://github.com/Iunusov/libmp3lame-CMAKE/archive/bb770fb6e4b4dfc963860380a5e7765c370aaef1.tar.gz
    URL_HASH SHA256=f7302418606988264d2ae5f68aa1ffbef034d1acd73448d1b1814cd341c81ff8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 7. dr_libs (header-only dr_mp3, dr_wav, dr_flac, pinned archive + SHA-256)
FetchContent_Declare(
    dr_libs
    URL https://github.com/mackron/dr_libs/archive/e381981afa52800b9a3864df548353ff67252fbc.tar.gz
    URL_HASH SHA256=3d490df7846fac464fdc0621a2a5aeb92069616081fc0183a20c5153c0d27388
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 8. stb (stb_vorbis, pinned archive + SHA-256)
FetchContent_Declare(
    stb
    URL https://github.com/nothings/stb/archive/6b5799bae8a1fe3792691ef4b72ae26434c3e799.tar.gz
    URL_HASH SHA256=87e9ecd2d2e0cb8fa35bf5a93ea6ca3999595ea4f0a4738ed1aac0a34cbd4da9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(nlohmann_json ogg opus fdk-aac dr_libs stb)

# fdk-aac 3.x also defines the xHE-AAC encoder library, which the plugin does not use
if(TARGET fdk-xhe-aac)
    set_target_properties(fdk-xhe-aac PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

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
