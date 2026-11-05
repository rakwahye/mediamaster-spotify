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
 * Plays spotify:track:<id>; lists your library, playlists, albums and
 * artists (library.c). Logs in on first use if not logged in.
 *
 * Commands:
 *   spotify                  account state, cache location, library version
 *   spotify login | logout
 *
 * Files: spotify.c (this: the plugin), session.c (the account),
 * creds.c (credentials.json), lrc.c (librespot-c glue), track.c (one
 * track: download, decode), ogg.c (Ogg page index), spclient.c (Spotify's
 * client API), meta.c (a track's tags), library.c (lists), pb.c (protobuf
 * wire format), lrc/tokens.c (the session's tokens, from librespot-c's
 * internals). */

#include "mm_plugin.h"
#include "librespot-c.h"
#include "library.h"
#include "lrc.h"
#include "meta.h"
#include "session.h"
#include "spclient.h"
#include "track.h"
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

/* The session's spclient address and tokens, refreshed on every open
 * (librespot-c renews the token then if it is due). */
static struct {
	pthread_mutex_t lock;
	struct spclient c;
	int ok;
} SPC = { PTHREAD_MUTEX_INITIALIZER, { "", 0, "", "", 0 }, 0 };

static int spclient_current(struct spclient *out)
{
	pthread_mutex_lock(&SPC.lock);
	int ok = SPC.ok;
	if (ok) *out = SPC.c;
	pthread_mutex_unlock(&SPC.lock);
	return ok ? 0 : -1;
}

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
	struct spclient c; /* safe to read now: see lrc/tokens.c */
	if (spclient_from_session(ss, &c) == 0) {
		pthread_mutex_lock(&SPC.lock);
		SPC.c = c;
		SPC.ok = 1;
		pthread_mutex_unlock(&SPC.lock);
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

	/* What it is. Playback has started downloading; a failure here only
	 * means the track plays untagged. */
	struct spclient c;
	struct meta m;
	if (MM_HOST_HAS(h, stream_tags) && spclient_current(&c) == 0) {
		if (meta_track(&c, uri, &m, err, sizeof(err)) == 0) h->stream_tags(h, t, m.kv);
		else h->log(h, MM_LOG_INFO, "no tags: %s", err);
	}

	info->duration_ms = 0; /* known when the download completes */
	info->flags = 0;
	*stream = t;
	return 0;
}

static long sp_read(void *stream, float *dst, uint32_t frames) { return track_read(stream, dst, frames); }
static int sp_seek(void *stream, uint64_t ms) { return track_seek(stream, ms); }
static void sp_interrupt(void *stream) { track_interrupt(stream); }
static void sp_close(void *stream) { track_close(stream); }

/* ------------------------------------------------------------------ tokens
 *
 * librespot-c fetches its spclient tokens while opening a track, and only
 * then. To list before anything has played, or once the token has aged,
 * open a track id that does not exist: the tokens come first, the lookup
 * then fails, and the session stays logged in (it reconnects for the next
 * track). The open takes the channel like a new play, so a download in
 * progress pauses and resumes after. */
static void tokens_refresh(const struct mm_host *h)
{
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	struct fetch f;
	memset(&f, 0, sizeof(f));
	f.h = h;
	snprintf(f.uri, sizeof(f.uri), "spotify:track:0000000000000000000000");
	if (channel_take(&f, 1) != 0) return;
	struct sp_session *ss = session_get();
	struct spclient c;
	if (ss) {
		int fd = librespotc_open(f.uri, ss);
		if (fd >= 0) librespotc_close(fd);
		if (spclient_from_session(ss, &c) == 0) { /* safe: see lrc/tokens.c */
			pthread_mutex_lock(&SPC.lock);
			SPC.c = c;
			SPC.ok = 1;
			pthread_mutex_unlock(&SPC.lock);
		}
	}
	channel_give(&f);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	h->log(h, MM_LOG_DEBUG, "tokens refreshed in %ld ms",
	       (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
}

static int spclient_ready(const struct mm_host *h, struct spclient *out, char *err, size_t errlen)
{
	if (spclient_current(out) == 0 && out->expires > time(NULL) + 60) return 0;
	tokens_refresh(h);
	if (spclient_current(out) == 0 && out->expires > time(NULL)) return 0;
	snprintf(err, errlen, "Spotify gave no token (details in the debug log)");
	return -1;
}

/* ------------------------------------------------------------------ catalog */

static int sp_list(void *priv, const char *uri, uint32_t flags, mm_item_fn emit, void *ctx,
                   char *err, size_t errlen)
{
	const struct spotify *sp = priv;
	struct spclient c;
	char user[64];

	if (!library_is_list(uri)) return MM_NOT_A_LIST; /* tracks, episodes: no network */
	if (session_login(err, errlen) != 0 || spclient_ready(sp->h, &c, err, errlen) != 0) return -1;
	if (session_username(user, sizeof(user)) != 0) {
		snprintf(err, errlen, "logged out");
		return -1;
	}
	return library_list(&c, user, uri, flags, emit, ctx, err, errlen);
}

static const struct mm_catalog_ops catalog = { .list = sp_list };

static const struct mm_source_ops source = {
	.open      = sp_open,
	.read      = sp_read,
	.seek      = sp_seek,
	.interrupt = sp_interrupt,
	.close     = sp_close,
};

/* ------------------------------------------------------------------ plugin */

/* spclient's request lines, into the host log at debug level. */
static const struct mm_host *g_debug_host;

static void debug_line(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (g_debug_host) g_debug_host->log(g_debug_host, MM_LOG_DEBUG, "%s", line);
}

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
	g_debug_host = h;
	spclient_debug = debug_line;
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		free(sp);
		return -1;
	}
	if (lrc_start(h, sp->cache) != 0) {
		curl_global_cleanup();
		free(sp);
		return -1;
	}
	session_setup(sp->cache, h->config_get(h, "credentials"));
	if (h->register_verb(h, "spotify", "[login|logout]\taccount state; log in or out", verb_spotify, sp) != 0) {
		lrc_stop();
		curl_global_cleanup();
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
	spclient_debug = NULL;
	spclient_cleanup();
	curl_global_cleanup();
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
	.catalog     = &catalog,
};

MM_PLUGIN_DEFINE(spotify)
{
	return &plugin;
}
