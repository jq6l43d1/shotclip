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

# After Flameshot exits, find the newest .png it just saved and put it on
# the clipboard. shotclip forks to the background after set_selection, so
# this returns quickly while the child keeps serving paste requests.
#
# We deliberately don't use --paste-once here: GNOME Shell's built-in
# clipboard manager reads every advertised MIME type right after a new
# selection appears, which would trigger paste-once on whatever type it
# happens to fetch first — leaving only that single type on the clipboard
# by the time the user actually pastes. Same caveat as wl-copy(1) -o.
sleep 0.2
LATEST=$(ls -t "$OUTDIR"/*.png 2>/dev/null | head -1)
if [ -n "$LATEST" ] && [ -f "$LATEST" ]; then
    # Replace any previous shotclip session so taking screenshots in a row
    # doesn't accumulate background processes (each new set_selection would
    # cancel the previous anyway, but be explicit).
    pkill -9 -x shotclip 2>/dev/null
    shotclip "$LATEST"
fi
