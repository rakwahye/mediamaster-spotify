<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
# Third-party code and services

Every external component this plugin links, vendors, spawns or calls,
with its license. Updated in the same commit that adds a dependency.

Project license: LGPL-2.1-or-later (see LICENSE).

## Submodules

| Component | Pinned | License | Used for |
|---|---|---|---|
| [mediamaster](https://github.com/rakwahye/mediamaster) | `mediamaster/` | LGPL-2.1-or-later | `include/mm_plugin.h` (the plugin ABI); the host for `make test` |
| [librespot-c](https://github.com/ejurgensen/librespot-c) | `librespot-c/` | MIT | Spotify protocol: login, track key, decrypted audio. Compiled into `spotify.so` with hidden visibility |

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
