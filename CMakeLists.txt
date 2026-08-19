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

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/model/BridgeArt.cpp
    src/model/BridgeArt.hpp
    src/model/CreditsModel.cpp
    src/model/CreditsModel.hpp
    src/model/DividerArt.cpp
    src/model/DividerArt.hpp
    src/model/EndingAction.cpp
    src/model/EndingAction.hpp
    src/plugin-main.cpp
    src/render/BridgeArtRenderer.cpp
    src/render/BridgeArtRenderer.hpp
    src/render/DividerArtRenderer.cpp
    src/render/DividerArtRenderer.hpp
    src/render/ImageEffects.cpp
    src/render/ImageEffects.hpp
    src/render/RenderThread.cpp
    src/render/RenderThread.hpp
    src/render/StripRenderer.cpp
    src/render/StripRenderer.hpp
    src/render/SvgArt.cpp
    src/render/SvgArt.hpp
    src/source/CreditsSource.cpp
    src/source/CreditsSource.hpp
    src/ui/CsvImportDialog.cpp
    src/ui/CsvImportDialog.hpp
    src/ui/DesignerDialog.cpp
    src/ui/DesignerDialog.hpp
    src/ui/PreviewWidget.cpp
    src/ui/PreviewWidget.hpp
    src/ui/SectionEditor.cpp
    src/ui/SectionEditor.hpp
    src/ui/StyleControls.cpp
    src/ui/StyleControls.hpp
    src/ui/ToolButtons.cpp
    src/ui/ToolButtons.hpp
    src/util/CsvParser.cpp
    src/util/CsvParser.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
