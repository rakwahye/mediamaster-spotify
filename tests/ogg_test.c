/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Ogg page index: rate, header end, pages and length, fed in pieces. */
#include "ogg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, why) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, why); return 1; } } while (0)

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	uint8_t *b = malloc((size_t)n + 64);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
	fclose(f);
	*len = (size_t)n;
	return b;
}

int main(int argc, char **argv)
{
	size_t len;
	uint8_t *buf = argc > 2 ? slurp(argv[1], &len) : NULL;
	CHECK(buf, "usage: ogg_test <file.ogg> <duration_ms>");
	uint64_t want = strtoull(argv[2], NULL, 10);

	/* All at once. */
	struct ogg_index a;
	ogg_index_init(&a);
	CHECK(ogg_index_scan(&a, buf, len) == 0, "scan");
	CHECK(a.rate == 44100, "rate from the identification header");
	CHECK(a.scanned == len, "every byte is a page");
	CHECK(a.header_end > 0 && a.npages > 1, "header pages, then audio pages");
	CHECK(a.pages[0].offset == a.header_end, "first audio page starts at header_end");
	CHECK(memcmp(buf + a.header_end, "OggS", 4) == 0, "header_end is a page");
	for (size_t i = 1; i < a.npages; i++)
		CHECK(a.pages[i].granule >= a.pages[i - 1].granule && a.pages[i].offset > a.pages[i - 1].offset,
		      "pages in order");
	CHECK(ogg_index_duration_ms(&a) == want, "length from the last granule");

	/* In odd pieces, as a download arrives: the same index. */
	struct ogg_index b;
	ogg_index_init(&b);
	CHECK(ogg_index_duration_ms(&b) == 0, "nothing known yet");
	for (size_t have = 0; have < len;) {
		have += 1 + (have * 7919) % 4093;
		if (have > len) have = len;
		CHECK(ogg_index_scan(&b, buf, have) == 0, "piecewise scan");
		CHECK(b.scanned <= have, "never past what arrived");
	}
	CHECK(b.npages == a.npages && b.header_end == a.header_end && b.rate == a.rate, "same index");
	CHECK(!memcmp(a.pages, b.pages, a.npages * sizeof(*a.pages)), "same pages");

	/* Trailing bytes that are not a page are left alone. */
	memset(buf + len, 0xA5, 64);
	CHECK(ogg_index_scan(&b, buf, len + 64) == 0 && b.scanned == len, "trailing junk ignored");
	CHECK(b.npages == a.npages, "no phantom pages");

	printf("ogg: ok (%zu audio pages, %llu ms)\n", a.npages, (unsigned long long)want);
	ogg_index_free(&a);
	ogg_index_free(&b);
	free(buf);
	return 0;
}
