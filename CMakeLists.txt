cmake_minimum_required(VERSION 3.14)
project(p2p_tilde LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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

# ╭──────────────────────────────────────╮
# │             Dependências             │
# ╰──────────────────────────────────────╯
cpmaddpackage(
    NAME
    IXWebSocket
    GITHUB_REPOSITORY
    machinezone/IXWebSocket
    GIT_TAG
    v11.4.4
    OPTIONS
    "USE_TLS ON"
    "BUILD_SHARED_LIBS OFF"
    "USE_ZLIB ON")

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
    "NO_WEBSOCKET ON")

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

pd_add_external(p2p~ "${CMAKE_CURRENT_SOURCE_DIR}/src/p2p~.cpp")
target_include_directories(p2p_tilde PRIVATE "${opus_SOURCE_DIR}/include")
message(WARNING "${opus_SOURCE_DIR}")
target_link_libraries(
    p2p_tilde
    PRIVATE ixwebsocket
            nlohmann_json::nlohmann_json
            datachannel
            opus)
