PREFIX = /usr/local
LD = ld
CC = cc
PKG_CONFIG = pkg-config
INSTALL = install
CFLAGS = -g -O2 -Wall -Wextra
LDFLAGS =
LIBS =
VLC_PLUGIN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vlc-plugin)
VLC_PLUGIN_LIBS := $(shell $(PKG_CONFIG) --libs vlc-plugin)

libdir = $(PREFIX)/lib
plugindir = $(libdir)/vlc/plugins

override CC += -std=gnu99
override CPPFLAGS += -DPIC -I. -Isrc
override CFLAGS += -fPIC
override LDFLAGS += -Wl,-no-undefined,-z,defs

#override CPPFLAGS += -DMODULE_STRING=\"foo\"
override CFLAGS += $(VLC_PLUGIN_CFLAGS)
override LIBS += $(VLC_PLUGIN_LIBS)

TARGETS = vlc_listenbrainz_plugin.so

all: vlc_listenbrainz_plugin.so

install: all
		mkdir -p -- $(DESTDIR)$(plugindir)/misc
		$(INSTALL) --mode 0755 vlc_listenbrainz_plugin.so $(DESTDIR)$(plugindir)/misc

install-strip:
		$(MAKE) install INSTALL="$(INSTALL) -s"

uninstall:
		rm -f $(plugindir)/misc/vlc_listenbrainz_plugin.so

clean:
		rm -f vlc_listenbrainz_plugin.so *.o

mostlyclean: clean

SOURCES = listenbrainz.c
$(SOURCES:%.c=%.o): %: listenbrainz.c

vlc_listenbrainz_plugin.so: $(SOURCES:%.c=%.o)
		$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

.PHONY: all install install-strip uninstall clean mostlyclean