# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Spotify plugin for mediamaster.
#
#   make              build/spotify.so
#   make test         load it into the pinned mediamaster and check it
#   make install      copy it into mediamaster's plugin directory
#
# Two submodules, each pinned to a commit and fetched by `make` when
# missing:
#   mediamaster/   the host: include/mm_plugin.h, and the daemon for tests
#   librespot-c/   Spotify protocol library, compiled into spotify.so
#
#   make MM=~/src/mediamaster        build against another host checkout
#   git -C <sub> checkout <rev> && git add <sub>
#                                     move a pin
#
# Needs: libevent json-c libgcrypt libcurl protobuf-c (with headers).

CC     ?= cc
MM     ?= mediamaster
LRC    := librespot-c
PREFIX ?= /usr/local
PLUGIN_DIR ?= $(PREFIX)/lib/mediamaster/plugins

# Fetch a submodule before anything below lists its files.
fetch = $(if $(wildcard $(1)/$(2)),,$(shell git submodule update --init $(1) >&2))
$(call fetch,$(LRC),librespot-c.h)
ifeq ($(MM),mediamaster)
$(call fetch,$(MM),include/mm_plugin.h)
endif

PKGS   := libevent json-c libgcrypt libcurl libprotobuf-c
CFLAGS ?= -O2 -g

# The plugin: strict C99 against the host ABI and librespot-c's header.
PLUG_CFLAGS := $(CFLAGS) -std=c99 -pedantic -Wall -Wextra -Wshadow \
               -Wstrict-prototypes -Wmissing-prototypes -D_POSIX_C_SOURCE=200809L \
               -fPIC -fvisibility=hidden -I$(MM)/include -I$(LRC)

# librespot-c: its own dialect, its warnings are upstream's business.
# The defines are what its configure would find on Linux/glibc; without
# HAVE_SYS_UTSNAME_H Spotify refuses the client-token request.
LRC_CFLAGS := $(CFLAGS) -std=gnu11 -w -fPIC -fvisibility=hidden -pthread \
              -D_GNU_SOURCE -DHAVE_SYS_UTSNAME_H -DHAVE_ENDIAN_H -DHAVE_PIPE2 \
              -I$(LRC) -I$(LRC)/src $(shell pkg-config --cflags $(PKGS))

PLUG_OBJ := build/spotify.o build/lrc.o
LRC_OBJ  := $(patsubst %.c,build/%.o,$(wildcard $(LRC)/src/*.c $(LRC)/src/proto/*.c $(LRC)/src/shannon/*.c))

.PHONY: all test install clean
all: build/spotify.so

build/spotify.so: $(PLUG_OBJ) $(LRC_OBJ)
	@echo "link $@ (plugin + $(words $(LRC_OBJ)) librespot-c objects)"
	@$(CC) -shared $(LDFLAGS) $^ $(shell pkg-config --libs $(PKGS)) -pthread -o $@

build/%.o: src/%.c src/*.h $(MM)/include/mm_plugin.h Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -c $< -o $@

build/$(LRC)/%.o: $(LRC)/%.c Makefile
	@mkdir -p $(@D)
	@$(CC) $(LRC_CFLAGS) -c $< -o $@

test: all
	@$(MAKE) --no-print-directory -C $(MM) build/mediamaster build/mmctl
	@sh tests/load.sh $(MM)/build build

install: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 644 build/spotify.so $(DESTDIR)$(PLUGIN_DIR)

clean:
	rm -rf build
