# SDLop -- a lean reimplementation of the SDL3 API (windowing, input, timing).
#
# No cmake, no configure: `make` builds everything it can with what is installed.
# Each optional backend is detected with pkg-config and enabled with a -D flag, so
# a machine without Wayland (or without X11, or without EGL) still gets a complete
# build of everything else.
#
#   make                 build build/libSDLop.{a,so}
#   make examples        build the example programs
#   make check           build and run the test suite
#   make bench           build the benchmark (needs real SDL3, see bench/README.md)
#   make install         install to $(PREFIX)
#   make clean
#
# Options:
#   CC=clang OPT="-O3 -march=native" DEBUG=1 WAYLAND=0 X11=0 EGL=0 OFFSCREEN=1

CC      ?= cc
AR      ?= ar
BUILD   ?= build
GEN     := src/generated
PREFIX  ?= /usr/local
VER     := 3.2.10-sdlop

CFLAGS   ?= -O2 -g
CFLAGS_BASE := $(CFLAGS) -std=gnu11
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fPIC -MMD -MP -Iinclude -I$(GEN) -Isrc
LDFLAGS ?=

ifdef DEBUG
CFLAGS += -O0 -DSDLOP_DEBUG
endif

# ---------------------------------------------------------------- dependencies
PKG_CONFIG ?= pkg-config
HAVE_PKGCONFIG := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && echo yes)

ifeq ($(HAVE_PKGCONFIG),yes)
WAYLAND  ?= $(shell $(PKG_CONFIG) --exists wayland-client wayland-egl && echo 1 || echo 0)
XKB      ?= $(shell $(PKG_CONFIG) --exists xkbcommon && echo 1 || echo 0)
EGL      ?= $(shell $(PKG_CONFIG) --exists egl && echo 1 || echo 0)
X11      ?= $(shell $(PKG_CONFIG) --exists x11 && echo 1 || echo 0)
else
WAYLAND ?= 0
XKB     ?= 0
EGL     ?= 0
X11     ?= 0
endif

ifneq ($(wildcard src/video/SDL_x11.c),)
else
X11 := 0
endif

LIBS := -lpthread -ldl -lm

ifeq ($(WAYLAND),1)
CFLAGS += -DSDLOP_HAVE_WAYLAND $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl 2>/dev/null)
LIBS   += $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl)
endif
ifeq ($(XKB),1)
CFLAGS += -DSDLOP_HAVE_XKBCOMMON
LIBS   += $(shell $(PKG_CONFIG) --libs xkbcommon)
endif
ifeq ($(EGL),1)
CFLAGS += -DSDLOP_HAVE_EGL
# No -lEGL: src/gl/SDL_egl.c resolves every entry point with dlsym() from the
# library it dlopen()s on demand, exactly like SDL's loader. Listing libEGL here
# would put it in DT_NEEDED and make a GL-less machine unable to even start.
LIBS   += $(shell $(PKG_CONFIG) --libs wayland-egl)
endif
ifeq ($(X11),1)
# X11 needs libXext as well: XShape (window hit test) and XShm (shared-memory
# presentation, one copy less per frame than XPutImage) both live there.
CFLAGS += -DSDLOP_HAVE_X11 -DSDLOP_HAVE_XSHAPE $(shell $(PKG_CONFIG) --cflags x11 xext 2>/dev/null)
LIBS   += $(shell $(PKG_CONFIG) --libs x11 xext 2>/dev/null || echo -lX11 -lXext)
# Each optional piece is detected on its own, so a machine with only libX11
# still gets a window, a keyboard and a mouse:
#   XShm          every X server (it is an X extension, checked at runtime too)
#   XInput2       unaccelerated relative motion for relative mouse mode
#   XRandR        one SDL display per output, hotplug, mode changes
#   xkbcommon-x11 the server's keymap, compiled by the same code Wayland uses
XSHM     ?= 1
XINPUT2  ?= $(shell $(PKG_CONFIG) --exists xi && echo 1 || echo 0)
XRANDR   ?= $(shell $(PKG_CONFIG) --exists xrandr && echo 1 || echo 0)
X11XKB   ?= $(shell $(PKG_CONFIG) --exists xkbcommon-x11 && echo 1 || echo 0)
ifeq ($(XSHM),1)
CFLAGS += -DSDLOP_HAVE_XSHM
endif
ifeq ($(XINPUT2),1)
CFLAGS += -DSDLOP_HAVE_XINPUT2 $(shell $(PKG_CONFIG) --cflags xi)
LIBS   += $(shell $(PKG_CONFIG) --libs xi)
endif
ifeq ($(XRANDR),1)
CFLAGS += -DSDLOP_HAVE_XRANDR $(shell $(PKG_CONFIG) --cflags xrandr)
LIBS   += $(shell $(PKG_CONFIG) --libs xrandr)
endif
ifeq ($(X11XKB),1)
# xkbcommon-x11 reads the keymap over XCB, so it needs the XCB connection Xlib
# is already using (libX11-xcb). Without it the X11 driver falls back to the
# local XKB configuration, which is the same fallback a headless session uses.
X11XCB ?= $(shell $(PKG_CONFIG) --exists x11-xcb && echo 1 || echo 0)
CFLAGS += -DSDLOP_HAVE_XKBCOMMON_X11
LIBS   += $(shell $(PKG_CONFIG) --libs xkbcommon-x11)
ifeq ($(X11XCB),1)
CFLAGS += -DSDLOP_HAVE_X11_XCB $(shell $(PKG_CONFIG) --cflags x11-xcb)
LIBS   += $(shell $(PKG_CONFIG) --libs x11-xcb)
endif
endif
endif

# ---------------------------------------------------------------- sources
SRCS := $(wildcard src/core/*.c) $(wildcard src/timer/*.c) $(wildcard src/events/*.c) \
        $(wildcard src/video/*.c) $(wildcard src/input/*.c) $(wildcard src/main/*.c) \
        $(wildcard src/gl/*.c) $(wildcard src/audio/*.c)

# The detected feature set is written to a stamp file that every object depends
# on, so installing a dependency (or setting WAYLAND=0) and re-running make
# rebuilds instead of silently keeping objects from the previous configuration.
FEATURE_STAMP := $(BUILD)/features
FEATURES := $(CFLAGS_BASE)$(if $(WAYLAND),wayland)$(if $(XKB),xkb)$(if $(EGL),egl)$(if $(X11),x11)$(if $(XSHM),shm)$(if $(XINPUT2),xi2)$(if $(XRANDR),xrandr)$(if $(X11XKB),x11xkb)$(if $(DEBUG),debug)

ifeq ($(WAYLAND),1)
# cursor-shape-v1 refers to the tablet-v2 interfaces, so tablet-v2 is generated too
WAYLAND_PROTOCOL_C := $(GEN)/xdg-shell-protocol.c $(GEN)/viewporter-protocol.c \
                      $(GEN)/cursor-shape-v1-protocol.c $(GEN)/tablet-v2-protocol.c \
                      $(GEN)/xdg-output-protocol.c $(GEN)/input-timestamps-protocol.c \
                      $(GEN)/relative-pointer-protocol.c $(GEN)/pointer-constraints-protocol.c
SRCS += $(WAYLAND_PROTOCOL_C)
endif

# Dev-only protocol for tools/wl_inject.c, which drives the seat of a headless
# compositor (wlroots' virtual pointer). It is deliberately *not* part of the
# library: nothing under src/ references it.
WLR_PROTOCOL_XML  := tools/reference/wlr-virtual-pointer-unstable-v1.xml
WLR_PROTOCOL_C    := $(GEN)/wlr-virtual-pointer-protocol.c
WLR_PROTOCOL_H    := $(GEN)/wlr-virtual-pointer-client-protocol.h

OBJS := $(patsubst src/%,$(BUILD)/obj/%,$(SRCS:.c=.o))

# headers generated from the SDL3 headers and from the data tables
GEN_HEADERS := $(GEN)/sdlop_keynames.h $(GEN)/sdlop_pixelformats.h $(GEN)/sdlop_evdev.h

.PHONY: all clean install examples check wayland-check x11-check bench bench-run tools inject regen FORCE
all: $(BUILD)/libSDLop.a $(BUILD)/libSDLop.so

$(FEATURE_STAMP): FORCE
	@mkdir -p $(BUILD)
	@echo '$(FEATURES)' | cmp -s - $@ || echo '$(FEATURES)' > $@

FORCE:

$(BUILD)/obj/%.o: src/%.c $(GEN_HEADERS) $(FEATURE_STAMP) | $(BUILD)/obj
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Header dependencies, generated by -MMD. Without these an edit to an internal
# header (a new driver entry point, say) leaves half the objects stale, and the
# resulting crash has nothing to do with the code you just wrote.
-include $(OBJS:.o=.d)

# Written to a temporary name and moved into place: linking a half-written
# archive is a miserable way to spend an afternoon.
$(BUILD)/libSDLop.a: $(OBJS)
	$(AR) rcs $@.tmp $(OBJS)
	mv $@.tmp $@
	@echo "  [ar] $@"

$(BUILD)/libSDLop.so: $(OBJS)
	$(CC) -shared -o $@ $(OBJS) $(LDFLAGS) $(LIBS)
	@echo "  [so] $@ (SDLop $(VER))"

$(BUILD)/obj:
	mkdir -p $@

$(BUILD)/tools:
	mkdir -p $@

# wl_inject: scripts a compositor's seat through wlroots' virtual pointer, so a
# headless sway can be driven like a real desktop (see tools/wl_inject.c).
inject: $(BUILD)/tools/wl_inject

# Grouped target (GNU make 4.3+): one recipe writes both files, so neither can
# go stale on its own.
$(WLR_PROTOCOL_C) $(WLR_PROTOCOL_H) &: $(WLR_PROTOCOL_XML)
	@mkdir -p $(GEN)
	wayland-scanner client-header $(WLR_PROTOCOL_XML) $(WLR_PROTOCOL_H)
	wayland-scanner private-code  $(WLR_PROTOCOL_XML) $(WLR_PROTOCOL_C)

WL_INJECT_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client xkbcommon 2>/dev/null || echo -lwayland-client -lxkbcommon)

$(BUILD)/tools/wl_inject: tools/wl_inject.c $(WLR_PROTOCOL_C) $(WLR_PROTOCOL_H) | $(BUILD)/tools
	$(CC) $(CFLAGS_BASE) -Wall -Wextra -Wno-unused-parameter -I$(GEN) $^ $(WL_INJECT_LIBS) -lm -o $@
	@echo "  [exe] $@"

# ---------------------------------------------------------------- generated code
# `make regen` rewrites every generated file from its upstream source, so the
# repository's copies are exactly what the generators produce.
regen: tools
tools: inject
	python3 tools/sdlop.py
	python3 tools/gen_keynames.py
	python3 tools/gen_pixelformats.py
	python3 tools/gen_evdev.py
ifeq ($(WAYLAND),1)
	wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml $(GEN)/xdg-shell-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml $(GEN)/xdg-shell-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/stable/viewporter/viewporter.xml $(GEN)/viewporter-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/stable/viewporter/viewporter.xml $(GEN)/viewporter-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml $(GEN)/cursor-shape-v1-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml $(GEN)/cursor-shape-v1-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/stable/tablet/tablet-v2.xml $(GEN)/tablet-v2-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/stable/tablet/tablet-v2.xml $(GEN)/tablet-v2-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml $(GEN)/xdg-output-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml $(GEN)/xdg-output-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/unstable/input-timestamps/input-timestamps-unstable-v1.xml $(GEN)/input-timestamps-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/unstable/input-timestamps/input-timestamps-unstable-v1.xml $(GEN)/input-timestamps-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml $(GEN)/relative-pointer-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml $(GEN)/relative-pointer-protocol.c
	wayland-scanner client-header /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $(GEN)/pointer-constraints-client-protocol.h
	wayland-scanner private-code  /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $(GEN)/pointer-constraints-protocol.c
endif

# ---------------------------------------------------------------- programs
EXAMPLES := $(patsubst examples/%.c,$(BUILD)/examples/%,$(wildcard examples/*.c))
# `make check` runs these; wayland_input is a *client* for a real compositor, so
# it is built on demand and driven by tests/wayland_input.sh instead.
WAYLAND_CLIENTS := $(BUILD)/tests/wayland_input $(BUILD)/tests/wayland_display
# Driven against a running X server (tests/x11_input.sh): an X client, not a suite.
X11_CLIENTS := $(BUILD)/tests/x11_input
TESTS    := $(filter-out $(WAYLAND_CLIENTS) $(X11_CLIENTS),$(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/*.c)))
BENCHES  := $(patsubst bench/%.c,$(BUILD)/bench/%_compare,$(wildcard bench/*.c))

examples: $(EXAMPLES)
tests: $(TESTS)
bench: $(BENCHES)
	@if [ -z "$(BENCHES)" ]; then echo "  (no bench/ sources)"; fi

bench-run: bench all
	@$(BUILD)/bench/bench_compare $(BUILD)/libSDLop.so

$(BUILD)/examples/%: examples/%.c $(BUILD)/libSDLop.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(BUILD)/libSDLop.a $(LIBS) -o $@
	@echo "  [exe] $@"

$(BUILD)/tests/%: tests/%.c $(BUILD)/libSDLop.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(BUILD)/libSDLop.a $(LIBS) -o $@
	@echo "  [exe] $@"

# The benchmark links against the *system* SDL3 and dlopen()s SDLop's shared
# library, so both are measured in one process. It therefore does not use the
# SDLop include path or the static library. `make bench` builds it, `make bench`
# also has to find real SDL3: see bench/README.md.
BENCH_CFLAGS := $(CFLAGS_BASE) -Wall -Wextra -Wno-unused-parameter
$(BUILD)/bench/%_compare: bench/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BENCH_CFLAGS) $< -o $@ $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null || echo -lSDL3) -ldl -lpthread -lm
	@echo "  [exe] $@"

check: tests
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

# Driven against a running X server; see tests/x11_input.sh.
x11-check: tests $(X11_CLIENTS)
	@X11_INPUT_CLIENT=$(BUILD)/tests/x11_input sh tests/x11_input.sh

# Driven against a running compositor; see tests/wayland_input.sh.
wayland-check: tests inject $(WAYLAND_CLIENTS)
	@WAYLAND_INPUT_CLIENT=$(BUILD)/tests/wayland_input \
	 WAYLAND_INJECT=$(BUILD)/tools/wl_inject \
	 sh tests/wayland_input.sh
	@WAYLAND_DISPLAY_CLIENT=$(BUILD)/tests/wayland_display \
	 sh tests/wayland_display.sh

$(WAYLAND_CLIENTS) $(X11_CLIENTS): $(BUILD)/tests/%: tests/%.c $(BUILD)/libSDLop.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(BUILD)/libSDLop.a $(LIBS) -o $@
	@echo "  [exe] $@"

install: all
	install -d $(DESTDIR)$(PREFIX)/include/SDL3 $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 include/SDL3/*.h $(DESTDIR)$(PREFIX)/include/SDL3/
	install -m 644 $(BUILD)/libSDLop.a $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(BUILD)/libSDLop.so $(DESTDIR)$(PREFIX)/lib/
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@VER@|$(VER)|' support/sdl3-sdlop.pc.in > \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/sdl3-sdlop.pc

clean:
	rm -rf $(BUILD)
