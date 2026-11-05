/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file meta.h
 * @brief What a Spotify track is: its tags, from spclient's metadata. */

#ifndef SPOTIFY_META_H
#define SPOTIFY_META_H

#include "spclient.h"

#define META_PAIRS 24

/** Tags in the form mm_host.stream_tags takes: kv points into buf. */
struct meta {
	const char *kv[2 * META_PAIRS + 1];
	char buf[4096];
};

/** spotify:track:<base62 id> → tags (title, each artist, album, each
 * album artist, track and disc number, date, cover art).
 * @returns 0, or -1 with the reason in err. */
int meta_track(const struct spclient *c, const char *uri, struct meta *m, char *err, size_t errlen);

/** Parse a metadata/4/track reply into m. Split out for tests.
 * @returns 0, or -1 if it is not a track. */
int meta_from_track(const uint8_t *body, size_t len, struct meta *m);

/** Base62 Spotify id (22 characters) → 32 lowercase hex digits.
 * @returns 0, or -1 if it is not one. */
int meta_id_hex(const char *id, char hex[33]);

#endif
