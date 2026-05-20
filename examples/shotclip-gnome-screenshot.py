#!/usr/bin/env python3
"""shotclip-gnome-screenshot — pair GNOME's native screenshot UI with shotclip.

Invokes the xdg-desktop-portal Screenshot interface in interactive mode
(same UI as the built-in Print key on stock GNOME), then runs shotclip on
the captured file so the clipboard gets the full Nautilus-style MIME set
(text/uri-list, x-special/gnome-copied-files, application/vnd.portal.*,
image/png) instead of just image/png.

Wire it up via GNOME Settings -> Keyboard -> Custom Shortcuts:

    Command:  /home/USER/.local/bin/shotclip-gnome-screenshot
    Shortcut: Print  (or whatever you prefer)

Equivalent to running this gdbus one-liner and then handing the resulting
file to shotclip:

    gdbus call --session \\
        --dest org.freedesktop.portal.Desktop \\
        --object-path /org/freedesktop/portal/desktop \\
        --method org.freedesktop.portal.Screenshot.Screenshot \\
        "" "{'interactive': <true>, 'modal': <true>}"

Requires:
    - python3 + python3-gi
    - shotclip on PATH
    - xdg-desktop-portal + xdg-desktop-portal-gnome
"""
import os
import subprocess
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib


def take_screenshot_via_portal() -> str | None:
    """Show the GNOME Screenshot UI, return the file:// URI of the result.

    Returns None if the user cancelled, raises on portal/D-Bus errors.
    """
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    loop = GLib.MainLoop()
    result = {"code": None, "uri": None}

    def on_response(_conn, _sender, _path, _iface, _signal, params):
        code, results = params.unpack()
        result["code"] = code
        result["uri"] = (results or {}).get("uri")
        loop.quit()

    # Subscribe BEFORE the call so we can't miss a fast Response.
    bus.signal_subscribe(
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        None,
        None,
        Gio.DBusSignalFlags.NONE,
        on_response,
    )

    options = GLib.Variant(
        "a{sv}",
        {
            "interactive": GLib.Variant("b", True),
            "modal":       GLib.Variant("b", True),
        },
    )
    bus.call_sync(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Screenshot",
        "Screenshot",
        GLib.Variant("(sa{sv})", ("", options)),
        GLib.VariantType.new("(o)"),
        Gio.DBusCallFlags.NONE,
        -1,
        None,
    )

    loop.run()

    # Portal response codes: 0 = success, 1 = user cancelled, 2 = other.
    if result["code"] == 1:
        return None
    if result["code"] != 0 or not result["uri"]:
        raise RuntimeError(
            f"portal returned code={result['code']} uri={result['uri']!r}"
        )
    return result["uri"]


def main() -> int:
    try:
        uri = take_screenshot_via_portal()
    except (GLib.Error, RuntimeError) as e:
        print(f"shotclip-gnome-screenshot: {e}", file=sys.stderr)
        return 1

    if uri is None:
        return 0  # user cancelled — quiet exit, matches GNOME's own behavior

    path = Gio.File.new_for_uri(uri).get_path()
    if not path:
        print(f"shotclip-gnome-screenshot: cannot resolve URI {uri}", file=sys.stderr)
        return 1

    # Replace any previous shotclip session so back-to-back screenshots
    # don't leave orphans.
    subprocess.run(["pkill", "-9", "-x", "shotclip"], check=False)
    os.execvp("shotclip", ["shotclip", path])


if __name__ == "__main__":
    sys.exit(main())
