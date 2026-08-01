include_guard(GLOBAL)

# This fork adds native CMake targets to FFmpeg 7.0. Pin the exact revision:
# the repository's CMake branch has no release tags.
cpmaddpackage(
    NAME ffmpeg_cmake
    GITHUB_REPOSITORY Pawday/ffmpeg-cmake
    GIT_TAG dea60f9dbe2f0052f4f9a6685016da9bc748e85a
    EXCLUDE_FROM_ALL YES
    OPTIONS
        "FFMPEG_DISABLE_CMAKE_WARNINGS ON"
        "FFMPEG_LICENSE LGPLV2_1")

# The fork exposes separate PIC archives and header targets, but does not
# provide a combined usage target for embedders.
add_library(ffmpeg_required INTERFACE)
add_library(FFmpeg::required ALIAS ffmpeg_required)
target_link_libraries(
    ffmpeg_required
    INTERFACE ffmpeg.avcodec.static_pic
              ffmpeg.swscale.static_pic
              ffmpeg.avutil.static_pic
              ffmpeg.avcodec.headers
              ffmpeg.swscale.headers
              ffmpeg.avutil.headers
              ${CMAKE_DL_LIBS})

if(UNIX)
    target_link_libraries(ffmpeg_required INTERFACE m)
endif()

if(WIN32)
    target_link_libraries(ffmpeg_required INTERFACE bcrypt)
endif()
