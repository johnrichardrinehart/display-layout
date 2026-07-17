CC ?= cc
PREFIX ?= /usr/local
VERSION ?= development

WAYLAND_SCANNER ?= wayland-scanner
WLR_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wlr-protocols)
WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
LAYER_SHELL_XML = $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
GENERATED_DIR = generated
LAYER_SHELL_HEADER = $(GENERATED_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_SHELL_CODE = $(GENERATED_DIR)/wlr-layer-shell-unstable-v1-protocol.c
XDG_SHELL_CODE = $(GENERATED_DIR)/xdg-shell-protocol.c

WAYLAND_CFLAGS = $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS = $(shell pkg-config --libs wayland-client)

CPPFLAGS += -Isrc -I$(GENERATED_DIR) $(WAYLAND_CFLAGS) -DDISPLAY_LAYOUT_VERSION='"$(VERSION)"'
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
UI_LIBS = -lX11 -lXrender $(WAYLAND_LIBS)

BACKEND_SOURCES = src/backend.c src/backend_common.c src/jsmn_impl.c src/backend_niri.c \
                  src/backend_sway.c src/backend_hyprland.c src/backend_wlr.c \
                  src/backend_kscreen.c src/backend_gnome.c
SOURCES = src/main.c $(BACKEND_SOURCES) src/config.c src/identifier_wayland.c \
          src/stb_truetype_impl.c $(LAYER_SHELL_CODE) $(XDG_SHELL_CODE)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean check install

all: display-layout

display-layout: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(UI_LIBS) -lm

$(LAYER_SHELL_HEADER): $(LAYER_SHELL_XML)
	mkdir -p $(GENERATED_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(LAYER_SHELL_CODE): $(LAYER_SHELL_XML) $(LAYER_SHELL_HEADER)
	mkdir -p $(GENERATED_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(XDG_SHELL_CODE): $(XDG_SHELL_XML)
	mkdir -p $(GENERATED_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

src/main.o: CFLAGS += $(UI_CFLAGS)
src/identifier_wayland.o: $(LAYER_SHELL_HEADER)
$(LAYER_SHELL_CODE:.c=.o): $(LAYER_SHELL_HEADER)

tests/config_test: tests/config_test.c src/config.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/config_test.c src/config.c

tests/backend_test: tests/backend_test.c $(BACKEND_SOURCES) \
                    src/identifier_wayland.c src/stb_truetype_impl.c \
                    $(LAYER_SHELL_CODE) $(XDG_SHELL_CODE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(WAYLAND_LIBS) -lm

check: tests/config_test tests/backend_test
	./tests/config_test
	./tests/backend_test

install: display-layout
	install -Dm755 display-layout $(DESTDIR)$(PREFIX)/bin/display-layout
	install -Dm644 config.example.ini $(DESTDIR)$(PREFIX)/share/doc/display-layout/config.example.ini
	install -Dm644 display-layout-editor.desktop $(DESTDIR)$(PREFIX)/share/applications/display-layout-editor.desktop
	install -Dm644 assets/display-layout-editor.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/display-layout-editor.svg
	install -Dm644 assets/DejaVuSansMono.ttf $(DESTDIR)$(PREFIX)/share/display-layout/DejaVuSansMono.ttf
	install -Dm644 assets/DejaVu-LICENSE.txt $(DESTDIR)$(PREFIX)/share/doc/display-layout/DejaVu-LICENSE.txt

clean:
	rm -f display-layout $(OBJECTS) tests/config_test tests/backend_test tests/backend_niri_test
	rm -rf $(GENERATED_DIR)
