# SPDX-License-Identifier: LGPL-2.1-or-later
# Throwaway measurement tool, spike branch only.
#   make -C spike && spike/spike ~/.cache/librespot/credentials.json spotify:track:<id>

LRC  := ../librespot-c
PKGS := libevent json-c libgcrypt libcurl libprotobuf-c

# Fetch the submodule before the source list below is expanded.
ifeq ($(wildcard $(LRC)/librespot-c.h),)
$(shell git -C .. submodule update --init librespot-c >&2)
endif
SRC  := spike.c $(wildcard $(LRC)/src/*.c $(LRC)/src/proto/*.c $(LRC)/src/shannon/*.c)

CFLAGS ?= -O2 -g
CFLAGS += -pthread -D_GNU_SOURCE -I$(LRC) -I$(LRC)/src $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -pthread

spike: $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDLIBS) -o $@

clean:
	rm -f spike
