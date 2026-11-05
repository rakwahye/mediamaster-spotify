/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file track.h
 * @brief One playing track: an Ogg Vorbis download kept in memory and
 * decoded to host PCM by a spawned ffmpeg.
 *
 *   fd ──download──▶ memory (+ Ogg page index) ──feed──▶ ffmpeg ──▶ read()
 *
 * Nothing here knows about Spotify: the bytes come from any readable fd
 * (nonblocking is fine), handed back through `release` when no longer
 * used. A download that is cut short can resume from where it stopped.
 * A `struct track *` is the host's stream handle.
 *
 * The length is exact once the download completes; it is then reported,
 * with the track now seekable, through host->stream_update, but not
 * before the host's first read(), so the host has the handle in place
 * when the report comes. */

#ifndef SPOTIFY_TRACK_H
#define SPOTIFY_TRACK_H

#include "mm_plugin.h"

struct track;

/** Where the bytes come from. All calls come from the track's download
 * thread, except `cancel`, which comes from track_close. */
struct track_io {
	/** The fd is done with: at the end, on error, or when cut. The callee
	 * closes it. */
	void (*release)(void *arg, int fd, int complete);
	/** Optional. After a cut, block until the download may go on, then
	 * return an fd that delivers the bytes from offset `from`; -1 to
	 * give up (what has arrived still plays). */
	int (*resume)(void *arg, size_t from);
	/** Optional, with resume: make a blocked or later resume return -1. */
	void (*cancel)(void *arg);
	/** Optional: the last thing track_close does. */
	void (*free)(void *arg);
};

struct track_src {
	int fd;      /* read until `size` bytes, 0 (EOF) or an error */
	size_t size; /* bytes the download delivers, 0 if unknown: either
	              * reaching it or EOF completes the download */
	const struct track_io *io;
	void *arg;
};

/** Start downloading and decoding. `name` is for log lines; `ffmpeg` the
 * binary to run. On failure no callback is made: the caller still owns
 * src->fd and src->arg. @returns the track, or NULL with the reason
 * logged. */
struct track *track_open(const struct mm_host *h, const char *name, const char *ffmpeg,
                         const struct track_src *src);

/** mm_source_ops semantics. */
long track_read(struct track *t, float *dst, uint32_t frames);
/** To the sample, and the same samples as playing through: decoding
 * restarts from memory a little before `ms` and is trimmed to it. A
 * target past what has downloaded so far is reached when the bytes
 * arrive. @returns 0, or -1 before any audio page has arrived. */
int track_seek(struct track *t, uint64_t ms);
void track_interrupt(struct track *t);
void track_close(struct track *t);

/** Stop downloading now (another track needs the connection). What has
 * arrived still plays. Returns at once; `release` follows, then `resume`
 * if the source has it. Any thread. */
void track_cut(struct track *t);

#endif
