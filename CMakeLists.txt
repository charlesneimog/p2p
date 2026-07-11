cmake_minimum_required(VERSION 3.30)
project(p2p~ LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
set(NO_WERROR ON)
set(NO_TESTS ON)
add_compile_options(-Wno-error=format)

# ╭──────────────────────────────────────╮
# │               pd.cmake               │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
if(NOT EXISTS ${PDCMAKE_FILE})
    file(DOWNLOAD https://raw.githubusercontent.com/pure-data/pd.cmake/refs/heads/main/pd.cmake ${PDCMAKE_FILE})
endif()
include(${PDCMAKE_FILE})
include_directories("${PD_SOURCES_PATH}")

# ╭──────────────────────────────────────╮
# │                 CPM                  │
# ╰──────────────────────────────────────╯
set(CPM_FILE ${CMAKE_BINARY_DIR}/CPM.cmake)
if(NOT EXISTS "${CPM_FILE}")
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.42.1/CPM.cmake" ${CPM_FILE})
endif()
include(${CPM_FILE})

find_package(Boost REQUIRED)
find_package(PkgConfig QUIET)

option(P2P_GEM_VIDEO "Build the GEM/WebRTC video receiver" ON)
if(P2P_GEM_VIDEO AND PkgConfig_FOUND)
    pkg_check_modules(
        FFMPEG
        QUIET
        IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale)
endif()

# ╭──────────────────────────────────────╮
# │             Dependências             │
# ╰──────────────────────────────────────╯
if(MINGW)
    add_compile_options(-Wno-error=format -Wno-error=format-extra-args)
endif()

cpmaddpackage(
    NAME
    libdatachannel
    GITHUB_REPOSITORY
    paullouisageneau/libdatachannel
    GIT_TAG
    v0.24.3
    OPTIONS
    "USE_NICE OFF"
    "USE_SYSTEM_SRTP OFF"
    "NO_MEDIA OFF")

cpmaddpackage(
    NAME
    opus
    GITHUB_REPOSITORY
    xiph/opus
    GIT_TAG
    v1.4
    OPTIONS
    "OPUS_BUILD_SHARED_LIBS OFF"
    "OPUS_BUILD_TESTING OFF"
    "OPUS_BUILD_PROGRAMS OFF")

if(P2P_GEM_VIDEO)
    cpmaddpackage(
        NAME
        Gem
        GITHUB_REPOSITORY
        umlaeute/Gem
        GIT_TAG
        v0.95
        DOWNLOAD_ONLY
        YES)

    # Gem's autotools build generates this header. We only consume its public
    # ABI here, so generate the default configuration for external clients.
    set(GEM_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gem-generated")
    file(MAKE_DIRECTORY "${GEM_GENERATED_INCLUDE_DIR}/Gem")
    file(WRITE "${GEM_GENERATED_INCLUDE_DIR}/Gem/GemGLconfig.h"
         "/* Generated for p2p~ consumers of Gem's public headers. */\n")
endif()

cpmaddpackage("gh:gabime/spdlog@1.17.0")
set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)

pd_add_external(p2p~ "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p~.cpp")
pd_add_datafile(p2p~ "${CMAKE_CURRENT_SOURCE_DIR}/p2p~-help.pd")
pd_add_datafile(p2p~ "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p~.cpp")

target_include_directories(p2p_tilde PRIVATE "${opus_SOURCE_DIR}/include")
target_link_libraries(
    p2p_tilde
    PRIVATE nlohmann_json::nlohmann_json
            datachannel-static
            spdlog
            opus)

if(P2P_GEM_VIDEO
   AND Gem_ADDED
   AND FFMPEG_FOUND)
    target_compile_definitions(p2p_tilde PRIVATE P2P_GEM_VIDEO=1)
    target_include_directories(
        p2p_tilde PRIVATE "${GEM_GENERATED_INCLUDE_DIR}" "${Gem_SOURCE_DIR}/src")
    # Pd does not guarantee that symbols from the separately loaded Gem external
    # are globally visible. Compile the image ABI used by pixBlock into p2p~ so
    # loading does not depend on Gem's dlopen visibility or load order.
    target_sources(
        p2p_tilde
        PRIVATE "${Gem_SOURCE_DIR}/src/Gem/Image.cpp"
                "${Gem_SOURCE_DIR}/src/Gem/PixConvert.cpp"
                "${Gem_SOURCE_DIR}/src/Gem/PixConvertAltivec.cpp"
                "${Gem_SOURCE_DIR}/src/Gem/PixConvertSSE2.cpp"
                "${Gem_SOURCE_DIR}/src/Utils/SIMD.cpp"
                "${Gem_SOURCE_DIR}/src/Utils/Thread.cpp")
    if(UNIX)
        target_compile_definitions(
            p2p_tilde PRIVATE HAVE_UNISTD_H=1 HAVE_SYS_TIME_H=1 HAVE_SYS_TYPES_H=1)
    endif()
    target_link_libraries(p2p_tilde PRIVATE PkgConfig::FFMPEG)
    message(STATUS "p2p~ GEM video support enabled (Gem source: ${Gem_SOURCE_DIR})")
else()
    message(STATUS "p2p~ GEM video support disabled; install FFmpeg development files or set P2P_GEM_VIDEO=OFF")
endif()

target_compile_options(
    p2p_tilde
    PRIVATE $<$<CONFIG:Release>:
            $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-O3>
            $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffunction-sections>
            $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fdata-sections>
            $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fno-math-errno>
            $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fno-trapping-math>
            $<$<CXX_COMPILER_ID:MSVC>:/O2>
            $<$<CXX_COMPILER_ID:MSVC>:/Ob3>
            $<$<CXX_COMPILER_ID:MSVC>:/Oi>
            $<$<CXX_COMPILER_ID:MSVC>:/Ot>
            $<$<CXX_COMPILER_ID:MSVC>:/fp:fast>
            $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:AppleClang,Clang,GNU>>:-msse4.2>
            $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>
            >)

set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(p2p_tilde PRIVATE -g)
endif()

execute_process(
    COMMAND brew --prefix boost
    OUTPUT_VARIABLE BOOST_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE)

include_directories(${BOOST_PREFIX}/include)
