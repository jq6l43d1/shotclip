# shotclip

A small libwayland-direct tool that puts files on the Wayland clipboard with
the same MIME types Nautilus uses — including the `application/vnd.portal.*`
types required by GFile-aware paste consumers like **Claude Code**.

```
$ shotclip ~/Pictures/Screenshots/whatever.png
$ wl-paste --list-types
image/png
application/vnd.portal.files
application/vnd.portal.filetransfer
text/plain
text/plain;charset=utf-8
x-special/gnome-copied-files
text/uri-list
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
   from a non-interactive code path, passes serial `0` and Mutter silently
   discards the request — even though `wl_data_source.offer` for the non-text
   MIME types fired correctly. `wl-copy` works because it talks to libwayland
   directly, opens a hidden surface, and uses the `wl_keyboard.enter` serial
   it receives once the surface gets focus.

`shotclip` does what `wl-copy` does for the serial trick, plus the portal
D-Bus dance:

1. Bind the standard Wayland globals plus `xdg_wm_base` and
   (optionally) `zwp_primary_selection_device_manager_v1`.
2. Pre-call `org.freedesktop.portal.FileTransfer.StartTransfer` + `AddFiles`
   and `org.freedesktop.portal.Documents.AddFull` over D-Bus so the portal
   keys are ready before any paste request arrives.
3. Create a 1×1 `xdg_toplevel` surface, wait for `wl_keyboard.enter`,
   capture the serial.
4. Build a `wl_data_source`, advertise the full MIME set, call
   `wl_data_device.set_selection(source, captured_serial)`.
5. **Immediately destroy the surface**. `wl_data_source` lives on the
   `wl_data_device` (per-seat), not on the surface, so the clipboard ownership
   survives — and the focus window stops showing up in Alt-Tab.
6. Serve `wl_data_source.send` events from the now-windowless process until
   the compositor cancels the source (something else took the clipboard) or
   `--paste-once` exit fires.

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

```
shotclip [OPTIONS] FILE [FILE ...]
```

`shotclip` forks to the background after `set_selection` succeeds (matching
`wl-copy`'s default), and stays alive serving paste requests until the
compositor cancels the selection. Pass multiple files to copy them as a set,
the way file managers do.

If `FILE` is omitted and standard input is not a TTY, paths are read from
stdin, one per line — handy for piping `find`/`ls`/`fd` output:

```bash
ls ~/Pictures/Screenshots/2026-05-19*.png | shotclip
find ~/Documents -name '*.pdf' -newer /tmp/marker | shotclip
```

### Options

| Flag | Behavior |
|------|----------|
| `-p, --primary` | Set the primary selection instead of the regular clipboard. |
| `-o, --paste-once` | Exit after the first paste of a data MIME type. Good for screenshot workflows where you only need one paste. Note: pasting into XWayland windows is known to break with this flag — same caveat as `wl-copy(1)`. |
| `-f, --foreground` | Don't fork — stay attached to the terminal. |
| `-c, --clear` | Clear the clipboard (or primary selection with `-p`). Takes no FILE args. |
| `-t, --type MIME` | Only advertise this single MIME type. Useful for `-t text/uri-list` to force a URI-only clipboard. |
| `-s, --seat NAME` | Use the seat with this name (as advertised in `wl_seat.name`) instead of the first one. Only relevant on multi-seat systems. |
| `--no-image-data` | Don't offer the file's content-type bytes; URI/portal types only. |
| `--keep-visible` | Don't destroy the focus window (debugging). |
| `-h, --help` | Usage. |
| `-V, --version` | Version. |

### What gets advertised

For a single file (e.g. `shotclip foo.png`):

- the file's detected content type (`image/png`, `application/pdf`, …)
- `text/uri-list` with one `file://` URI
- `x-special/gnome-copied-files` (Nautilus convention)
- `text/plain;charset=utf-8` + `text/plain` (the path)
- `application/vnd.portal.filetransfer` + `application/vnd.portal.files` (portal handles)

For multiple files (e.g. `shotclip a.png b.png`):

- same set, but no content-type bytes (Nautilus does the same — bytes-of-one only makes sense for single files)
- `text/uri-list` is multiline, `x-special/gnome-copied-files` includes all URIs, portal keys reference all files

## Example: Flameshot → Claude Code on GNOME Wayland

`examples/shotclip-flameshot.sh` (installed as `~/.local/bin/shotclip-flameshot`)
captures a screenshot with Flameshot, saves it to `~/Pictures/Screenshots/`,
and then runs `shotclip --paste-once` on the saved file:

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
  tested on. Should work on any compositor that supports `xdg-shell`,
  `wl_data_device_manager`, and survives the focus-window destroy trick.
- The 1×1 focus window flashes briefly during `set_selection`. It then
  destroys itself, so Alt-Tab is clean.
- The portal `FileTransfer` session uses default `autostop=true`, so the
  portal MIME types resolve only once. After that single paste, the other
  MIME types (`image/png`, `text/uri-list`, etc.) keep working as long as
  `shotclip` is running — but if you specifically need the portal types
  multiple times, run a second `shotclip` invocation.

## Related and prior art

[**bugaevc/wl-clipboard#71**](https://github.com/bugaevc/wl-clipboard/issues/71)
— "wl-copy: how to craft a multi-mimetype copy" — open since 2019, still open
in 2025. Users have repeatedly asked for `wl-copy` to advertise more than one
MIME type, with use cases including pasting browser-grabbed images, restoring
rich text in Confluence/Office round-trips, GIMP clipboard history, and —
exactly what shotclip targets — *"copy file paths as `text/uri-list`, and
paste them into file managers… I'd like to copy a version of absolute paths
as `text/plain` too. That's what thunar does."* (@lilydjwg, 2021).

The wl-clipboard maintainer's position there is that generic CLI-driven
multi-MIME is too design-y for a small tool — every proposed flag syntax
(`-t a,b`, `-C converter`, `--extra MIME <(…)`) brings open questions, and
the real consumers are clipboard managers that should use libwayland
directly. Fair.

`shotclip` takes the **narrower, opinionated path** instead: it doesn't try
to solve "arbitrary MIME N-tuples." It solves "I have one or more files, give
me the clipboard a file manager would give me" — including the
`application/vnd.portal.*` keys that didn't even come up in that thread but
turn out to be the missing piece for pasting into modern GFile-aware apps
like Claude Code on GNOME Wayland. Where wl-copy is a `cat`-for-clipboard,
shotclip is a `cp`-to-clipboard.

Other relevant tooling:

- [`wl-clipboard`](https://github.com/bugaevc/wl-clipboard) (`wl-copy`/`wl-paste`)
  — the upstream Wayland clipboard CLI. shotclip mimics its flag names and
  fork-to-background default where they carry over.
- [`xdg-desktop-portal`](https://github.com/flatpak/xdg-desktop-portal)
  `org.freedesktop.portal.FileTransfer` and `org.freedesktop.portal.Documents`
  — the D-Bus services whose keys end up in our portal MIME payloads.
- Nautilus (`nautilus-clipboard.c`) — the reference implementation of the
  exact MIME set we mimic.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
