# shotclip

A small libwayland-direct tool that puts a file on the Wayland clipboard with
the same MIME types Nautilus uses — including the `application/vnd.portal.*`
types required by GFile-aware paste consumers like **Claude Code**.

```
$ shotclip ~/Pictures/Screenshots/whatever.png &
$ wl-paste --list-types
application/vnd.portal.files
application/vnd.portal.filetransfer
text/uri-list
x-special/gnome-copied-files
text/plain;charset=utf-8
text/plain
image/png
```

That's the same set Nautilus produces when you right-click → Copy a file. So
the resulting clipboard pastes correctly into apps that expect a file
reference, not just raw image bytes.

## Why this exists

On GNOME Wayland (tested on Ubuntu 26.04 / Mutter 50), every off-the-shelf
clipboard tool falls short for the use case of "paste a screenshot into Claude
Code":

- `wl-copy --type image/png` produces only `image/png`. Claude Code reads
  `application/vnd.portal.filetransfer`, so the paste does nothing.
- `wl-copy --type text/uri-list` produces the right URI string but no portal
  registration, so portal-aware receivers can't open the file.
- Flameshot's built-in `-c` clipboard, copyq, PyGObject `Gdk.ContentProvider`
  subclasses, PyQt's `QMimeData`, even a hand-written GTK4 C program — all
  end up advertising only `image/png` or only `text/plain`. The compositor
  doesn't filter MIME types; the tooling silently drops them upstream.

The actual problem is twofold:

1. The `application/vnd.portal.filetransfer` and `application/vnd.portal.files`
   MIME types aren't raw data — they're keys to live D-Bus sessions registered
   with `org.freedesktop.portal.FileTransfer` and
   `org.freedesktop.portal.Documents`. You have to call the portal yourself
   to mint them.
2. `wl_data_device.set_selection` on Wayland is only honored when accompanied
   by a serial from a recent input event. GTK4's clipboard layer, when called
   from a non-interactive activate handler, passes serial `0` and Mutter
   silently discards the request — even though `wl_data_source.offer` for the
   non-text MIME types fired correctly. wl-copy works because it talks to
   libwayland directly, opens a hidden surface, and uses the
   `wl_keyboard.enter` serial it receives once the surface gets focus.

`shotclip` does what wl-copy does for the serial trick, plus the portal
D-Bus dance:

1. Bind to `wl_compositor`, `wl_shm`, `wl_seat`, `wl_data_device_manager`, and
   `xdg_wm_base` from the registry.
2. Create a 1×1 transparent `xdg_toplevel` surface.
3. Pre-call `org.freedesktop.portal.FileTransfer.StartTransfer` +
   `AddFiles(fd)` and `org.freedesktop.portal.Documents.AddFull(fd)` over
   D-Bus so the portal keys are ready before any paste request arrives.
4. Wait for `wl_keyboard.enter`. Capture the serial.
5. Create a `wl_data_source`, advertise all seven MIME types, and call
   `wl_data_device.set_selection(source, captured_serial)`.
6. Serve `wl_data_source.send` events: read the file for `image/png`, format
   URIs for the text types, return the cached portal keys for the
   `vnd.portal.*` types.

The process stays alive serving paste requests until the compositor cancels
the source (i.e. something else takes the clipboard) or you close the
1×1 window.

## Installation

Build dependencies on Ubuntu/Debian:

```bash
sudo apt install libwayland-dev wayland-protocols libglib2.0-dev pkg-config build-essential
```

Then build and install to `~/.local`:

```bash
make
make install            # -> ~/.local/bin/shotclip + ~/.local/bin/shotclip-flameshot
```

Or system-wide:

```bash
sudo make install PREFIX=/usr/local
```

You also need an `xdg-desktop-portal` backend that implements `FileTransfer`
and `Documents` (on GNOME this is `xdg-desktop-portal-gnome` plus
`xdg-desktop-portal-gtk`).

## Usage

```bash
shotclip /path/to/file
```

It backgrounds itself and serves the clipboard until something else takes
over. A tiny 1×1 transparent window briefly appears (and may show up in your
window list with the title "Copying to clipboard…") — that surface is what
gives us the keyboard-enter serial. You can ignore it.

## Example: Flameshot → Claude Code on GNOME Wayland

`examples/shotclip-flameshot.sh` (installed as `~/.local/bin/shotclip-flameshot`)
captures a screenshot with Flameshot, saves it to `~/Pictures/Screenshots/`,
and then runs `shotclip` on the saved file:

1. Bind `~/.local/bin/shotclip-flameshot` to your `Print` key in
   **Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts**.
2. Press Print, drag a region, annotate in Flameshot, save.
3. The screenshot lands in `~/Pictures/Screenshots/` and on your clipboard.
4. Paste into Claude Code with **Ctrl+Shift+V**. Image appears.

For Flameshot itself to work on GNOME Wayland, you currently need a build
that includes [flameshot-org/flameshot#4664](https://github.com/flameshot-org/flameshot/pull/4664)
(non-empty `parent_window` to xdg-desktop-portal). At the time of writing,
that fix is in `master` but not in any stable release.

## Limitations

- GNOME 50 / Mutter on Ubuntu 26.04 is the only environment this has been
  tested on. Should work on any compositor that supports `xdg-shell` and
  `wl_data_device_manager`, but the portal types only matter on
  xdg-desktop-portal-aware desktops.
- The 1×1 surface flickers briefly. A more careful implementation could use
  `xdg-foreign` or layer-shell, but Mutter doesn't expose layer-shell.
- The portal `FileTransfer` session is configured with default
  `autostop=true`, so the clipboard is single-use for portal receivers. After
  one paste, subsequent portal-based pastes return nothing. The other MIME
  types (`image/png`, `text/uri-list`, etc.) keep working as long as
  `shotclip` is running.

## License

GPL-3.0. See [LICENSE](LICENSE).
