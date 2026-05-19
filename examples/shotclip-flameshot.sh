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
#   - shotclip installed on PATH (e.g. ~/.local/bin/shotclip)
#   - xdg-desktop-portal-gnome + xdg-desktop-portal-gtk

set -u

OUTDIR="$HOME/Pictures/Screenshots"
mkdir -p "$OUTDIR"

# Flameshot needs a proper systemd app scope so the screenshot portal trusts it.
systemd-run --user --scope --unit="app-gnome-org.flameshot.Flameshot-$(date +%s%N)" -- \
    flameshot gui -p "$OUTDIR"

# After Flameshot exits, find the newest .png it just saved and put it on the
# clipboard. --paste-once makes shotclip exit after the first paste, so we
# don't leave a stale clipboard owner around if you take another screenshot.
sleep 0.2
LATEST=$(ls -t "$OUTDIR"/*.png 2>/dev/null | head -1)
if [ -n "$LATEST" ] && [ -f "$LATEST" ]; then
    shotclip --paste-once "$LATEST"
fi
