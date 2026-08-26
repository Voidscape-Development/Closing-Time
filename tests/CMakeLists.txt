# The offscreen harness. Compiles model/, render/ and util/ directly rather than linking the
# plugin module: none of them carry Q_OBJECT, so the harness needs no moc and no Qt Widgets, and a
# rebuild after a renderer edit is a handful of files rather than the whole plugin.

add_executable(closing-time-tests)

target_sources(
  closing-time-tests
  PRIVATE
    harness/Fixtures.cpp
    harness/Fixtures.hpp
    harness/Harness.cpp
    harness/Harness.hpp
    harness/Probe.cpp
    harness/Probe.hpp
    harness/main.cpp
    suites/AnimatedLogos.cpp
    suites/Backgrounds.cpp
    suites/BridgedRows.cpp
    suites/Dividers.cpp
    suites/Fonts.cpp
    suites/Layout.cpp
    suites/LogoRows.cpp
    suites/Persistence.cpp
    suites/StickyBlocks.cpp
    suites/StyleLibrary.cpp
    suites/SectionTypes.cpp
    suites/SubtitleHeadings.cpp
    ${CLOSING_TIME_CORE_SOURCES}
)

# `src` for the plugin's own headers, `tests` so a suite includes "harness/Probe.hpp" by the same
# path from wherever it sits.
target_include_directories(closing-time-tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")

# Svg is here for the same reason the plugin needs it: bridge and divider artwork are SVG tiles.
# Widgets is deliberately absent -- nothing under test opens a window.
target_link_libraries(
  closing-time-tests
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Svg
)

if(TARGET plugin-support)
  target_link_libraries(closing-time-tests PRIVATE plugin-support)
endif()

set_target_properties(
  closing-time-tests
  PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
)

# One CTest entry for the whole run: the suites share a process and a font database, and the
# harness already reports which of them failed. `--verbose` so a passing run still lists them.
add_test(NAME closing-time-tests COMMAND closing-time-tests --verbose)

# Offscreen is set in main() as well, but a stray platform plugin in the environment would be
# picked up before that; setting it here means `ctest` cannot inherit one.
set_tests_properties(closing-time-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
