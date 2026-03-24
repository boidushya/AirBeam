#!/usr/bin/env bash
# Full reinstall of AirBeam: build, install plugin, sign, and restart coreaudiod.

set -e

cd "$(dirname "$0")"

echo "Building AirBeam..."
cmake -S . -B build -GNinja -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build --target AirBeamASP

PLUGIN_SRC="build/source/AirBeamASP/AirBeamASP.driver"
PLUGIN_DST="/Library/Audio/Plug-Ins/HAL/AirBeamASP.driver"

if [ ! -d "$PLUGIN_SRC" ]; then
  echo "Build failed — plugin not found at $PLUGIN_SRC"
  exit 1
fi

echo "Installing plugin..."
sudo rm -rf "$PLUGIN_DST"
sudo cp -r "$PLUGIN_SRC" "$PLUGIN_DST"

echo "Signing plugin..."
sudo codesign --force --deep -s - "$PLUGIN_DST"

echo "Restarting coreaudiod..."
sudo killall coreaudiod

echo "Done. The AirBeam device should appear in a few seconds."
