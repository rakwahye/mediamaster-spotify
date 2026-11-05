/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ogg.c
 * @brief Ogg page parsing, from RFC 3533; Vorbis identification header,
 * from the Vorbis I specification, section 4.2.2. */

#include "ogg.h"
#include <stdlib.h>
#include <string.h>

#define PAGE_HEADER 27 /* bytes before the segment table */

/* Length of the complete page at p, 0 if p is not a page or incomplete. */
static size_t page_len(const uint8_t *p, size_t avail)
{
	if (avail < PAGE_HEADER || memcmp(p, "OggS", 4) != 0 || p[4] != 0) return 0;
	size_t segs = p[26];
	if (avail < PAGE_HEADER + segs) return 0;
	size_t len = PAGE_HEADER + segs;
	for (size_t i = 0; i < segs; i++) len += p[PAGE_HEADER + i];
	return len <= avail ? len : 0;
}

static int64_t page_granule(const uint8_t *p)
{
	uint64_t g = 0;
	for (int i = 7; i >= 0; i--) g = (g << 8) | p[6 + i];
	return (int64_t)g;
}

/* "\x01vorbis", version, channels, then the rate as 32-bit little endian. */
static unsigned vorbis_rate(const uint8_t *page, size_t len)
{
	size_t body = PAGE_HEADER + page[26];
	const uint8_t *b = page + body;
	if (len < body + 16 || b[0] != 1 || memcmp(b + 1, "vorbis", 6) != 0) return 0;
	return (unsigned)b[12] | (unsigned)b[13] << 8 | (unsigned)b[14] << 16 | (unsigned)b[15] << 24;
}

void ogg_index_init(struct ogg_index *ix)
{
	memset(ix, 0, sizeof(*ix));
}

void ogg_index_free(struct ogg_index *ix)
{
	free(ix->pages);
	memset(ix, 0, sizeof(*ix));
}

int ogg_index_scan(struct ogg_index *ix, const uint8_t *buf, size_t have)
{
	size_t len;
	while ((len = page_len(buf + ix->scanned, have - ix->scanned)) != 0) {
		const uint8_t *p = buf + ix->scanned;
		int64_t g = page_granule(p);
		if (!ix->rate) ix->rate = vorbis_rate(p, len);
		if (g > 0) { /* header pages carry 0; -1 means no packet ends here */
			if (!ix->header_end) ix->header_end = ix->scanned;
			if (ix->npages == ix->cap) {
				size_t cap = ix->cap ? ix->cap * 2 : 1024;
				struct ogg_page *n = realloc(ix->pages, cap * sizeof(*n));
				if (!n) return -1;
				ix->pages = n;
				ix->cap = cap;
			}
			ix->pages[ix->npages].offset = ix->scanned;
			ix->pages[ix->npages].granule = g;
			ix->npages++;
		}
		ix->scanned += len;
	}
	return 0;
}

uint64_t ogg_index_duration_ms(const struct ogg_index *ix)
{
	if (!ix->rate || !ix->npages) return 0;
	return (uint64_t)ix->pages[ix->npages - 1].granule * 1000u / ix->rate;
}
