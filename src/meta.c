/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file meta.c
 * @brief GET metadata/4/track/<gid>, a protobuf Track (librespot-c's
 * metadata.proto, from librespot, MIT), into tags. Cover art is the
 * largest image of the album's cover group, served from i.scdn.co. */

#include "meta.h"
#include "proto/metadata.pb-c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int meta_id_hex(const char *id, char hex[33])
{
	static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint8_t n[16] = { 0 }; /* big endian */

	if (strlen(id) != 22) return -1;
	for (const char *p = id; *p; p++) {
		const char *d = strchr(digits, *p);
		if (!d || !*p) return -1;
		unsigned carry = (unsigned)(d - digits);
		for (int i = 15; i >= 0; i--) {
			unsigned v = n[i] * 62u + carry;
			n[i] = (uint8_t)(v & 0xff);
			carry = v >> 8;
		}
		if (carry) return -1; /* more than 128 bits */
	}
	for (int i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", n[i]);
	return 0;
}

/* Building the kv list into m->buf. */
struct put { struct meta *m; size_t used; int pairs; };

static void put(struct put *p, const char *key, const char *val)
{
	size_t n = val ? strlen(val) + 1 : 0;
	if (!n || n == 1 || p->pairs == META_PAIRS || p->used + n > sizeof(p->m->buf)) return;
	char *v = p->m->buf + p->used;
	memcpy(v, val, n);
	p->used += n;
	p->m->kv[2 * p->pairs] = key;
	p->m->kv[2 * p->pairs + 1] = v;
	p->m->kv[2 * ++p->pairs] = NULL;
}

static void put_int(struct put *p, const char *key, int has, int32_t v)
{
	char s[16];
	if (!has || v <= 0) return;
	snprintf(s, sizeof(s), "%d", (int)v);
	put(p, key, s);
}

/* Width in pixels, or what the size class usually is. */
static int32_t image_width(const Image *im)
{
	static const int32_t by_size[] = { 300, 64, 640, 1280 }; /* DEFAULT SMALL LARGE XLARGE */
	if (im->has_width && im->width > 0) return im->width;
	return im->has_size && (unsigned)im->size < 4 ? by_size[im->size] : 300;
}

static const Image *largest(const ImageGroup *g)
{
	const Image *best = NULL;
	for (size_t i = 0; g && i < g->n_image; i++) {
		const Image *im = g->image[i];
		if (im->has_file_id && im->file_id.len && (!best || image_width(im) > image_width(best))) best = im;
	}
	return best;
}

int meta_from_track(const uint8_t *body, size_t len, struct meta *m)
{
	struct put p = { m, 0, 0 };
	m->kv[0] = NULL;

	Track *t = track__unpack(NULL, len, body);
	if (!t) return -1;
	put(&p, "title", t->name);
	for (size_t i = 0; i < t->n_artist; i++) put(&p, "artist", t->artist[i]->name);
	const Album *a = t->album;
	if (a) {
		put(&p, "album", a->name);
		for (size_t i = 0; i < a->n_artist; i++) put(&p, "albumartist", a->artist[i]->name);
	}
	put_int(&p, "tracknumber", t->has_number, t->number);
	put_int(&p, "discnumber", t->has_disc_number, t->disc_number);
	if (a && a->date && a->date->has_year) {
		char d[16];
		if (a->date->has_month && a->date->has_day)
			snprintf(d, sizeof(d), "%04d-%02d-%02d", (int)a->date->year, (int)a->date->month, (int)a->date->day);
		else
			snprintf(d, sizeof(d), "%04d", (int)a->date->year);
		put(&p, "date", d);
	}
	const Image *im = a ? largest(a->cover_group) : NULL;
	if (im) {
		char url[128];
		int o = snprintf(url, sizeof(url), "https://i.scdn.co/image/");
		for (size_t i = 0; i < im->file_id.len && o + 3 < (int)sizeof(url); i++)
			o += snprintf(url + o, sizeof(url) - (size_t)o, "%02x", im->file_id.data[i]);
		put(&p, "art", url);
	}
	track__free_unpacked(t, NULL);
	return 0;
}

int meta_track(const struct spclient *c, const char *uri, struct meta *m, char *err, size_t errlen)
{
	char hex[33], path[64];
	struct spclient_reply r;

	if (strncmp(uri, "spotify:track:", 14) != 0 || meta_id_hex(uri + 14, hex) != 0) {
		snprintf(err, errlen, "not a track id: %s", uri);
		return -1;
	}
	snprintf(path, sizeof(path), "metadata/4/track/%s", hex);
	if (spclient_get(c, path, "application/x-protobuf", &r, err, errlen) != 0) return -1;
	int rc = meta_from_track(r.body, r.len, m);
	free(r.body);
	if (rc != 0) snprintf(err, errlen, "%s: not a track", path);
	return rc;
}
