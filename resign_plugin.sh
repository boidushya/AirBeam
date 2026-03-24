#!/usr/bin/env bash
# Re-sign the AirBeam HAL plugin and restart coreaudiod.
# Run this after reinstalling the plugin.

set -e

PLUGIN="/Library/Audio/Plug-Ins/HAL/AirBeamASP.driver"

if [ ! -d "$PLUGIN" ]; then
  echo "Plugin not found at $PLUGIN"
  exit 1
fi

echo "Signing $PLUGIN..."
sudo codesign --force --deep -s - "$PLUGIN"

echo "Restarting coreaudiod..."
sudo killall coreaudiod

echo "Done. The AirBeam device should appear in a few seconds."
