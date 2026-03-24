#!/usr/bin/env bash
# Auto-selects AirBeam Bedroom device when macOS falls back to a non-intentional output.
#
# To disable:  touch ~/.airbeam-pause
# To re-enable: rm ~/.airbeam-pause

## CONFIGURE THESE FOR YOUR SETUP ##
# Run `SwitchAudioSource -a` to see all available device names.
# Set DEVICE to your AirBeam virtual output device name.
# Add any device you'd intentionally switch to in INTENTIONAL — the script won't override those.
DEVICE="D6D63AF4F8EF@Bedroom"
INTERVAL=5

INTENTIONAL=(
  "D6D63AF4F8EF@Bedroom"
  "Boidushya's AirPods Pro"
  "Bedroom"
  "BlackHole 2ch"
  "Speaker + Recording"
)

is_intentional() {
  local current="$1"
  for dev in "${INTENTIONAL[@]}"; do
    if [ "$current" = "$dev" ]; then
      return 0
    fi
  done
  return 1
}

while true; do
  if [ -f "$HOME/.airbeam-pause" ]; then
    sleep "$INTERVAL"
    continue
  fi

  current=$(SwitchAudioSource -c -t output)
  if ! is_intentional "$current"; then
    if SwitchAudioSource -a -t output | grep -q "$DEVICE"; then
      SwitchAudioSource -s "$DEVICE" -t output
      echo "$(date): Switched output from '$current' to $DEVICE"
    fi
  fi
  sleep "$INTERVAL"
done
