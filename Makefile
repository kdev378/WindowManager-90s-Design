# w98wm - POSIX make (GNU make / BSD make の双方で動くこと)
# SPEC §1.4 のビルドプロファイル: core (既定) / xft / static

PREFIX     = /usr/local
BINDIR     = $(PREFIX)/bin
MANDIR     = $(PREFIX)/share/man

CC         = cc
PKG_CONFIG = pkg-config

# ------------------------------------------------------------------
# ビルドの切り替え（コマンドラインで 1 を渡す: make DEBUG=1 など）
#
# POSIX make には条件分岐が無いので、「変数名の一部を変数で組み立てる」
# 方式で選ぶ（$(CFLAGS_OPT_$(DEBUG)) など）。GNU make / BSD make の双方で動く。
#   DEBUG=1    -O0 -g3。--gc-sections も外す（gdb で全関数が見えるように）
#   STATIC=1   静的リンク (SPEC §1.4 で experimental)。.a が揃っていること
#   XFT=1      未実装。黙って core を作らず、エラーで止める
#   NO_SHAPE=1 xcb-shape があっても使わない（無い環境の再現用）
# ------------------------------------------------------------------
DEBUG    = 0
STATIC   = 0
XFT      = 0
NO_SHAPE = 0

# SPEC §9.2 の最適化方針。-fno-stack-protector は安全性のため使わない。
CFLAGS_OPT_0 = -Os -fno-plt -fno-asynchronous-unwind-tables \
               -ffunction-sections -fdata-sections
CFLAGS_OPT_1 = -O0 -g3 -fno-omit-frame-pointer
CFLAGS_OPT   = $(CFLAGS_OPT_$(DEBUG))
CFLAGS_WARN= -Wall -Wextra -Wpedantic -Wno-unused-parameter \
             -Wshadow -Wstrict-prototypes -Wmissing-prototypes
LDFLAGS_GC_0 = -Wl,--gc-sections
LDFLAGS_GC_1 =
LDFLAGS_STATIC_0 =
LDFLAGS_STATIC_1 = -static
PKG_STATIC_0 =
PKG_STATIC_1 = --static

# 必須依存 (SPEC §1.4)。pkg-config が無い／見つからないときは素の -l に落とす
# （以前は pkg-config の失敗で全フラグが空になり、原因の分かりにくい
#   リンクエラーになっていた）。
REQ_PKGS   = xcb xcb-randr xcb-sync xcb-keysyms
PKG_CFLAGS != $(PKG_CONFIG) --cflags $(REQ_PKGS) 2>/dev/null || true
PKG_LIBS   != $(PKG_CONFIG) $(PKG_STATIC_$(STATIC)) --libs $(REQ_PKGS) 2>/dev/null || echo "-lxcb-randr -lxcb-sync -lxcb-keysyms -lxcb"

# xcb-shape は任意 (SPEC §5.3)。無ければ shape.c が矩形扱いに縮退する。
# ヘッダだけあってライブラリが無い環境でリンクに失敗しないよう、
# pkg-config で見つからなければ WM_NO_SHAPE_HEADER でヘッダごと使わない。
SHAPE_AUTO_CFLAGS != $(PKG_CONFIG) --exists xcb-shape 2>/dev/null && $(PKG_CONFIG) --cflags xcb-shape || echo "-DWM_NO_SHAPE_HEADER"
SHAPE_AUTO_LIBS   != $(PKG_CONFIG) --exists xcb-shape 2>/dev/null && $(PKG_CONFIG) $(PKG_STATIC_$(STATIC)) --libs xcb-shape || true
SHAPE_CFLAGS_0 = $(SHAPE_AUTO_CFLAGS)
SHAPE_CFLAGS_1 = -DWM_NO_SHAPE_HEADER
SHAPE_LIBS_0   = $(SHAPE_AUTO_LIBS)
SHAPE_LIBS_1   =

CFLAGS  = -std=c99 -D_POSIX_C_SOURCE=200809L $(CFLAGS_OPT) $(CFLAGS_WARN) \
          $(PKG_CFLAGS) $(SHAPE_CFLAGS_$(NO_SHAPE)) $(EXTRA_CFLAGS)
LDFLAGS = $(LDFLAGS_GC_$(DEBUG)) $(LDFLAGS_STATIC_$(STATIC)) $(EXTRA_LDFLAGS)
LIBS    = $(SHAPE_LIBS_$(NO_SHAPE)) $(PKG_LIBS)

OBJS = src/main.o src/wm.o src/event.o src/client.o src/icccm.o \
       src/ewmh.o src/atoms.o src/stack.o src/focus.o src/move.o \
       src/layout.o src/input.o src/config.o src/util.o src/type.o \
       src/theme.o src/draw.o src/font.o src/deco.o src/cursor.o src/menu.o \
       src/sync.o src/motif.o src/icon.o src/ping.o src/shape.o \
       src/taskbar.o src/tray.o src/switcher.o src/ctext.o src/jis0208.o

BIN = w98wm

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJS): src/w98wm.h src/atoms.h src/brand.h src/compat.h src/draw.h .buildcfg

#
# .buildcfg: 使ったフラグの控え。中身が変わったときだけ更新するので、
# `make` の後に `make DEBUG=1` とすると全部ビルドし直される
# （以前は .o が新しいので何も起きず、同じバイナリのままだった）。
# 指定の検査もここで行う。未知の値や未実装のプロファイルは黙って
# 無視せずに止める。
#
.buildcfg: FORCE
	@if [ "$(XFT)" != 0 ]; then \
		echo "make: XFT=1 (xft プロファイル, SPEC §1.4) はまだ実装されていません。" >&2; \
		echo "      core ビルドの日本語表示は SPEC §4.5.5 / docs/MANUAL-TEST.md を参照。" >&2; \
		exit 1; \
	fi
	@for v in "DEBUG=$(DEBUG)" "STATIC=$(STATIC)" "NO_SHAPE=$(NO_SHAPE)"; do \
		case "$$v" in *=0|*=1) ;; *) echo "make: $$v は 0 か 1 で指定してください" >&2; exit 1 ;; esac; \
	done
	@echo '$(CC) $(CFLAGS) | $(LDFLAGS) $(LIBS)' > .buildcfg.new
	@if cmp -s .buildcfg.new .buildcfg; then rm -f .buildcfg.new; else mv .buildcfg.new .buildcfg; fi

FORCE:

clean:
	rm -f $(BIN) $(OBJS) .buildcfg .buildcfg.new

install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

test: $(BIN)
	sh tests/run-tests.sh

check-brand:
	sh tools/check-brand.sh

memcheck: $(BIN)
	sh tools/memcheck.sh

.PHONY: all clean install test check-brand memcheck FORCE
