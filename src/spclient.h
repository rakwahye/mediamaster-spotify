/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file spclient.h
 * @brief Spotify's own client API, with the session's tokens.
 *
 * spclient is the HTTP API Spotify's apps use: metadata, search,
 * playlists, library. It takes the bearer token login5 issues to the
 * session and the client token, both of which librespot-c already
 * holds. No Web API, no developer app. */

#ifndef SPOTIFY_SPCLIENT_H
#define SPOTIFY_SPCLIENT_H

#include <stddef.h>
#include <stdint.h>

struct sp_session;

/** Where and as whom to ask. A copy: valid until the access token expires
 * (an hour; renewed whenever librespot-c opens a track). */
struct spclient {
	char host[256];
	unsigned short port;
	char access_token[512];
	char client_token[512];
};

/** Copy the session's spclient address and tokens. Call only between
 * librespot-c commands that leave the session alone, such as right after
 * librespotc_open returned while no other open can run (see lrc/tokens.c).
 * @returns 0, or -1 if the session has no tokens yet. */
int spclient_from_session(struct sp_session *ss, struct spclient *out);

struct spclient_reply {
	long status;   /* HTTP status, 0 if none */
	uint8_t *body; /* malloc'd, free with free() */
	size_t len;
};

/** GET https://<host>:<port>/<path> with the tokens, `accept` as Accept.
 * @returns 0 with a 2xx reply, else -1 with the reason in err (the reply,
 * if any, is freed). */
int spclient_get(const struct spclient *c, const char *path, const char *accept,
                 struct spclient_reply *r, char *err, size_t errlen);

#endif
