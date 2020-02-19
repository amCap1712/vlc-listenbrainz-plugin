PREFIX = /usr/local
LD = ld
CC = cc
PKG_CONFIG = pkg-config
INSTALL = install
CFLAGS = -g -O2 -Wall -Wextra
LDFLAGS = -pthread
LIBS =
VLC_PLUGIN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vlc-plugin)
VLC_PLUGIN_LIBS := $(shell $(PKG_CONFIG) --libs vlc-plugin)
VLC_PLUGIN_DIR := $(shell $(PKG_CONFIG) --variable=pluginsdir vlc-plugin)

plugindir = $(VLC_PLUGIN_DIR)/misc

SOURCES = listenbrainz.c
SOURCES_DIR = vlc-3.0

override CC += -std=gnu11
override CPPFLAGS += -DPIC -I. -Isrc
override CFLAGS += -fPIC

override CPPFLAGS += -DMODULE_STRING=\"listenbrainz\"
override CFLAGS += $(VLC_PLUGIN_CFLAGS)
override LIBS += $(VLC_PLUGIN_LIBS)

ifeq ($(OS),Windows_NT)
  SUFFIX := dll
  override LDFLAGS += -Wl,-no-undefined
else
  SYSTEM_NAME=$(shell uname -s)
  ifeq ($(SYSTEM_NAME),Linux)
    SUFFIX := so
    override LDFLAGS += -Wl,-no-undefined
  else 
    ifeq ($(SYSTEM_NAME),Darwin)
      SUFFIX := dylib
      override LDFLAGS += -Wl
    endif
  endif
endif

TARGETS = liblistenbrainz_plugin.$(SUFFIX)

all: liblistenbrainz_plugin.$(SUFFIX)

install: all
		mkdir -p -- $(DESTDIR)$(plugindir)
		$(INSTALL) --mode 0755 liblistenbrainz_plugin.$(SUFFIX) $(DESTDIR)$(plugindir)

install-strip:
		$(MAKE) install INSTALL="$(INSTALL) -s"

uninstall:
		rm -f $(plugindir)/liblistenbrainz_plugin.$(SUFFIX)

clean:
		rm -rf liblistenbrainz_plugin.$(SUFFIX) **/*.o

mostlyclean: clean

$(SOURCES:%.c=$(SOURCES_DIR)/%.o): %: $(SOURCES_DIR)/listenbrainz.c

liblistenbrainz_plugin.$(SUFFIX): $(SOURCES:%.c=$(SOURCES_DIR)/%.o)
		$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

.PHONY: all install install-strip uninstall clean mostlyclean