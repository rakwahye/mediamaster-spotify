/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file track.c
 * @brief Download into memory, feed ffmpeg, read PCM.
 *
 * Three threads touch a track:
 *   download  fd → buf, indexing pages as they complete, until the
 *             expected size or EOF
 *   feed      buf → ffmpeg's stdin, waiting for the download when caught up
 *   host      ffmpeg's stdout → read(); interrupt and close
 * `lock` guards buf, have, done, cut and the report flags; `more` is
 * signalled when any of them changes.
 *
 * ffmpeg gets an empty signal mask, default dispositions and only the
 * two pipes (every fd of ours is close-on-exec); its stderr is the
 * daemon's, so decode errors land in the daemon's log. */

#define _GNU_SOURCE /* pipe2 */
#include "track.h"
#include "ogg.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define FRAME_BYTES (MM_PCM_CHANNELS * sizeof(float))
#define FEED_CHUNK  (64 * 1024)
#define READ_CHUNK  (64 * 1024)

struct track {
	const struct mm_host *h;
	char name[128];
	struct track_src src;

	pthread_mutex_t lock;
	pthread_cond_t more;
	uint8_t *buf;
	size_t cap, have;
	struct ogg_index ix;
	int done;              /* download over: complete, failed or cut */
	int stopping;          /* track_close: feeder must quit */
	int reading;           /* the host has called read() */
	int report;            /* 1: info ready to report, 2: reported */
	struct mm_stream_info info;

	int cut[2];            /* self-pipe: stop the download */
	int wake[2];           /* self-pipe: interrupt read() */
	pid_t pid;
	int in, out;           /* ffmpeg's stdin (ours to write), stdout */
	pthread_t dl, feed;
	int dl_on, feed_on;

	unsigned char part[FRAME_BYTES]; /* partial frame between reads */
	size_t part_len;
};

/* ------------------------------------------------------------------ ffmpeg */

static int spawn_ffmpeg(struct track *t, const char *ffmpeg)
{
	char *argv[] = {
		(char *)ffmpeg, (char *)"-nostdin", (char *)"-hide_banner",
		(char *)"-loglevel", (char *)"error",
		(char *)"-f", (char *)"ogg", (char *)"-i", (char *)"pipe:0",
		(char *)"-map", (char *)"0:a:0", (char *)"-f", (char *)"f32le",
		(char *)"-ar", (char *)"48000", (char *)"-ac", (char *)"2", (char *)"pipe:1",
		NULL
	};
	int pi[2], po[2];
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	sigset_t none, all;

	if (pipe2(pi, O_CLOEXEC) != 0) return -1;
	if (pipe2(po, O_CLOEXEC) != 0) {
		close(pi[0]); close(pi[1]);
		return -1;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pi[0], 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawnattr_init(&at);
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &all);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	int rc = posix_spawnp(&t->pid, ffmpeg, &fa, &at, argv, environ);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	close(pi[0]);
	close(po[1]);
	if (rc != 0) {
		t->h->log(t->h, MM_LOG_WARN, "cannot run %s: %s", ffmpeg, strerror(rc));
		close(pi[1]);
		close(po[0]);
		t->pid = -1;
		return -1;
	}
	t->in = pi[1];
	t->out = po[0];
	return 0;
}

/* Its output is no longer wanted: no grace, no "Broken pipe" chatter. */
static void stop_ffmpeg(pid_t pid)
{
	kill(pid, SIGKILL);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
}

/* ------------------------------------------------------------------ report */

/* Lock held. Hand the final info to the host once it is both known and
 * the host is reading. @returns 1 if the caller should send it now. */
static int report_due(struct track *t)
{
	if (t->report != 1 || !t->reading) return 0;
	t->report = 2;
	return 1;
}

static void report(struct track *t)
{
	if (MM_HOST_HAS(t->h, stream_update)) t->h->stream_update(t->h, t, &t->info);
}

/* ------------------------------------------------------------------ download */

/* Lock held. Make room for n more bytes. */
static int reserve(struct track *t, size_t n)
{
	if (t->cap - t->have >= n) return 0;
	size_t cap = t->cap ? t->cap : 1u << 20;
	while (cap - t->have < n) cap *= 2;
	uint8_t *b = realloc(t->buf, cap);
	if (!b) return -1;
	t->buf = b;
	t->cap = cap;
	return 0;
}

static void *download(void *arg)
{
	struct track *t = arg;
	uint8_t chunk[READ_CHUNK];
	int complete = 0;
	const char *why = NULL;

	for (;;) {
		struct pollfd pf[2] = { { t->src.fd, POLLIN, 0 }, { t->cut[0], POLLIN, 0 } };
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			why = strerror(errno);
			break;
		}
		if (pf[1].revents) {
			why = "cut short";
			break;
		}
		if (!pf[0].revents) continue;
		ssize_t r = read(t->src.fd, chunk, sizeof(chunk));
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			why = strerror(errno);
			break;
		}
		if (r == 0) {
			complete = 1;
			break;
		}
		pthread_mutex_lock(&t->lock);
		int bad = reserve(t, (size_t)r);
		if (!bad) {
			memcpy(t->buf + t->have, chunk, (size_t)r);
			t->have += (size_t)r;
			bad = ogg_index_scan(&t->ix, t->buf, t->have);
			pthread_cond_broadcast(&t->more);
		}
		complete = t->src.size && t->have >= t->src.size;
		pthread_mutex_unlock(&t->lock);
		if (bad) {
			why = "out of memory";
			break;
		}
		if (complete) break; /* the sender need not close to say so */
	}

	t->src.release(t->src.arg, t->src.fd, complete);

	pthread_mutex_lock(&t->lock);
	t->done = 1;
	if (complete) {
		t->info.duration_ms = ogg_index_duration_ms(&t->ix);
		t->info.flags = 0;
		t->report = 1;
	}
	int send = report_due(t);
	pthread_cond_broadcast(&t->more);
	size_t have = t->have;
	pthread_mutex_unlock(&t->lock);

	if (complete)
		t->h->log(t->h, MM_LOG_DEBUG, "%s: downloaded %zu bytes, %llu ms", t->name, have,
		          (unsigned long long)t->info.duration_ms);
	else
		t->h->log(t->h, MM_LOG_INFO, "%s: download %s after %zu bytes", t->name, why, have);
	if (send) report(t);
	return NULL;
}

/* ------------------------------------------------------------------ feed */

static void *feed(void *arg)
{
	struct track *t = arg;
	uint8_t chunk[FEED_CHUNK];
	size_t pos = 0;
	sigset_t pipe_sig;

	/* A decoder that went away is EPIPE here, not a signal. */
	sigemptyset(&pipe_sig);
	sigaddset(&pipe_sig, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_sig, NULL);

	for (;;) {
		pthread_mutex_lock(&t->lock);
		while (pos == t->have && !t->done && !t->stopping)
			pthread_cond_wait(&t->more, &t->lock);
		size_t n = t->have - pos;
		if (t->stopping || n == 0) {
			pthread_mutex_unlock(&t->lock);
			break;
		}
		if (n > sizeof(chunk)) n = sizeof(chunk);
		memcpy(chunk, t->buf + pos, n);
		pthread_mutex_unlock(&t->lock);

		size_t off = 0;
		while (off < n) {
			ssize_t w = write(t->in, chunk + off, n - off);
			if (w < 0 && errno == EINTR) continue;
			if (w < 0) goto out; /* ffmpeg is gone; read() sees the end */
			off += (size_t)w;
		}
		pos += n;
	}
out:
	close(t->in); /* end of input: ffmpeg flushes and exits */
	t->in = -1;
	return NULL;
}

/* ------------------------------------------------------------------ ops */

static void close_pair(int p[2])
{
	if (p[0] >= 0) close(p[0]);
	if (p[1] >= 0) close(p[1]);
}

struct track *track_open(const struct mm_host *h, const char *name, const char *ffmpeg,
                         const struct track_src *src)
{
	struct track *t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	t->h = h;
	t->src = *src;
	snprintf(t->name, sizeof(t->name), "%s", name);
	t->cut[0] = t->cut[1] = t->wake[0] = t->wake[1] = -1;
	t->pid = -1;
	t->in = t->out = -1;
	ogg_index_init(&t->ix);
	pthread_mutex_init(&t->lock, NULL);
	pthread_cond_init(&t->more, NULL);

	if (src->size) {
		t->buf = malloc(src->size);
		if (t->buf) t->cap = src->size;
	}
	if (pipe2(t->cut, O_CLOEXEC | O_NONBLOCK) != 0 || pipe2(t->wake, O_CLOEXEC | O_NONBLOCK) != 0)
		goto fail;
	if (spawn_ffmpeg(t, ffmpeg) != 0) goto fail;
	if (pthread_create(&t->feed, NULL, feed, t) != 0) goto fail;
	t->feed_on = 1;
	if (pthread_create(&t->dl, NULL, download, t) != 0) goto fail;
	t->dl_on = 1;
	return t;

fail:
	if (t->feed_on) { /* the download never started: nothing released */
		pthread_mutex_lock(&t->lock);
		t->stopping = 1;
		pthread_cond_broadcast(&t->more);
		pthread_mutex_unlock(&t->lock);
		pthread_join(t->feed, NULL);
	}
	if (t->out >= 0) close(t->out);
	if (t->in >= 0) close(t->in);
	if (t->pid > 0) stop_ffmpeg(t->pid);
	close_pair(t->cut);
	close_pair(t->wake);
	pthread_cond_destroy(&t->more);
	pthread_mutex_destroy(&t->lock);
	ogg_index_free(&t->ix);
	free(t->buf);
	free(t);
	return NULL;
}

long track_read(struct track *t, float *dst, uint32_t frames)
{
	pthread_mutex_lock(&t->lock);
	t->reading = 1;
	int send = report_due(t);
	pthread_mutex_unlock(&t->lock);
	if (send) report(t);

	if (t->out < 0) return 0;
	unsigned char *d = (unsigned char *)dst;
	size_t want = (size_t)frames * FRAME_BYTES;
	size_t have = t->part_len;
	memcpy(d, t->part, have);

	while (have < FRAME_BYTES) {
		struct pollfd pf[2] = { { t->out, POLLIN, 0 }, { t->wake[0], POLLIN, 0 } };
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		if (pf[1].revents) {   /* interrupted: once; partial frame kept */
			char b[16];
			while (read(t->wake[0], b, sizeof(b)) > 0) { }
			return 0;
		}
		if (!pf[0].revents) continue;
		ssize_t r = read(t->out, d + have, want - have);
		if (r < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		if (r == 0) {          /* end of track, or ffmpeg failed */
			close(t->out);
			t->out = -1;
			t->part_len = 0;
			return 0;
		}
		have += (size_t)r;
	}
	size_t whole = have - have % FRAME_BYTES;
	t->part_len = have - whole;
	memcpy(t->part, d + whole, t->part_len);
	return (long)(whole / FRAME_BYTES);
}

void track_interrupt(struct track *t)
{
	char c = 1;
	ssize_t w = write(t->wake[1], &c, 1);
	(void)w; /* full pipe: already pending */
}

void track_cut(struct track *t)
{
	char c = 1;
	ssize_t w = write(t->cut[1], &c, 1);
	(void)w;
}

void track_close(struct track *t)
{
	track_cut(t);
	pthread_join(t->dl, NULL);

	pthread_mutex_lock(&t->lock);
	t->stopping = 1;
	pthread_cond_broadcast(&t->more);
	pthread_mutex_unlock(&t->lock);
	stop_ffmpeg(t->pid);            /* a blocked feeder gets EPIPE */
	pthread_join(t->feed, NULL);
	if (t->out >= 0) close(t->out);

	close_pair(t->cut);
	close_pair(t->wake);
	pthread_cond_destroy(&t->more);
	pthread_mutex_destroy(&t->lock);
	ogg_index_free(&t->ix);
	free(t->buf);
	free(t);
}
