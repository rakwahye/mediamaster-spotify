/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file creds.h
 * @brief Stored Spotify credentials, in librespot's credentials.json format.
 *
 *   {"username": "...", "auth_type": 1, "auth_data": "<base64>"}
 *
 * auth_type 1 is AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS: a reusable
 * blob Spotify hands out after a login. librespot, ncspot and spotifyd
 * write this file, so mediamaster can start from theirs. */

#ifndef SPOTIFY_CREDS_H
#define SPOTIFY_CREDS_H

#include <stddef.h>
#include <stdint.h>

#define CREDS_BLOB_MAX 512

struct creds {
	char username[64];
	uint8_t blob[CREDS_BLOB_MAX];
	size_t blob_len;
};

/** Read `path`. @returns 0, or -1 with a reason in err. */
int creds_load(const char *path, struct creds *c, char *err, size_t errlen);

/** Write `path` atomically, mode 0600. @returns 0, or -1 with a reason. */
int creds_save(const char *path, const struct creds *c, char *err, size_t errlen);

#endif
