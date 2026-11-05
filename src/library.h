/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file library.h
 * @brief The catalog: what is inside your library, playlists, albums.
 *
 *   spotify:                 Liked Songs, Your Albums, Your Artists, then your
 *                            playlists
 *   spotify:collection       Liked Songs (spotify:user:<you>:collection too)
 *   spotify:collection:albums    the albums you saved
 *   spotify:collection:artists   the artists you follow
 *   spotify:playlist:<id>    a playlist's tracks
 *   spotify:album:<id>       an album's tracks
 *   spotify:artist:<id>      an artist's top tracks and latest release
 *   spotify:search:<words>   what Spotify's search finds (first page)
 *
 * Tracks come from context-resolve, the endpoint Spotify's apps fill a
 * play context from (JSON, paged); your playlists from your rootlist
 * (protobuf); saved albums and followed artists from collection paging
 * (protobuf); titles, when wanted, from extended metadata in batches. */

#ifndef SPOTIFY_LIBRARY_H
#define SPOTIFY_LIBRARY_H

#include "mm_plugin.h"
#include "spclient.h"

/** 1 if `uri` is one of the lists above (decided without the network). */
int library_is_list(const char *uri);

/** mm_catalog_ops.list for one of the URIs above, as `user`.
 * @returns 0, MM_NOT_A_LIST for a track, or -1 with err set. */
int library_list(const struct spclient *c, const char *user, const char *uri, uint32_t flags,
                 mm_item_fn emit, void *ctx, char *err, size_t errlen);

/* ---- the parsers, split out for tests ---- */

/** Words → "spotify:search:<words>" as Spotify's apps write it: spaces
 * as '+', anything not unreserved percent-encoded. */
void lib_search_uri(const char *words, char *out, size_t cap);

/* A track, or for the collection's lists an album or artist. */
struct lib_track {
	char *uri, *title, *artist, *album; /* malloc'd; all but uri may be NULL */
	uint64_t duration_ms;
};

struct lib_tracks {
	struct lib_track *v;
	size_t n, cap;
};

void lib_tracks_free(struct lib_tracks *t);

/** Spclient paths of pages still to fetch, in order. */
struct lib_pages {
	char **path;
	size_t n, cap;
};

void lib_pages_free(struct lib_pages *p);

/** A context-resolve reply (a context, or one of its later pages): add
 * its tracks to t, and the spclient path of every page it only links to
 * (page_url on a page without tracks, next_page_url) to `more`, in
 * order. @returns 0, or -1 if it is not one. */
int lib_parse_context(const char *json, struct lib_tracks *t, struct lib_pages *more);

/** A rootlist reply: on_list(ctx, uri, name, owner) per playlist, in
 * order (folders flattened), and the full count into *total.
 * @returns 0, or -1 if malformed. */
int lib_parse_rootlist(const uint8_t *body, size_t len,
                       void (*on_list)(void *ctx, const char *uri, const char *name, const char *owner),
                       void *ctx, int *total);

/** A collection paging reply: add the items whose URI starts with
 * `prefix` and are not removed to t; the next page's token, or "", into
 * next. @returns 0, or -1 if malformed. */
int lib_parse_collection(const uint8_t *body, size_t len, const char *prefix, struct lib_tracks *t,
                         char *next, size_t nextcap);

enum lib_kind { LIB_TRACK, LIB_ALBUM, LIB_ARTIST };

/** An extended-metadata reply of `kind`: names (and for tracks artists,
 * album and length; for albums artists) onto the matching entries of t.
 * @returns 0, or -1. */
int lib_parse_batch(const uint8_t *body, size_t len, enum lib_kind kind, struct lib_tracks *t);

#endif
