if(MAX_SDK_PATH)
    if(EXISTS "${MAX_SDK_PATH}/source/max-sdk-base/script/max-pretarget.cmake")
        set(MAX_SDK_BASE_DIR "${MAX_SDK_PATH}/source/max-sdk-base")
    elseif(EXISTS "${MAX_SDK_PATH}/script/max-pretarget.cmake")
        set(MAX_SDK_BASE_DIR "${MAX_SDK_PATH}")
    else()
        message(FATAL_ERROR "MAX_SDK_PATH is not a max-sdk or max-sdk-base checkout: ${MAX_SDK_PATH}")
    endif()
else()
    message(STATUS "MAX_SDK_PATH not set; fetching Cycling '74 max-sdk")
    cpmaddpackage(
        NAME max-sdk
        GITHUB_REPOSITORY Cycling74/max-sdk
        GIT_TAG v8.2.0
        DOWNLOAD_ONLY YES)
    set(MAX_SDK_BASE_DIR "${max-sdk_SOURCE_DIR}/source/max-sdk-base")
endif()

set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/p2p-max-package/externals")
set(MAX_SDK_INCLUDES "${MAX_SDK_BASE_DIR}/c74support/max-includes")
set(MAX_SDK_MSP_INCLUDES "${MAX_SDK_BASE_DIR}/c74support/msp-includes")
set(MAX_SDK_JIT_INCLUDES "${MAX_SDK_BASE_DIR}/c74support/jit-includes")

add_library(
    p2p_max_core STATIC
    p2p/P2PMainThreadDispatch.cpp
    p2p/P2PPeer.cpp
    p2p/P2PSession.cpp
    p2p/P2PSessionRegistry.cpp)
target_include_directories(
    p2p_max_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/p2p"
    PRIVATE "${opus_SOURCE_DIR}/include" ${MAX_SDK_INCLUDES} ${MAX_SDK_MSP_INCLUDES} ${MAX_SDK_JIT_INCLUDES})
target_link_libraries(
    p2p_max_core
    PUBLIC nlohmann_json::nlohmann_json datachannel-static opus FFmpeg::required Boost::headers
    PRIVATE spdlog)
target_compile_definitions(p2p_max_core PUBLIC P2P_JITTER_VIDEO=1)
set_target_properties(p2p_max_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(externals/p2p.config)
add_subdirectory(externals/p2p.s.audio_tilde)
add_subdirectory(externals/p2p.r.audio_tilde)
add_subdirectory(externals/p2p.r.video)

add_custom_target(
    p2p_max_package
    DEPENDS p2p.config
            p2p.s.audio_tilde
            p2p.r.audio_tilde
            p2p.r.video)
