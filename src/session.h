/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file session.h
 * @brief The Spotify account session.
 *
 * Credentials come from mediamaster's own <cache>/credentials.json, or,
 * until it has one, from spotify.credentials (a librespot or ncspot file,
 * never written to). After each login the fresh reusable credentials
 * Spotify returns are saved to the cache.
 *
 * Calls are serialised; login blocks for the network round trips. */

#ifndef SPOTIFY_SESSION_H
#define SPOTIFY_SESSION_H

#include <stddef.h>

struct sp_session;

/** @param cache    directory for credentials.json (must exist)
 *  @param import   fallback credentials file, or NULL */
void session_setup(const char *cache, const char *import);

/** Log in unless logged in. @returns 0, or -1 with a reason in err. */
int session_login(char *err, size_t errlen);

/** End the session and delete mediamaster's saved credentials. */
void session_logout(void);

/** One line: "logged in as X", "logged out", "login failed: ...". */
void session_describe(char *out, size_t cap);

/** The live librespot-c session, or NULL. Valid until logout. */
struct sp_session *session_get(void);

void session_teardown(void);

#endif
