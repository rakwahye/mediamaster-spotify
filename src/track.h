/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file track.h
 * @brief One playing track: an Ogg Vorbis download kept in memory and
 * decoded to host PCM by a spawned ffmpeg.
 *
 *   fd ──download──▶ memory (+ Ogg page index) ──feed──▶ ffmpeg ──▶ read()
 *
 * Nothing here knows about Spotify: the bytes come from any readable fd
 * (nonblocking is fine), and the owner learns through `release` when the
 * fd is no longer used. A `struct track *` is the host's stream handle.
 *
 * The length is exact once the download completes; it is then reported
 * through host->stream_update, but not before the host's first read(),
 * so the host has the handle in place when the report comes. */

#ifndef SPOTIFY_TRACK_H
#define SPOTIFY_TRACK_H

#include "mm_plugin.h"

struct track;

struct track_src {
	int fd;      /* read until `size` bytes, 0 (EOF) or an error */
	size_t size; /* bytes the download delivers, 0 if unknown: either
	              * reaching it or EOF completes the download */
	/** Called once, from the download thread, when the fd is done with:
	 * at the end, on error, after track_cut() or during track_close().
	 * The callee closes fd. */
	void (*release)(void *arg, int fd, int complete);
	void *arg;
};

/** Start downloading and decoding. `name` is for log lines; `ffmpeg` the
 * binary to run. On failure nothing is released: the caller still owns
 * src->fd. @returns the track, or NULL with the reason logged. */
struct track *track_open(const struct mm_host *h, const char *name, const char *ffmpeg,
                         const struct track_src *src);

/** mm_source_ops semantics. */
long track_read(struct track *t, float *dst, uint32_t frames);
void track_interrupt(struct track *t);
void track_close(struct track *t);

/** Stop downloading now (another track needs the connection). What has
 * arrived still plays. Returns at once; `release` follows. Any thread. */
void track_cut(struct track *t);

#endif
