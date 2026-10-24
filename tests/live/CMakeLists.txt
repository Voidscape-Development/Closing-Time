# The live harness: the plugin module loaded into a real libobs and played on a graphics device.
# See LiveHost.cpp for what it checks and how to run it. Linux only; needs X11 for the EGL display.

find_package(X11 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(closing-time-live-tests)

file(
  GLOB _live_core_sources
  "${CMAKE_SOURCE_DIR}/src/model/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/render/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/util/*.cpp"
)

# The preview widget is the one piece of the designer worth driving here, and the only one with a moc.
target_sources(
  closing-time-live-tests
  PRIVATE
    LiveHost.cpp
    ${_live_core_sources}
    "${CMAKE_SOURCE_DIR}/src/ui/PreviewWidget.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/PreviewWidget.hpp"
)

target_include_directories(closing-time-live-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_link_libraries(
  closing-time-live-tests
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Svg Qt6::Widgets Qt6::Test X11::X11
)

if(TARGET plugin-support)
  target_link_libraries(closing-time-live-tests PRIVATE plugin-support)
endif()

set_target_properties(
  closing-time-live-tests
  PROPERTIES AUTOMOC ON AUTOUIC OFF AUTORCC OFF CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
)

# Needs the plugin built first, since that is what it loads.
add_dependencies(closing-time-live-tests ${CMAKE_PROJECT_NAME})
