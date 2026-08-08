# w98wm - POSIX make (GNU make / BSD make の双方で動くこと)
# SPEC §1.4 のビルドプロファイル: core (既定) / xft / static

PREFIX     = /usr/local
BINDIR     = $(PREFIX)/bin
MANDIR     = $(PREFIX)/share/man

CC         = cc
PKG_CONFIG = pkg-config

# SPEC §9.2 の最適化方針。-fno-stack-protector は安全性のため使わない。
CFLAGS_OPT = -Os -fno-plt -fno-asynchronous-unwind-tables \
             -ffunction-sections -fdata-sections
CFLAGS_WARN= -Wall -Wextra -Wpedantic -Wno-unused-parameter \
             -Wshadow -Wstrict-prototypes -Wmissing-prototypes
LDFLAGS_GC = -Wl,--gc-sections

REQ_PKGS   = xcb xcb-randr xcb-sync xcb-keysyms
PKG_CFLAGS != $(PKG_CONFIG) --cflags $(REQ_PKGS) 2>/dev/null
PKG_LIBS   != $(PKG_CONFIG) --libs   $(REQ_PKGS) 2>/dev/null

CFLAGS  = -std=c99 -D_POSIX_C_SOURCE=200809L $(CFLAGS_OPT) $(CFLAGS_WARN) \
          $(PKG_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS = $(LDFLAGS_GC) $(EXTRA_LDFLAGS)
LIBS    = $(PKG_LIBS)

OBJS = src/main.o src/wm.o src/event.o src/client.o src/icccm.o \
       src/ewmh.o src/atoms.o src/stack.o src/focus.o src/move.o \
       src/layout.o src/input.o src/config.o src/util.o src/type.o

BIN = w98wm

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJS): src/w98wm.h src/atoms.h src/brand.h src/compat.h

clean:
	rm -f $(BIN) $(OBJS) tests/unit/unit_tests

install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

test: $(BIN)
	sh tests/run-tests.sh

check-brand:
	sh tools/check-brand.sh

memcheck: $(BIN)
	sh tools/memcheck.sh

.PHONY: all clean install test check-brand memcheck
