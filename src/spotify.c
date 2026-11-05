/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file spotify.c
 * @brief Spotify for mediamaster.
 *
 * A mediamaster plugin is one shared object exporting one function,
 * mm_plugin_entry(), which returns a static `struct mm_plugin`. The
 * struct names the plugin, states its policy attributes, and points at
 * the interface tables it implements. Everything the plugin needs from
 * the daemon arrives in `struct mm_host` at init.
 *
 * Config is read under the plugin's own name ("spotify.<key>"):
 *   spotify.cache = <dir>          login and device id
 *                                  (default $XDG_CACHE_HOME/mediamaster/spotify)
 *   spotify.credentials = <file>   a librespot or ncspot credentials.json to
 *                                  log in with until mediamaster has its own
 *   spotify.ffmpeg = <bin>         decoder (default ffmpeg)
 *
 * Plays spotify:track:<id>. Logs in on the first play if not logged in.
 *
 * Commands:
 *   spotify                  account state, cache location, library version
 *   spotify login | logout
 *
 * Files: spotify.c (this: the plugin), session.c (the account),
 * creds.c (credentials.json), lrc.c (librespot-c glue), track.c (one
 * track: download, decode), ogg.c (Ogg page index). */

#include "mm_plugin.h"
#include "lrc.h"
#include "session.h"
#include "track.h"
#include "librespot-c.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

struct spotify {
	const struct mm_host *h;
	char cache[1024];
};

/* ------------------------------------------------------------------ channel
 *
 * A session streams one file at a time. The newest open takes the
 * channel: an unfinished download is cut and plays as far as it got.
 * That only ever bites a track being skipped or replaced, since a track
 * still downloading near its end could not have played without gaps
 * anyway. Each holder gets a ticket, so a download that is over before
 * its open returns is not recorded as the owner afterwards. */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t freed;
	int busy;
	uintptr_t ticket;
	struct track *owner; /* NULL while the holder is still opening */
} CH = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, NULL };

static uintptr_t channel_take(void)
{
	pthread_mutex_lock(&CH.lock);
	while (CH.busy) {
		if (CH.owner) track_cut(CH.owner);
		pthread_cond_wait(&CH.freed, &CH.lock);
	}
	CH.busy = 1;
	CH.owner = NULL;
	uintptr_t ticket = ++CH.ticket;
	pthread_mutex_unlock(&CH.lock);
	return ticket;
}

static void channel_give(uintptr_t ticket)
{
	pthread_mutex_lock(&CH.lock);
	if (CH.busy && CH.ticket == ticket) {
		CH.busy = 0;
		CH.owner = NULL;
		pthread_cond_broadcast(&CH.freed);
	}
	pthread_mutex_unlock(&CH.lock);
}

static void channel_own(uintptr_t ticket, struct track *t)
{
	pthread_mutex_lock(&CH.lock);
	if (CH.busy && CH.ticket == ticket) CH.owner = t;
	pthread_mutex_unlock(&CH.lock);
}

/* track_src.release: the download is over. */
static void release(void *arg, int fd, int complete)
{
	(void)complete;
	librespotc_close(fd);
	channel_give((uintptr_t)arg);
}

/* ------------------------------------------------------------------ source */

static int sp_open(void *priv, const char *uri, void **stream, struct mm_stream_info *info)
{
	const struct spotify *sp = priv;
	const struct mm_host *h = sp->h;
	char err[512];

	if (strncmp(uri, "spotify:track:", 14) != 0) {
		h->log(h, MM_LOG_WARN, "not a track: %s", uri);
		return -1;
	}
	if (session_login(err, sizeof(err)) != 0) {
		h->log(h, MM_LOG_WARN, "%s", err);
		return -1;
	}

	uintptr_t ticket = channel_take();
	struct sp_metadata md;
	int fd = librespotc_open(uri, session_get());
	if (fd < 0) {
		h->log(h, MM_LOG_WARN, "%s: %s", uri, librespotc_last_errmsg());
		channel_give(ticket);
		return -1;
	}
	if (librespotc_metadata_get(&md, fd) < 0) md.file_len = 0;
	librespotc_write(fd, NULL, NULL);

	const char *ffmpeg = h->config_get(h, "ffmpeg");
	struct track_src src = { fd, md.file_len, release, (void *)ticket };
	struct track *t = track_open(h, uri, ffmpeg && *ffmpeg ? ffmpeg : "ffmpeg", &src);
	if (!t) {
		release((void *)ticket, fd, 0);
		return -1;
	}
	channel_own(ticket, t);

	info->duration_ms = 0; /* known when the download completes */
	info->flags = 0;
	*stream = t;
	return 0;
}

static long sp_read(void *stream, float *dst, uint32_t frames) { return track_read(stream, dst, frames); }
static void sp_interrupt(void *stream) { track_interrupt(stream); }
static void sp_close(void *stream) { track_close(stream); }

static const struct mm_source_ops source = {
	.open      = sp_open,
	.read      = sp_read,
	.interrupt = sp_interrupt,
	.close     = sp_close,
};

/* ------------------------------------------------------------------ plugin */

static void default_cache(char *out, size_t cap)
{
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0] == '/')
		snprintf(out, cap, "%s/mediamaster/spotify", xdg);
	else
		snprintf(out, cap, "%s/.cache/mediamaster/spotify", home ? home : "/tmp");
}

/* mkdir -p, mode 0700: the cache holds a login. */
static int mkdir_p(const char *dir)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s", dir);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/') continue;
		*s = '\0';
		if (mkdir(p, 0700) != 0 && errno != EEXIST) return -1;
		*s = '/';
	}
	return mkdir(p, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

static int verb_spotify(void *priv, struct mm_verb_ctx *c)
{
	const struct spotify *sp = priv;
	const char *sub = c->argc > 1 ? c->argv[1] : "";
	char state[512], err[512];

	if (!strcmp(sub, "login")) {
		if (session_login(err, sizeof(err)) != 0) {
			snprintf(c->reply, c->reply_len, "%s", err);
			return MM_ERR;
		}
		session_describe(state, sizeof(state));
		snprintf(c->reply, c->reply_len, "%s", state);
		return MM_OK;
	}
	if (!strcmp(sub, "logout")) {
		session_logout();
		snprintf(c->reply, c->reply_len, "logged out");
		return MM_OK;
	}
	if (*sub) {
		snprintf(c->reply, c->reply_len, "usage: spotify [login | logout]");
		return MM_ERR;
	}
	session_describe(state, sizeof(state));
	snprintf(c->reply, c->reply_len, "%s, cache %s, %s", state, sp->cache, lrc_version());
	return MM_OK;
}

static int spotify_init(const struct mm_host *h, void **priv)
{
	struct spotify *sp = calloc(1, sizeof(*sp));
	if (!sp) return -1;
	sp->h = h;

	const char *cache = h->config_get(h, "cache");
	if (cache && *cache) snprintf(sp->cache, sizeof(sp->cache), "%s", cache);
	else default_cache(sp->cache, sizeof(sp->cache));

	if (mkdir_p(sp->cache) != 0) {
		h->log(h, MM_LOG_WARN, "cannot create %s: %s", sp->cache, strerror(errno));
		free(sp);
		return -1;
	}
	if (lrc_start(h, sp->cache) != 0) {
		free(sp);
		return -1;
	}
	session_setup(sp->cache, h->config_get(h, "credentials"));
	if (h->register_verb(h, "spotify", "[login|logout]\taccount state; log in or out", verb_spotify, sp) != 0) {
		lrc_stop();
		free(sp);
		return -1;
	}
	*priv = sp;
	return 0;
}

static void spotify_fini(void *priv)
{
	session_teardown();
	lrc_stop();
	free(priv);
}

static const struct mm_plugin plugin = {
	.abi         = MM_ABI,
	.attrs       = MM_ATTR_NETWORK | MM_ATTR_NEEDS_LOGIN |
	               MM_ATTR_RATE_LIMITED | MM_ATTR_UNOFFICIAL_API,
	.name        = "spotify",
	.version     = "0.1.0",
	.description = "Spotify, via librespot-c",
	.schemes     = "spotify",
	.init        = spotify_init,
	.fini        = spotify_fini,
	.source      = &source,
};

MM_PLUGIN_DEFINE(spotify)
{
	return &plugin;
}
