# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Spotify plugin for mediamaster.
#
#   make              build/spotify.so
#   make test         unit tests, then load it into the installed mediamaster
#   make install      into mediamaster's plugin directory
#
# Builds against the installed mediamaster SDK (pkg-config mediamaster:
# mm_plugin.h and the plugin directory), so install mediamaster first. The
# daemon checks the ABI the plugin was built for when it loads it; after a
# mediamaster update that changes the ABI, rebuild and reinstall.
#
# One submodule, pinned to a commit and fetched by `make` when missing:
#   librespot-c/   Spotify protocol library, compiled into spotify.so
#   git -C librespot-c checkout <rev> && git add librespot-c    moves the pin
#
# Needs: libevent json-c libgcrypt libcurl protobuf-c (with headers);
# ffmpeg with Vorbis to play, and to test.

CC  ?= cc
LRC := librespot-c

MM_CFLAGS  := $(shell pkg-config --cflags mediamaster 2>/dev/null)
PLUGIN_DIR ?= $(shell pkg-config --variable=plugindir mediamaster 2>/dev/null)
ifeq ($(MM_CFLAGS),)
  $(error mediamaster is not installed: pkg-config cannot find mediamaster.pc. \
          Run make install in the mediamaster repository first, or set PKG_CONFIG_PATH)
endif
MM_HEADER := $(patsubst -I%,%,$(filter -I%,$(MM_CFLAGS)))/mm_plugin.h

# Fetch the submodule before anything below lists its files.
fetch = $(if $(wildcard $(1)/$(2)),,$(shell git submodule update --init $(1) >&2))
$(call fetch,$(LRC),librespot-c.h)

PKGS   := libevent json-c libgcrypt libcurl libprotobuf-c
CFLAGS ?= -O2 -g

# The plugin: strict C99 against the host ABI and librespot-c's header.
PLUG_CFLAGS := $(CFLAGS) -std=c99 -pedantic -Wall -Wextra -Wshadow \
               -Wstrict-prototypes -Wmissing-prototypes -D_POSIX_C_SOURCE=200809L \
               -fPIC -fvisibility=hidden $(MM_CFLAGS) -I$(LRC) -I$(LRC)/src \
               $(shell pkg-config --cflags json-c libcurl libprotobuf-c)

# librespot-c: its own dialect, its warnings are upstream's business.
# The defines are what its configure would find on Linux/glibc; without
# HAVE_SYS_UTSNAME_H Spotify refuses the client-token request.
LRC_CFLAGS := $(CFLAGS) -std=gnu11 -w -fPIC -fvisibility=hidden -pthread \
              -D_GNU_SOURCE -DHAVE_SYS_UTSNAME_H -DHAVE_ENDIAN_H -DHAVE_PIPE2 \
              -I$(LRC) -I$(LRC)/src $(shell pkg-config --cflags $(PKGS))

# src/lrc/: the few lines that read librespot-c's internals, built in its
# dialect against its internal header (see src/lrc/tokens.c).
LRC_INT_CFLAGS := $(filter-out -w,$(LRC_CFLAGS)) -Wall -Wextra -Wno-unused-parameter

PLUG_OBJ := $(patsubst src/%.c,build/%.o,$(wildcard src/*.c src/lrc/*.c))
LRC_OBJ  := $(patsubst %.c,build/%.o,$(wildcard $(LRC)/src/*.c $(LRC)/src/proto/*.c $(LRC)/src/shannon/*.c))

.PHONY: all test install uninstall clean
all: build/spotify.so

build/spotify.so: $(PLUG_OBJ) $(LRC_OBJ)
	@echo "link $@ (plugin + $(words $(LRC_OBJ)) librespot-c objects)"
	@$(CC) -shared $(LDFLAGS) $^ $(shell pkg-config --libs $(PKGS)) -pthread -o $@

build/%.o: src/%.c src/*.h $(MM_HEADER) Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -c $< -o $@

build/lrc/%.o: src/lrc/%.c src/*.h Makefile
	@mkdir -p $(@D)
	$(CC) $(LRC_INT_CFLAGS) -c $< -o $@

build/$(LRC)/%.o: $(LRC)/%.c Makefile
	@mkdir -p $(@D)
	@$(CC) $(LRC_CFLAGS) -c $< -o $@

build/tests/creds: tests/creds_test.c src/creds.c src/creds.h Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -Isrc tests/creds_test.c src/creds.c $(shell pkg-config --libs json-c) -o $@

# 20 s of 44.1 kHz stereo Vorbis in ~100 ms pages, like a Spotify file.
# Noise, so a seek that lands a few samples off cannot pass for exact.
OGG_MS := 20000
build/tests/noise.ogg:
	@mkdir -p $(@D)
	@ffmpeg -nostdin -loglevel error -y -f lavfi -i anoisesrc=c=pink:seed=7:a=0.3:r=44100:d=20 \
	        -ac 2 -c:a libvorbis -q:a 8 -page_duration 100000 $@

build/tests/ogg: tests/ogg_test.c src/ogg.c src/ogg.h Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -Isrc tests/ogg_test.c src/ogg.c -o $@

build/tests/track: tests/track_test.c src/track.c src/ogg.c src/track.h src/ogg.h Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -Isrc tests/track_test.c src/track.c src/ogg.c -pthread -o $@

build/tests/meta: tests/meta_test.c src/meta.c src/spclient.c src/meta.h src/spclient.h Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -Isrc tests/meta_test.c src/meta.c src/spclient.c $(LRC)/src/proto/metadata.pb-c.c \
	      $(shell pkg-config --libs libcurl libprotobuf-c) -o $@

PROTO := $(addprefix $(LRC)/src/proto/,metadata.pb-c.c extended_metadata.pb-c.c \
           entity_extension_data.pb-c.c extension_kind.pb-c.c google_any.pb-c.c)

build/tests/library: tests/library_test.c src/library.c src/pb.c src/spclient.c src/*.h Makefile
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -Isrc tests/library_test.c src/library.c src/pb.c src/spclient.c $(PROTO) \
	      $(shell pkg-config --libs json-c libcurl libprotobuf-c) -o $@

test: all build/tests/creds build/tests/meta build/tests/library build/tests/ogg build/tests/track build/tests/noise.ogg
	@build/tests/creds
	@build/tests/meta
	@build/tests/library
	@build/tests/ogg build/tests/noise.ogg $(OGG_MS)
	@build/tests/track ffmpeg build/tests/noise.ogg $(OGG_MS)
	@if command -v mediamaster >/dev/null && command -v mmctl >/dev/null; then sh tests/load.sh build; \
	 else echo "load: skipped (mediamaster is not installed, or not in PATH)"; fi

install: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 644 build/spotify.so $(DESTDIR)$(PLUGIN_DIR)

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_DIR)/spotify.so

clean:
	rm -rf build
