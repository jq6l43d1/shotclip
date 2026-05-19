PREFIX ?= $(HOME)/.local
CC     ?= cc

XDG_SHELL_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

PKGS := wayland-client gio-2.0 gio-unix-2.0
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

all: shotclip

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

shotclip: shotclip.c xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -o $@ shotclip.c xdg-shell-protocol.c $(LDLIBS)

install: shotclip
	install -D -m 755 shotclip $(DESTDIR)$(PREFIX)/bin/shotclip
	install -D -m 755 examples/shotclip-flameshot.sh $(DESTDIR)$(PREFIX)/bin/shotclip-flameshot

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/shotclip $(DESTDIR)$(PREFIX)/bin/shotclip-flameshot

clean:
	rm -f shotclip xdg-shell-client-protocol.h xdg-shell-protocol.c

.PHONY: all install uninstall clean
