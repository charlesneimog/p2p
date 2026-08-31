cmake_minimum_required(VERSION 3.30)
project(p2p LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
set(NO_WERROR ON)

# These names are libdatachannel's public cache options. Force them before CPM adds the dependency so an older build
# directory cannot retain its defaults and unexpectedly build example executables.
set(NO_TESTS
    ON
    CACHE BOOL "Disable libdatachannel tests" FORCE)
set(NO_EXAMPLES
    ON
    CACHE BOOL "Disable libdatachannel examples" FORCE)
add_compile_options(-Wno-error=format)

set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OPENSSL_MSVC_STATIC_RT TRUE)
set(BUILD_SHARED_LIBS OFF)

# FindOpenSSL caches absolute library paths. A cache created for native Apple Silicon can otherwise keep /opt/homebrew's
# arm64 dylibs when the same build tree is reconfigured for an Intel slice.
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "(^|;)x86_64(;|$)")
    foreach(OPENSSL_CACHE_ENTRY OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)
        if(DEFINED ${OPENSSL_CACHE_ENTRY} AND "${${OPENSSL_CACHE_ENTRY}}" MATCHES "^/opt/homebrew/")
            unset(${OPENSSL_CACHE_ENTRY} CACHE)
        endif()
    endforeach()
endif()

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
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.43.1/CPM.cmake" ${CPM_FILE})
endif()
include(${CPM_FILE})

find_package(Boost REQUIRED)

option(P2P_BUILD_MAX "Build the Max/MSP + Jitter object family" ON)
set(MAX_SDK_PATH
    ""
    CACHE PATH "Path to the Cycling '74 max-sdk checkout")

set(P2P_MAX_ENABLED OFF)
if(P2P_BUILD_MAX AND NOT LINUX)
    set(P2P_MAX_ENABLED ON)
endif()

# ╭──────────────────────────────────────╮
# │               OpenSSL                │
# ╰──────────────────────────────────────╯
cpmaddpackage(
    URI
    "gh:jimmy-park/openssl-cmake#3.6.0"
    OPTIONS
    "OPENSSL_USE_STATIC_LIBS ON"
    "BUILD_SHARED_LIBS OFF"
    "BUILD_EXAMPLES OFF"
    "BUILD_TESTING OFF"
    "CMAKE_OSX_DEPLOYMENT_TARGET=10.15")

# ╭──────────────────────────────────────╮
# │             Dependências             │
# ╰──────────────────────────────────────╯
if(MINGW)
    add_compile_options(-Wno-error=format -Wno-error=format-extra-args)
endif()

# P2PSession uses nlohmann/json directly. Do not rely on libdatachannel's optional examples to create this target as a
# side effect.
cpmaddpackage(
    NAME
    nlohmann_json
    GITHUB_REPOSITORY
    nlohmann/json
    GIT_TAG
    v3.11.3
    OPTIONS
    "JSON_BuildTests OFF"
    "JSON_Install OFF")

cpmaddpackage(
    NAME
    libdatachannel
    GITHUB_REPOSITORY
    paullouisageneau/libdatachannel
    GIT_TAG
    v0.24.5
    OPTIONS
    "USE_NICE OFF"
    "USE_SYSTEM_SRTP OFF"
    "NO_MEDIA OFF"
    "NO_EXAMPLES ON"
    "NO_TESTS ON")

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

include(cmake/FFmpeg.cmake)

cpmaddpackage(
    NAME
    Gem
    GITHUB_REPOSITORY
    umlaeute/Gem
    GIT_TAG
    v0.95
    DOWNLOAD_ONLY
    YES)

# Gem's autotools build generates this header. We only consume its public ABI here, so generate the default
# configuration for external clients.
set(GEM_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gem-generated")
file(MAKE_DIRECTORY "${GEM_GENERATED_INCLUDE_DIR}/Gem")
file(WRITE "${GEM_GENERATED_INCLUDE_DIR}/Gem/GemGLconfig.h"
     "/* Generated for p2p~ consumers of Gem's public headers. */\n")

cpmaddpackage("gh:gabime/spdlog@1.17.0")
set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(P2P_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/P2PPeer.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/P2PSession.cpp")

add_library(p2p_core STATIC ${P2P_CORE_SOURCES})
target_include_directories(
    p2p_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p"
    PRIVATE "${opus_SOURCE_DIR}/include")
target_link_libraries(
    p2p_core
    PUBLIC nlohmann_json::nlohmann_json
           datachannel-static
           opus
           FFmpeg::required
           Boost::headers
    PRIVATE spdlog)
target_compile_definitions(p2p_core PUBLIC P2P_VIDEO=1)
set_target_properties(p2p_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    find_library(P2P_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(P2P_CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(
        p2p_core
        PUBLIC ${P2P_SECURITY_FRAMEWORK} ${P2P_CORE_FOUNDATION_FRAMEWORK})
endif()

set(P2P_PD_RUNTIME_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pd/P2PMainThreadDispatch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pd/P2PSessionRegistry.cpp"
    # "${CMAKE_CURRENT_SOURCE_DIR}/src/pd/p2p_r_video.cpp"
)

# A flat Mach-O namespace makes every global symbol in separately loaded Pd externals interpose. That is especially
# unsafe here because every external would otherwise embed its own copy of the session registry, libdatachannel, and
# OpenSSL. Keep those objects in one runtime dylib on macOS and let the thin Pd externals share it through normal
# two-level namespace bindings.
if(APPLE)
    add_library(p2p_pd_runtime SHARED ${P2P_PD_RUNTIME_SOURCES})
    target_include_directories(p2p_pd_runtime PRIVATE "${PD_SOURCES_PATH}")
    target_compile_definitions(p2p_pd_runtime PRIVATE UNIX MACOSX)
    if(NOT PD_FLOATSIZE EQUAL 32)
        target_compile_definitions(p2p_pd_runtime PRIVATE PD_FLOATSIZE=${PD_FLOATSIZE})
    endif()
    target_link_libraries(p2p_pd_runtime PRIVATE p2p_core)
    # Pd API symbols are supplied by the host process when the runtime is loaded as a dependency of an external.
    target_link_options(p2p_pd_runtime PRIVATE -undefined dynamic_lookup)
    set_target_properties(
        p2p_pd_runtime
        PROPERTIES PREFIX ""
                   OUTPUT_NAME "p2p.runtime"
                   SUFFIX ".dylib"
                   POSITION_INDEPENDENT_CODE ON
                   MACOSX_RPATH ON
                   BUILD_WITH_INSTALL_NAME_DIR ON
                   INSTALL_NAME_DIR "@rpath"
                   BUILD_RPATH "@loader_path"
                   INSTALL_RPATH "@loader_path")
    set(P2P_PD_EXTERNAL_COMMON_SOURCES "")
else()
    set(P2P_PD_EXTERNAL_COMMON_SOURCES ${P2P_PD_RUNTIME_SOURCES})
endif()

# Pd and Max expose the same object names, but CMake target names are global.
# Keep distinct internal names while pd.cmake preserves the public filenames.
pd_add_external(
    p2p.config
    "${P2P_PD_EXTERNAL_COMMON_SOURCES};${CMAKE_CURRENT_SOURCE_DIR}/src/pd/p2p_config.cpp"
    TARGET p2p_pd_config)
pd_add_external(
    p2p.s.audio~
    "${P2P_PD_EXTERNAL_COMMON_SOURCES};${CMAKE_CURRENT_SOURCE_DIR}/src/pd/p2p_s_audio.cpp"
    TARGET p2p_pd_s_audio)
pd_add_external(
    p2p.r.audio~
    "${P2P_PD_EXTERNAL_COMMON_SOURCES};${CMAKE_CURRENT_SOURCE_DIR}/src/pd/p2p_r_audio.cpp"
    TARGET p2p_pd_r_audio)
pd_add_external(
    p2p.r.video
    "${P2P_PD_EXTERNAL_COMMON_SOURCES};${CMAKE_CURRENT_SOURCE_DIR}/src/pd/p2p_r_video.cpp"
    TARGET p2p_pd_r_video)

pd_add_datafile(p2p_pd_config "${CMAKE_CURRENT_SOURCE_DIR}/p2p-help.pd")
pd_add_datafile(p2p_pd_config "${CMAKE_CURRENT_SOURCE_DIR}/puredata")
pd_add_datafile(p2p_pd_config "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
pd_add_datafile(p2p_pd_config "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

file(
    GLOB
    P2P_PD_DATAFILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pd/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pd/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/*.hpp")

pd_add_datafile(p2p_pd_config "${P2P_PD_DATAFILES}")

target_include_directories(p2p_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/pd")

if(APPLE)
    foreach(P2P_PD_TARGET p2p_pd_config p2p_pd_r_audio p2p_pd_s_audio p2p_pd_r_video)
        target_include_directories(
            ${P2P_PD_TARGET}
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p"
                    "${opus_SOURCE_DIR}/include"
                    "${libdatachannel_SOURCE_DIR}/include")
        # These are header-only usage requirements exposed by P2PSession.hpp/P2PPeer.hpp. Do not link any of the
        # corresponding static implementation libraries into the thin externals.
        target_link_libraries(
            ${P2P_PD_TARGET}
            PRIVATE p2p_pd_runtime
                    nlohmann_json::nlohmann_json
                    Boost::headers
                    ffmpeg.avcodec.headers
                    ffmpeg.swscale.headers
                    ffmpeg.avutil.headers)
        target_compile_definitions(${P2P_PD_TARGET} PRIVATE P2P_VIDEO=1)
        set_target_properties(
            ${P2P_PD_TARGET}
            PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")

        # pd.cmake currently adds -flat_namespace. Retain dynamic lookup only for Pd's host API; normal two-level
        # binding ensures the external's other symbols resolve to p2p.runtime instead of another loaded external.
        get_target_property(P2P_PD_LINK_OPTIONS ${P2P_PD_TARGET} LINK_OPTIONS)
        if(P2P_PD_LINK_OPTIONS)
            list(REMOVE_ITEM P2P_PD_LINK_OPTIONS -flat_namespace)
            set_property(TARGET ${P2P_PD_TARGET} PROPERTY LINK_OPTIONS "${P2P_PD_LINK_OPTIONS}")
        endif()
    endforeach()
    add_dependencies(p2p_pd_config p2p_pd_runtime)
    pd_add_datafile(p2p_pd_config "$<TARGET_FILE:p2p_pd_runtime>")
else()
    target_link_libraries(p2p_pd_config PRIVATE p2p_core)
    target_link_libraries(p2p_pd_r_audio PRIVATE p2p_core)
    target_link_libraries(p2p_pd_s_audio PRIVATE p2p_core)
    target_link_libraries(p2p_pd_r_video PRIVATE p2p_core)
endif()
target_include_directories(p2p_pd_r_video PRIVATE "${GEM_GENERATED_INCLUDE_DIR}" "${Gem_SOURCE_DIR}/src")
# Pd does not guarantee that symbols from the separately loaded Gem external are globally visible. Compile the image ABI
# used by pixBlock into p2p~ so loading does not depend on Gem's dlopen visibility or load order.
target_sources(
    p2p_pd_r_video
    PRIVATE "${Gem_SOURCE_DIR}/src/Gem/Image.cpp"
            "${Gem_SOURCE_DIR}/src/Gem/PixConvert.cpp"
            "${Gem_SOURCE_DIR}/src/Gem/PixConvertAltivec.cpp"
            "${Gem_SOURCE_DIR}/src/Gem/PixConvertSSE2.cpp"
            "${Gem_SOURCE_DIR}/src/Utils/SIMD.cpp"
            "${Gem_SOURCE_DIR}/src/Utils/Thread.cpp")
if(UNIX)
    target_compile_definitions(p2p_core PUBLIC HAVE_UNISTD_H=1 HAVE_SYS_TIME_H=1 HAVE_SYS_TYPES_H=1)
endif()
set(P2P_OPTIMIZED_TARGETS p2p_core p2p_pd_r_video p2p_pd_r_audio p2p_pd_s_audio)
if(APPLE)
    list(APPEND P2P_OPTIMIZED_TARGETS p2p_pd_runtime)
endif()
foreach(P2P_TARGET IN LISTS P2P_OPTIMIZED_TARGETS)
    target_compile_options(
        ${P2P_TARGET}
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
endforeach()

set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ╭──────────────────────────────────────╮
# │              Max/Jitter              │
# ╰──────────────────────────────────────╯
if(P2P_MAX_ENABLED)
    add_subdirectory(max)
endif()
