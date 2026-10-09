PLUGIN = quicksettings
TARGET = lib$(PLUGIN).so

CC ?= gcc
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr
LIBDIR ?= $(shell $(PKG_CONFIG) --variable=libdir libxfce4panel-2.0)
PANEL_PLUGIN_DIR ?= $(LIBDIR)/xfce4/panel/plugins
PANEL_DESKTOP_DIR ?= $(PREFIX)/share/xfce4/panel/plugins

CFLAGS += -Wall -Wextra -fPIC -O2
CFLAGS += $(shell $(PKG_CONFIG) --cflags gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0)

SRC = src/qs-plugin.c src/qs-util.c src/qs-ui.c src/qs-audio.c \
      src/qs-display.c src/qs-net.c src/qs-power.c src/qs-session.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $(OBJ) $(LDLIBS)

install: $(TARGET)
	install -d "$(DESTDIR)$(PANEL_PLUGIN_DIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(PANEL_PLUGIN_DIR)/$(TARGET)"
	install -d "$(DESTDIR)$(PANEL_DESKTOP_DIR)"
	install -m 644 "data/quicksettings.desktop" "$(DESTDIR)$(PANEL_DESKTOP_DIR)/quicksettings.desktop"

uninstall:
	rm -f "$(DESTDIR)$(PANEL_PLUGIN_DIR)/$(TARGET)"
	rm -f "$(DESTDIR)$(PANEL_DESKTOP_DIR)/quicksettings.desktop"

clean:
	rm -f "$(TARGET)" $(OBJ)
