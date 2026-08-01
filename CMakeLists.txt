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

option(P2P_GEM_VIDEO "Build the GEM/WebRTC video receiver" ON)
option(P2P_BUILD_MAX "Build the Max/MSP + Jitter object family" ON)
set(MAX_SDK_PATH
    ""
    CACHE PATH "Path to the Cycling '74 max-sdk checkout")

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
    v0.24.3
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

if(P2P_GEM_VIDEO OR P2P_BUILD_MAX)
    # FFmpeg does not provide a CMake build. Let CPM own source acquisition and use its supported configure/Make build
    # to produce only the static, PIC libraries needed for H.264 decoding and RGBA conversion.
    cpmaddpackage(
        NAME
        ffmpeg
        GITHUB_REPOSITORY
        FFmpeg/FFmpeg
        GIT_TAG
        n7.1.5
        DOWNLOAD_ONLY
        YES)

    include(ExternalProject)
    include(ProcessorCount)
    find_package(Threads REQUIRED)
    find_program(FFMPEG_SHELL NAMES sh bash REQUIRED)
    find_program(
        FFMPEG_MAKE
        NAMES gmake
              make
              mingw32-make
              REQUIRED)
    ProcessorCount(FFMPEG_BUILD_JOBS)
    if(NOT FFMPEG_BUILD_JOBS)
        set(FFMPEG_BUILD_JOBS 2)
    endif()

    set(FFMPEG_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-install")
    set(FFMPEG_LIBRARY_DIR "${FFMPEG_INSTALL_DIR}/lib")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_INSTALL_DIR}/include")
    file(MAKE_DIRECTORY "${FFMPEG_INCLUDE_DIR}")

    set(FFMPEG_CONFIGURE_OPTIONS
        --prefix=${FFMPEG_INSTALL_DIR}
        --disable-shared
        --enable-static
        --enable-pic
        --disable-autodetect
        --disable-programs
        --disable-doc
        --disable-debug
        --disable-network
        --disable-iconv
        --disable-avdevice
        --disable-avfilter
        --disable-avformat
        --disable-swresample
        --disable-postproc
        --disable-everything
        --enable-avcodec
        --enable-avutil
        --enable-swscale
        --enable-decoder=h264
        --enable-parser=h264)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
        # Some of FFmpeg's x86 inline assembly still emits non-PIC relocations even with --enable-pic, which cannot be
        # linked into Pd/Max modules.
        list(
            APPEND
            FFMPEG_CONFIGURE_OPTIONS
            --disable-x86asm
            --disable-inline-asm)
    endif()
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES FFMPEG_OSX_ARCH_COUNT)
        if(NOT
           FFMPEG_OSX_ARCH_COUNT
           EQUAL
           1)
            message(FATAL_ERROR "The static FFmpeg build requires one CMAKE_OSX_ARCHITECTURES value")
        endif()
        list(
            GET
            CMAKE_OSX_ARCHITECTURES
            0
            FFMPEG_OSX_ARCH)
        list(
            APPEND
            FFMPEG_CONFIGURE_OPTIONS
            --arch=${FFMPEG_OSX_ARCH}
            "--extra-cflags=-arch ${FFMPEG_OSX_ARCH}"
            "--extra-ldflags=-arch ${FFMPEG_OSX_ARCH}")
    endif()

    ExternalProject_Add(
        ffmpeg_static
        SOURCE_DIR "${ffmpeg_SOURCE_DIR}"
        BINARY_DIR "${ffmpeg_BINARY_DIR}/static-build"
        CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}"
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}" "${FFMPEG_SHELL}" "${ffmpeg_SOURCE_DIR}/configure"
            ${FFMPEG_CONFIGURE_OPTIONS}
        BUILD_COMMAND "${FFMPEG_MAKE}" clean
        COMMAND "${FFMPEG_MAKE}" -j${FFMPEG_BUILD_JOBS}
        INSTALL_COMMAND "${FFMPEG_MAKE}" install
        BUILD_BYPRODUCTS
            "${FFMPEG_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}avcodec${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "${FFMPEG_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}avutil${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "${FFMPEG_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}swscale${CMAKE_STATIC_LIBRARY_SUFFIX}"
        USES_TERMINAL_BUILD YES
        USES_TERMINAL_INSTALL YES)

    foreach(FFMPEG_COMPONENT avutil avcodec swscale)
        add_library(
            ffmpeg_${FFMPEG_COMPONENT}
            STATIC
            IMPORTED
            GLOBAL)
        add_library(FFmpeg::${FFMPEG_COMPONENT} ALIAS ffmpeg_${FFMPEG_COMPONENT})
        set_target_properties(
            ffmpeg_${FFMPEG_COMPONENT}
            PROPERTIES
                IMPORTED_LOCATION
                "${FFMPEG_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}${FFMPEG_COMPONENT}${CMAKE_STATIC_LIBRARY_SUFFIX}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
        add_dependencies(ffmpeg_${FFMPEG_COMPONENT} ffmpeg_static)
    endforeach()
    target_link_libraries(ffmpeg_avutil INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
    target_link_libraries(ffmpeg_avcodec INTERFACE FFmpeg::avutil)
    target_link_libraries(ffmpeg_swscale INTERFACE FFmpeg::avutil)
    if(UNIX)
        target_link_libraries(ffmpeg_avutil INTERFACE m)
    endif()
    include(CheckLibraryExists)
    check_library_exists(
        atomic
        __atomic_load_8
        ""
        FFMPEG_HAVE_LIBATOMIC)
    if(FFMPEG_HAVE_LIBATOMIC)
        target_link_libraries(ffmpeg_avutil INTERFACE atomic)
    endif()
    if(WIN32)
        target_link_libraries(ffmpeg_avutil INTERFACE bcrypt)
    endif()

    add_library(ffmpeg_required INTERFACE)
    add_library(FFmpeg::required ALIAS ffmpeg_required)
    target_link_libraries(ffmpeg_required INTERFACE FFmpeg::avcodec FFmpeg::swscale FFmpeg::avutil)
endif()

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

    # Gem's autotools build generates this header. We only consume its public ABI here, so generate the default
    # configuration for external clients.
    set(GEM_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gem-generated")
    file(MAKE_DIRECTORY "${GEM_GENERATED_INCLUDE_DIR}/Gem")
    file(WRITE "${GEM_GENERATED_INCLUDE_DIR}/Gem/GemGLconfig.h"
         "/* Generated for p2p~ consumers of Gem's public headers. */\n")
endif()

cpmaddpackage("gh:gabime/spdlog@1.17.0")
set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(P2P_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/p2p_setup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/P2PMainThreadDispatch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/P2PPeer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/P2PSession.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/P2PSessionRegistry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/p2p_config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/p2p_s_audio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/p2p_r_audio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/p2p_r_video.cpp")

pd_add_external(p2p "${P2P_SOURCES}")
pd_add_datafile(p2p "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p-test.pd")
pd_add_datafile(p2p "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p-smoke.pd")
pd_add_datafile(p2p "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p-tls-smoke.pd")
pd_add_datafile(p2p "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p/TESTING.md")

target_include_directories(p2p PRIVATE "${opus_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/puredata/p2p")
target_link_libraries(
    p2p
    PRIVATE nlohmann_json::nlohmann_json
            datachannel-static
            spdlog
            opus)

if(P2P_GEM_VIDEO AND Gem_ADDED)
    target_compile_definitions(p2p PRIVATE P2P_GEM_VIDEO=1)
    target_include_directories(p2p PRIVATE "${GEM_GENERATED_INCLUDE_DIR}" "${Gem_SOURCE_DIR}/src")
    # Pd does not guarantee that symbols from the separately loaded Gem external are globally visible. Compile the image
    # ABI used by pixBlock into p2p~ so loading does not depend on Gem's dlopen visibility or load order.
    target_sources(
        p2p
        PRIVATE "${Gem_SOURCE_DIR}/src/Gem/Image.cpp"
                "${Gem_SOURCE_DIR}/src/Gem/PixConvert.cpp"
                "${Gem_SOURCE_DIR}/src/Gem/PixConvertAltivec.cpp"
                "${Gem_SOURCE_DIR}/src/Gem/PixConvertSSE2.cpp"
                "${Gem_SOURCE_DIR}/src/Utils/SIMD.cpp"
                "${Gem_SOURCE_DIR}/src/Utils/Thread.cpp")
    if(UNIX)
        target_compile_definitions(p2p PRIVATE HAVE_UNISTD_H=1 HAVE_SYS_TIME_H=1 HAVE_SYS_TYPES_H=1)
    endif()
    target_link_libraries(p2p PRIVATE FFmpeg::required)
    message(STATUS "p2p~ GEM video support enabled (Gem source: ${Gem_SOURCE_DIR})")
else()
    message(STATUS "p2p~ GEM video support disabled; set P2P_GEM_VIDEO=ON to enable it")
endif()

target_compile_options(
    p2p
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
    target_compile_options(p2p PRIVATE -g)
endif()

# ╭──────────────────────────────────────╮
# │              Max/Jitter              │
# ╰──────────────────────────────────────╯
if(P2P_BUILD_MAX AND NOT LINUX)
    if(MAX_SDK_PATH)
        if(EXISTS "${MAX_SDK_PATH}/c74support/max-includes/ext.h")
            set(MAX_SDK_BASE_PATH "${MAX_SDK_PATH}")
        elseif(EXISTS "${MAX_SDK_PATH}/source/c74support/max-includes/ext.h")
            set(MAX_SDK_BASE_PATH "${MAX_SDK_PATH}/source")
        else()
            message(FATAL_ERROR "MAX_SDK_PATH does not contain c74support/max-includes/ext.h: " "${MAX_SDK_PATH}")
        endif()
    else()
        message(STATUS "MAX_SDK_PATH not set; fetching Cycling '74 max-sdk-base")
        cpmaddpackage(
            NAME
            max_sdk_base
            GITHUB_REPOSITORY
            Cycling74/max-sdk-base
            GIT_TAG
            main
            DOWNLOAD_ONLY
            YES)
        set(MAX_SDK_BASE_PATH "${max_sdk_base_SOURCE_DIR}")
    endif()

    set(MAX_API_INCLUDES "${MAX_SDK_BASE_PATH}/c74support/max-includes" "${MAX_SDK_BASE_PATH}/c74support/msp-includes"
                         "${MAX_SDK_BASE_PATH}/c74support/jit-includes")
    if(APPLE)
        find_library(
            MAX_AUDIO_API_FRAMEWORK MaxAudioAPI
            PATHS "${MAX_SDK_BASE_PATH}/c74support/msp-includes"
            NO_DEFAULT_PATH REQUIRED)
        find_library(
            JITTER_API_FRAMEWORK JitterAPI
            PATHS "${MAX_SDK_BASE_PATH}/c74support/jit-includes"
            NO_DEFAULT_PATH REQUIRED)
    endif()
    message(STATUS "Building Max externals with SDK base: ${MAX_SDK_BASE_PATH}")
    set(MAX_PACKAGE_DIR "${CMAKE_BINARY_DIR}/p2p-max-package/externals")
    file(MAKE_DIRECTORY "${MAX_PACKAGE_DIR}")
    # Remove artifacts produced by the older shared-core layout when an existing build directory is reconfigured.
    file(
        REMOVE
        "${MAX_PACKAGE_DIR}/libp2p_max_core.dylib"
        "${MAX_PACKAGE_DIR}/libp2p_max_core.so"
        "${MAX_PACKAGE_DIR}/p2p_max_core.dll")

    add_library(
        p2p_max_core STATIC
        max/p2p/P2PMainThreadDispatch.cpp
        max/p2p/P2PPeer.cpp
        max/p2p/P2PSession.cpp
        max/p2p/P2PSessionRegistry.cpp)
    target_include_directories(
        p2p_max_core
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/max/p2p"
        PRIVATE "${opus_SOURCE_DIR}/include" ${MAX_API_INCLUDES})
    target_link_libraries(
        p2p_max_core
        PUBLIC nlohmann_json::nlohmann_json datachannel-static opus
        PRIVATE spdlog)
    set_target_properties(p2p_max_core PROPERTIES POSITION_INDEPENDENT_CODE ON ARCHIVE_OUTPUT_DIRECTORY
                                                                               "${CMAKE_BINARY_DIR}/max-static")

    target_compile_definitions(p2p_max_core PUBLIC P2P_JITTER_VIDEO=1)
    # P2PPeer.hpp exposes FFmpeg frame types when video is enabled, so the four frontend targets also require FFmpeg's
    # include directories.
    target_link_libraries(p2p_max_core PUBLIC FFmpeg::required)
    message(STATUS "Max/Jitter H264 video support enabled")

    function(
        p2p_add_max_external
        target
        output_name
        source)
        string(
            REPLACE "_"
                    "-"
                    bundle_identifier_suffix
                    "${target}")
        add_library(${target} MODULE "${source}")
        target_include_directories(${target} PRIVATE ${MAX_API_INCLUDES})
        target_link_libraries(${target} PRIVATE p2p_max_core)
        if(APPLE)
            # Header-only compilation is not sufficient for Jitter. Linking these SDK frameworks makes the jit_matrix
            # class available when Max loads the bundle.
            target_link_libraries(${target} PRIVATE "${MAX_AUDIO_API_FRAMEWORK}" "${JITTER_API_FRAMEWORK}")
        endif()
        set_target_properties(
            ${target}
            PROPERTIES PREFIX ""
                       OUTPUT_NAME "${output_name}"
                       LIBRARY_OUTPUT_DIRECTORY "${MAX_PACKAGE_DIR}")
        if(APPLE)
            # A Max external is a CFBundle-shaped directory whose executable has no suffix. CMake's MODULE output is the
            # executable inside it.
            set_target_properties(${target} PROPERTIES SUFFIX "" LIBRARY_OUTPUT_DIRECTORY
                                                                 "${MAX_PACKAGE_DIR}/${output_name}.mxo/Contents/MacOS")
            file(MAKE_DIRECTORY "${MAX_PACKAGE_DIR}/${output_name}.mxo/Contents")
            file(
                WRITE "${MAX_PACKAGE_DIR}/${output_name}.mxo/Contents/Info.plist"
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleExecutable</key><string>${output_name}</string>
<key>CFBundleIdentifier</key><string>com.charlesneimog.${bundle_identifier_suffix}</string>
<key>CFBundlePackageType</key><string>iLaX</string>
</dict></plist>
")
            target_link_options(${target} PRIVATE "-Wl,-undefined,dynamic_lookup")
            add_custom_command(
                TARGET ${target}
                POST_BUILD
                COMMAND /usr/bin/codesign --force --deep --sign - "${MAX_PACKAGE_DIR}/${output_name}.mxo"
                COMMENT "Ad-hoc signing ${output_name}.mxo for Max")
        elseif(WIN32)
            set_target_properties(${target} PROPERTIES SUFFIX ".mxe64")
        else()
            set_target_properties(${target} PROPERTIES SUFFIX ".mxo")
        endif()
    endfunction()

    p2p_add_max_external(p2p_config_max "p2p.config" max/p2p/p2p_config.cpp)
    p2p_add_max_external(p2p_s_audio_max "p2p.s.audio~" max/p2p/p2p_s_audio.cpp)
    p2p_add_max_external(p2p_r_audio_max "p2p.r.audio~" max/p2p/p2p_r_audio.cpp)
    p2p_add_max_external(p2p_r_video_max "p2p.r.video" max/p2p/p2p_r_video.cpp)
    add_custom_target(
        p2p_max_package
        DEPENDS p2p_config_max
                p2p_s_audio_max
                p2p_r_audio_max
                p2p_r_video_max)
endif()

execute_process(
    COMMAND brew --prefix boost
    OUTPUT_VARIABLE BOOST_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE)

include_directories(${BOOST_PREFIX}/include)
