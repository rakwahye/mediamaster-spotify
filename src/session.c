/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file session.c
 * @brief Login, logout and credential persistence. */

#include "session.h"
#include "creds.h"
#include "librespot-c.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct {
	pthread_mutex_t lock;
	char cache[1024];
	char import[1024];
	char saved[1100];     /* <cache>/credentials.json */
	struct sp_session *sp;
	char username[64];
	char failure[256];    /* last login error, "" if none */
} S = { .lock = PTHREAD_MUTEX_INITIALIZER };

void session_setup(const char *cache, const char *import)
{
	pthread_mutex_lock(&S.lock);
	snprintf(S.cache, sizeof(S.cache), "%s", cache);
	snprintf(S.import, sizeof(S.import), "%s", import ? import : "");
	snprintf(S.saved, sizeof(S.saved), "%s/credentials.json", cache);
	pthread_mutex_unlock(&S.lock);
}

/* Lock held. Our own file first, then the import. */
static int find_credentials(struct creds *c, char *err, size_t errlen)
{
	if (access(S.saved, F_OK) == 0) return creds_load(S.saved, c, err, errlen);
	if (S.import[0]) return creds_load(S.import, c, err, errlen);
	snprintf(err, errlen, "no credentials: set spotify.credentials to a librespot or ncspot credentials.json");
	return -1;
}

int session_login(char *err, size_t errlen)
{
	struct creds c;
	struct sp_credentials fresh;
	char why[256];
	int rc = -1;

	pthread_mutex_lock(&S.lock);
	if (S.sp) {
		pthread_mutex_unlock(&S.lock);
		return 0;
	}
	if (find_credentials(&c, err, errlen) != 0) goto out;

	S.sp = librespotc_login_stored_cred(c.username, c.blob, c.blob_len);
	if (!S.sp) {
		snprintf(err, errlen, "login failed: %s", librespotc_last_errmsg());
		goto out;
	}
	snprintf(S.username, sizeof(S.username), "%s", c.username);
	librespotc_bitrate_set(S.sp, SP_BITRATE_320);

	/* Keep what Spotify handed back, so we stand on our own from now on. */
	if (librespotc_credentials_get(&fresh, S.sp) == 0 && fresh.stored_cred_len &&
	    fresh.stored_cred_len <= sizeof(c.blob)) {
		snprintf(c.username, sizeof(c.username), "%s", fresh.username[0] ? fresh.username : S.username);
		memcpy(c.blob, fresh.stored_cred, fresh.stored_cred_len);
		c.blob_len = fresh.stored_cred_len;
	}
	if (creds_save(S.saved, &c, why, sizeof(why)) != 0)
		snprintf(err, errlen, "logged in, but credentials not saved: %s", why); /* still a success */
	rc = 0;
out:
	snprintf(S.failure, sizeof(S.failure), "%s", rc ? err : "");
	pthread_mutex_unlock(&S.lock);
	return rc;
}

void session_logout(void)
{
	pthread_mutex_lock(&S.lock);
	if (S.sp) librespotc_logout(S.sp);
	S.sp = NULL;
	S.username[0] = '\0';
	S.failure[0] = '\0';
	unlink(S.saved);
	pthread_mutex_unlock(&S.lock);
}

void session_describe(char *out, size_t cap)
{
	pthread_mutex_lock(&S.lock);
	if (S.sp) snprintf(out, cap, "logged in as %s", S.username);
	else if (S.failure[0]) snprintf(out, cap, "logged out (%s)", S.failure);
	else snprintf(out, cap, "logged out");
	pthread_mutex_unlock(&S.lock);
}

struct sp_session *session_get(void)
{
	pthread_mutex_lock(&S.lock);
	struct sp_session *sp = S.sp;
	pthread_mutex_unlock(&S.lock);
	return sp;
}

void session_teardown(void)
{
	pthread_mutex_lock(&S.lock);
	if (S.sp) librespotc_logout(S.sp);
	S.sp = NULL;
	pthread_mutex_unlock(&S.lock);
}
