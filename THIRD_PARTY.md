<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
# Third-party code and services

Every external component this plugin links, vendors, spawns or calls,
with its license. Updated in the same commit that adds a dependency.

Project license: LGPL-2.1-or-later (see LICENSE).

## Built against

| Component | License | Used for |
|---|---|---|
| [mediamaster](https://github.com/rakwahye/mediamaster) | LGPL-2.1-or-later | the plugin ABI, `mm_plugin.h`, from its installed SDK (`pkg-config mediamaster`); the daemon, for `make test` |

## Submodule

| Component | Pinned | License | Used for |
|---|---|---|---|
| [librespot-c](https://github.com/ejurgensen/librespot-c) | `librespot-c/` | MIT | Spotify protocol: login, track key, decrypted audio. Compiled into `spotify.so` with hidden visibility. `src/lrc/tokens.c` reads its internal session struct for the spclient tokens |

## Linked (system libraries, required by librespot-c)

Licences read from the packages' copyright files, 2026-11-05.

| Library | License | Notes |
|---|---|---|
| libevent | BSD-3-Clause | GPL-3+ appears only on an autoconf m4 macro, not the library |
| json-c | MIT | |
| libgcrypt | LGPL-2.1-or-later | its tools and manual are GPL-2+; not used |
| libcurl | curl (MIT-style) | |
| protobuf-c (runtime) | BSD-2-Clause | the protoc-c generator is BSD-3; not used (librespot-c ships generated code) |

## Services

| Service | Terms |
|---|---|
| Spotify (via librespot-c) | Premium account required. Unofficial client protocol: may change without notice, and use is likely against Spotify's terms. The user's call. |

## Read for design, not copied

| Project | License | What we took |
|---|---|---|
| librespot-org/librespot (Rust) | MIT | spclient endpoint paths and request shapes (`core/src/spclient.rs`); message field numbers from `protocol/proto` (`playlist4_external`, `collection2v2`, `context*`), read by hand in `pb.c` |
| devgianlu/go-librespot | GPL-3.0 | facts only: the collection paging media type and set names. No code |
| hrkfdn/ncspot | BSD-2-Clause | worker/event model around librespot |
| MichaelPachec0/ncspot | BSD-2-Clause | per-area latch for the internal APIs (planned) |
