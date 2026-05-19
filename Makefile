PREFIX ?= $(HOME)/.local
CC     ?= cc

PROTODIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(PROTODIR)/stable/xdg-shell/xdg-shell.xml
PRIMARY_SEL_XML := $(PROTODIR)/unstable/primary-selection/primary-selection-unstable-v1.xml

PKGS := wayland-client gio-2.0 gio-unix-2.0
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

GEN_HDRS := xdg-shell-client-protocol.h primary-selection-unstable-v1-client-protocol.h
GEN_SRCS := xdg-shell-protocol.c        primary-selection-unstable-v1-protocol.c

all: shotclip

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@
xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

primary-selection-unstable-v1-client-protocol.h: $(PRIMARY_SEL_XML)
	wayland-scanner client-header $< $@
primary-selection-unstable-v1-protocol.c: $(PRIMARY_SEL_XML)
	wayland-scanner private-code $< $@

shotclip: shotclip.c $(GEN_SRCS) $(GEN_HDRS)
	$(CC) $(CFLAGS) -o $@ shotclip.c $(GEN_SRCS) $(LDLIBS)

install: shotclip
	install -D -m 755 shotclip $(DESTDIR)$(PREFIX)/bin/shotclip
	install -D -m 755 examples/shotclip-flameshot.sh $(DESTDIR)$(PREFIX)/bin/shotclip-flameshot

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/shotclip $(DESTDIR)$(PREFIX)/bin/shotclip-flameshot

clean:
	rm -f shotclip $(GEN_HDRS) $(GEN_SRCS)

.PHONY: all install uninstall clean
