cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Closing Time draws its credit roll with QPainter and ships a Qt designer window, so both
# the frontend API and Qt are hard requirements rather than the template's opt-in features.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "Closing Time requires both ENABLE_FRONTEND_API and ENABLE_QT to be enabled.")
endif()

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

# Svg is here for the bridge and divider artwork: both are drawn from SVG tiles rather than
# from glyphs, so QSvgRenderer is part of the render path and not just an icon loader.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
)

# FFmpeg is what decodes a video logo -- WebM, MP4 and the rest. GIF, APNG and animated WebP come
# through Qt and need none of it, so this is optional rather than required: without it the plugin
# builds and animates exactly as before, and a video file picked as a logo is reported in the
# designer as needing a build that has it. OBS ships these libraries, so a machine set up to build
# plugins against the OBS dependencies usually already has them.
option(ENABLE_VIDEO_LOGOS "Decode video logos with FFmpeg when it is available" ON)

set(CLOSING_TIME_HAVE_FFMPEG OFF)

if(ENABLE_VIDEO_LOGOS)
  find_package(FFmpeg COMPONENTS avcodec avformat avutil swscale QUIET)

  if(NOT FFmpeg_FOUND AND NOT TARGET FFmpeg::avcodec)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(CLOSING_TIME_FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
    endif()
  endif()

  if(TARGET FFmpeg::avcodec)
    set(CLOSING_TIME_HAVE_FFMPEG ON)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale)
  elseif(CLOSING_TIME_FFMPEG_FOUND)
    set(CLOSING_TIME_HAVE_FFMPEG ON)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::CLOSING_TIME_FFMPEG)
  endif()
endif()

if(CLOSING_TIME_HAVE_FFMPEG)
  message(STATUS "Closing Time: video logos enabled (FFmpeg found)")
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CLOSING_TIME_HAVE_FFMPEG=1)
else()
  message(STATUS "Closing Time: video logos disabled (FFmpeg not found); GIF/APNG/WebP still animate")
endif()

# The document, the renderer and the parser. None of them carry Q_OBJECT or touch Qt Widgets,
# which is what lets the test harness compile them on their own -- so the list is shared between
# the two targets rather than written out twice and left to drift.
set(
  CLOSING_TIME_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/BridgeArt.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/BridgeArt.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/CreditsModel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/CreditsModel.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/DividerArt.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/DividerArt.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/EndingAction.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/EndingAction.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/FontBundle.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/FontBundle.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/StyleLibrary.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/StyleLibrary.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/AnimatedLogo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/AnimatedLogo.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/BridgeArtRenderer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/BridgeArtRenderer.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/DividerArtRenderer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/DividerArtRenderer.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/FontResolution.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/FontResolution.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/ImageEffects.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/ImageEffects.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderThread.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderThread.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/StripRenderer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/StripRenderer.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/SvgArt.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/SvgArt.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/CsvParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/CsvParser.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/FontFiles.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/FontFiles.hpp"
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${CLOSING_TIME_CORE_SOURCES}
    src/plugin-main.cpp
    src/source/CreditsSource.cpp
    src/source/CreditsSource.hpp
    src/ui/CsvImportDialog.cpp
    src/ui/CsvImportDialog.hpp
    src/ui/DesignerDialog.cpp
    src/ui/DesignerDialog.hpp
    src/ui/FontDialog.cpp
    src/ui/FontDialog.hpp
    src/ui/PreviewWidget.cpp
    src/ui/PreviewWidget.hpp
    src/ui/SectionEditor.cpp
    src/ui/SectionEditor.hpp
    src/ui/StyleControls.cpp
    src/ui/StyleControls.hpp
    src/ui/StyleLibraryDialog.cpp
    src/ui/StyleLibraryDialog.hpp
    src/ui/ToolButtons.cpp
    src/ui/ToolButtons.hpp
)

# Off by default so a plain plugin build is unchanged, and so nobody packaging the plugin has to
# have a test runner they never asked for. See tests/README.md for how to turn it on.
option(ENABLE_TESTS "Build the offscreen test harness" OFF)

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
