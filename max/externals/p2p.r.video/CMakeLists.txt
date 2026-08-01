include("${MAX_SDK_BASE_DIR}/script/max-pretarget.cmake")

add_library(${PROJECT_NAME} MODULE ../../p2p/p2p_r_video.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE p2p_max_core)
target_include_directories(${PROJECT_NAME} PRIVATE ${C74_INCLUDES})
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

include("${MAX_SDK_BASE_DIR}/script/max-posttarget.cmake")
