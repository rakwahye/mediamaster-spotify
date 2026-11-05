/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ogg.h
 * @brief Incremental Ogg Vorbis page index over a buffer that grows.
 *
 * Fed the bytes of a download as they arrive, it records the sample rate
 * (from the Vorbis identification header), where the header pages end,
 * and the byte offset and granule position of every audio page. The
 * granule of the last page is the stream's length in samples. */

#ifndef SPOTIFY_OGG_H
#define SPOTIFY_OGG_H

#include <stddef.h>
#include <stdint.h>

struct ogg_page {
	size_t offset;
	int64_t granule; /* samples at the end of the page */
};

struct ogg_index {
	unsigned rate;         /* 0 until the identification header is seen */
	size_t header_end;     /* offset of the first audio page, 0 until known */
	size_t scanned;        /* bytes consumed so far */
	struct ogg_page *pages;
	size_t npages, cap;
};

void ogg_index_init(struct ogg_index *ix);
void ogg_index_free(struct ogg_index *ix);

/** Index every complete page in buf[ix->scanned .. have). Call again as
 * `have` grows. Bytes that are not a page (padding after the last one)
 * are left unscanned. @returns 0, or -1 if out of memory. */
int ogg_index_scan(struct ogg_index *ix, const uint8_t *buf, size_t have);

/** Length so far, in ms; exact once the whole stream is scanned. */
uint64_t ogg_index_duration_ms(const struct ogg_index *ix);

#endif
