#!/bin/bash
# Example shotclip integration: capture+annotate with Flameshot, then put
# the saved image on the clipboard with the full Nautilus-style MIME types.
#
# Wire it up via GNOME Settings -> Keyboard -> View and Customize Shortcuts ->
# Custom Shortcuts. Bind this file to your Print key:
#
#     Name:     Annotated screenshot to clipboard
#     Command:  /home/USER/.local/bin/shotclip-flameshot
#     Shortcut: Print
#
# Requires:
#   - flameshot built with the GNOME-Wayland portal fix (>= flameshot-org PR #4664)
#   - shotclip installed somewhere on PATH (e.g. ~/.local/bin/shotclip)
#   - xdg-desktop-portal-gnome + xdg-desktop-portal-gtk

set -u

OUTDIR="$HOME/Pictures/Screenshots"
mkdir -p "$OUTDIR"

# Flameshot needs a proper systemd app scope so the screenshot portal trusts it.
systemd-run --user --scope --unit="app-gnome-org.flameshot.Flameshot-$(date +%s%N)" -- \
    flameshot gui -p "$OUTDIR"

# After Flameshot exits, find the newest .png it just saved.
sleep 0.2
LATEST=$(ls -t "$OUTDIR"/*.png 2>/dev/null | head -1)
if [ -n "$LATEST" ] && [ -f "$LATEST" ]; then
    pkill -9 -x shotclip 2>/dev/null
    setsid shotclip "$LATEST" > /dev/null 2>&1 < /dev/null &
    disown
fi
