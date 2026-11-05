# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Spotify plugin for mediamaster.
#
#   make              build/spotify.so
#   make test         load it into the pinned mediamaster and check it
#   make install      copy it into mediamaster's plugin directory
#
# The plugin is written against one mediamaster: the submodule in
# ./mediamaster, pinned to a commit. `make` fetches it when missing.
#
#   make MM=~/src/mediamaster        build against another checkout
#   git -C mediamaster checkout <rev> && git add mediamaster
#                                     move the pin

CC     ?= cc
MM     ?= mediamaster
PREFIX ?= /usr/local
PLUGIN_DIR ?= $(PREFIX)/lib/mediamaster/plugins

SDK := $(MM)/include/mm_plugin.h

CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes \
          -Wmissing-prototypes -D_POSIX_C_SOURCE=200809L \
          -fPIC -fvisibility=hidden -I$(MM)/include

.PHONY: all test install clean
all: build/spotify.so

build/spotify.so: src/spotify.c $(SDK)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) src/spotify.c -shared $(LDFLAGS) -o $@

$(SDK):
	git submodule update --init $(MM)

test: all
	@$(MAKE) --no-print-directory -C $(MM) build/mediamaster build/mmctl
	@sh tests/load.sh $(MM)/build build

install: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 644 build/spotify.so $(DESTDIR)$(PLUGIN_DIR)

clean:
	rm -rf build
