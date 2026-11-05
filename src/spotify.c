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
 * A session streams one file at a time, so the files take turns. A new
 * play goes first: it cuts whatever is downloading. The cut download
 * waits for the channel to come free, then reopens the file at the byte
 * where it stopped and carries on. Downloads only wait on each other
 * when tracks are opened faster than they arrive: skipping, or a play
 * that fails while another track is still coming in. */

struct fetch {
	const struct mm_host *h;
	char uri[64];
	size_t size;
	struct track *t; /* NULL until track_open returns */
	int cancelled;   /* the track is closing: never take the channel again */
};

static struct {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	struct fetch *holder;
	int fresh_waiting; /* new plays waiting: resumes let them go first */
} CH = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0 };

static int channel_take(struct fetch *f, int fresh)
{
	pthread_mutex_lock(&CH.lock);
	if (fresh) CH.fresh_waiting++;
	for (;;) {
		if (f->cancelled) break;
		if (!CH.holder && (fresh || !CH.fresh_waiting)) {
			CH.holder = f;
			break;
		}
		if (fresh && CH.holder && CH.holder->t) track_cut(CH.holder->t);
		pthread_cond_wait(&CH.changed, &CH.lock);
	}
	if (fresh) CH.fresh_waiting--;
	int ok = CH.holder == f;
	pthread_mutex_unlock(&CH.lock);
	return ok ? 0 : -1;
}

static void channel_give(struct fetch *f)
{
	pthread_mutex_lock(&CH.lock);
	if (CH.holder == f) {
		CH.holder = NULL;
		pthread_cond_broadcast(&CH.changed);
	}
	pthread_mutex_unlock(&CH.lock);
}

/* Take the channel and start the file streaming from byte `from`.
 * @returns the fd to read, or -1 (logged) without the channel. */
static int fetch_start(struct fetch *f, size_t from, int fresh)
{
	const struct mm_host *h = f->h;
	struct sp_session *ss;
	struct sp_metadata md;
	int fd;

	if (channel_take(f, fresh) != 0) return -1;
	if (!(ss = session_get())) {
		h->log(h, MM_LOG_WARN, "%s: logged out", f->uri);
		goto fail;
	}
	if ((fd = librespotc_open(f->uri, ss)) < 0) {
		h->log(h, MM_LOG_WARN, "%s: %s", f->uri, librespotc_last_errmsg());
		goto fail;
	}
	if (fresh) {
		/* librespot-c does not reliably close the fd at the end, so the
		 * length is what says the download is complete. */
		f->size = librespotc_metadata_get(&md, fd) < 0 ? 0 : md.file_len;
		h->log(h, MM_LOG_DEBUG, "%s: %zu bytes", f->uri, f->size);
	}
	if (from && librespotc_seek(fd, from) < 0) {
		h->log(h, MM_LOG_WARN, "%s: cannot resume at %zu: %s", f->uri, from, librespotc_last_errmsg());
		librespotc_close(fd);
		goto fail;
	}
	librespotc_write(fd, NULL, NULL);
	return fd;

fail:
	channel_give(f);
	return -1;
}

/* ------------------------------------------------------------------ track_io */

static void io_release(void *arg, int fd, int complete)
{
	(void)complete;
	librespotc_close(fd);
	channel_give(arg);
}

static int io_resume(void *arg, size_t from)
{
	return fetch_start(arg, from, 0);
}

static void io_cancel(void *arg)
{
	struct fetch *f = arg;
	pthread_mutex_lock(&CH.lock);
	f->cancelled = 1;
	pthread_cond_broadcast(&CH.changed);
	pthread_mutex_unlock(&CH.lock);
}

static void io_free(void *arg)
{
	free(arg);
}

static const struct track_io io = { io_release, io_resume, io_cancel, io_free };

/* ------------------------------------------------------------------ source */

static int sp_open(void *priv, const char *uri, void **stream, struct mm_stream_info *info)
{
	const struct spotify *sp = priv;
	const struct mm_host *h = sp->h;
	char err[512];

	if (strncmp(uri, "spotify:track:", 14) != 0 || strlen(uri) >= sizeof(((struct fetch *)0)->uri)) {
		h->log(h, MM_LOG_WARN, "not a track: %s", uri);
		return -1;
	}
	if (session_login(err, sizeof(err)) != 0) {
		h->log(h, MM_LOG_WARN, "%s", err);
		return -1;
	}

	struct fetch *f = calloc(1, sizeof(*f));
	if (!f) return -1;
	f->h = h;
	snprintf(f->uri, sizeof(f->uri), "%s", uri);
	int fd = fetch_start(f, 0, 1);
	if (fd < 0) {
		free(f);
		return -1;
	}

	const char *ffmpeg = h->config_get(h, "ffmpeg");
	struct track_src src = { fd, f->size, &io, f };
	struct track *t = track_open(h, uri, ffmpeg && *ffmpeg ? ffmpeg : "ffmpeg", &src);
	if (!t) {
		io_release(f, fd, 0);
		free(f);
		return -1;
	}
	pthread_mutex_lock(&CH.lock);
	f->t = t;                              /* now it can be cut */
	pthread_cond_broadcast(&CH.changed);   /* a waiting play may want to */
	pthread_mutex_unlock(&CH.lock);

	info->duration_ms = 0; /* known when the download completes */
	info->flags = 0;
	*stream = t;
	return 0;
}

static long sp_read(void *stream, float *dst, uint32_t frames) { return track_read(stream, dst, frames); }
static int sp_seek(void *stream, uint64_t ms) { return track_seek(stream, ms); }
static void sp_interrupt(void *stream) { track_interrupt(stream); }
static void sp_close(void *stream) { track_close(stream); }

static const struct mm_source_ops source = {
	.open      = sp_open,
	.read      = sp_read,
	.seek      = sp_seek,
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
