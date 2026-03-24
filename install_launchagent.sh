#!/usr/bin/env bash
# Installs the airbeam-watch LaunchAgent with the correct path for your system.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLIST_SRC="$SCRIPT_DIR/com.airbeam.watch.plist"
PLIST_DST="$HOME/Library/LaunchAgents/com.airbeam.watch.plist"

# Unload existing agent if running
launchctl unload "$PLIST_DST" 2>/dev/null || true

# Generate plist with correct path
sed "s|__AIRBEAM_DIR__|$SCRIPT_DIR|g" "$PLIST_SRC" > "$PLIST_DST"

launchctl load "$PLIST_DST"

echo "LaunchAgent installed. airbeam-watch.sh is running from $SCRIPT_DIR"
