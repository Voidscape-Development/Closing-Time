#!/bin/bash
#
# Installs what the offscreen test harness needs to build and run.
#
# The plugin itself is only ever built against a real OBS install, but tests/ compiles model/,
# render/ and util/ on their own -- so a session that wants to run the checks needs Qt 6 and the
# libobs headers and nothing else. Without this every session spends several minutes rediscovering
# that and installing them by hand before it can test anything.
set -euo pipefail

# Local checkouts have their own toolchains; this is only for the web sessions that start empty.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  SUDO="sudo"
fi

# Already present from a cached container: nothing to do, and re-running apt would only be slow.
if pkg-config --exists libobs Qt6Gui Qt6Svg 2>/dev/null; then
  echo "closing-time: Qt 6 and libobs already present"
else
  export DEBIAN_FRONTEND=noninteractive

  $SUDO apt-get update -qq

  # qt6-svg-dev is not optional: bridge and divider artwork are SVG tiles, so the renderer does
  # not link without QSvgRenderer.
  $SUDO apt-get install -y -qq --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    qt6-base-dev \
    qt6-svg-dev \
    libobs-dev

  echo "closing-time: installed Qt 6 and libobs"
fi

# gersemi formats the CMake files and CI checks it; clang-format ships with the image already.
if ! command -v gersemi >/dev/null 2>&1; then
  pip install --quiet --break-system-packages gersemi >/dev/null 2>&1 ||
    pip install --quiet gersemi >/dev/null 2>&1 ||
    echo "closing-time: could not install gersemi; CMake formatting will not be checkable"
fi

# Qt needs a platform plugin and there is no display here. The harness sets this itself, but
# exporting it means an ad-hoc binary built during a session does not have to remember to.
# Defaulted, because the variable is only guaranteed when the hook is run by the harness and
# `set -u` would otherwise turn a hand-run of this script into a failure.
echo 'export QT_QPA_PLATFORM=offscreen' >> "${CLAUDE_ENV_FILE:-/dev/null}"

echo "closing-time: ready -- cmake -S . -B build -DENABLE_TESTS=ON && cmake --build build --target closing-time-tests"
