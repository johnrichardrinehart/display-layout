CC ?= cc
PREFIX ?= /usr/local
VERSION ?= development

CPPFLAGS += -Isrc -DDISPLAY_LAYOUT_VERSION='"$(VERSION)"'
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
UI_LIBS = -lX11 -lXrender

SOURCES = src/main.c src/backend.c src/backend_niri.c src/config.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean check install

all: display-layout

display-layout: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(UI_LIBS) -lm

src/main.o: CFLAGS += $(UI_CFLAGS)

tests/config_test: tests/config_test.c src/config.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/config_test.c src/config.c

tests/backend_niri_test: tests/backend_niri_test.c src/backend_niri.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/backend_niri_test.c src/backend_niri.c

check: tests/config_test tests/backend_niri_test
	./tests/config_test
	./tests/backend_niri_test

install: display-layout
	install -Dm755 display-layout $(DESTDIR)$(PREFIX)/bin/display-layout
	install -Dm644 config.example.ini $(DESTDIR)$(PREFIX)/share/doc/display-layout/config.example.ini
	install -Dm644 display-layout-editor.desktop $(DESTDIR)$(PREFIX)/share/applications/display-layout-editor.desktop
	install -Dm644 assets/display-layout-editor.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/display-layout-editor.svg
	install -Dm644 assets/DejaVuSansMono.ttf $(DESTDIR)$(PREFIX)/share/display-layout/DejaVuSansMono.ttf
	install -Dm644 assets/DejaVu-LICENSE.txt $(DESTDIR)$(PREFIX)/share/doc/display-layout/DejaVu-LICENSE.txt

clean:
	rm -f display-layout $(OBJECTS) tests/config_test tests/backend_niri_test
