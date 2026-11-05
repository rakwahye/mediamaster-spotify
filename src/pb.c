/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file pb.c
 * @brief Protobuf wire format, read side. */

#include "pb.h"
#include <string.h>

static int varint(struct pb *r, uint64_t *out)
{
	uint64_t v = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		if (r->p >= r->end) return -1;
		uint8_t b = *r->p++;
		v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			*out = v;
			return 0;
		}
	}
	return -1;
}

static int fixed(struct pb *r, int bytes, uint64_t *out)
{
	if (r->end - r->p < bytes) return -1;
	uint64_t v = 0;
	for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | r->p[i];
	r->p += bytes;
	*out = v;
	return 0;
}

int pb_next(struct pb *r, struct pb_field *f)
{
	uint64_t key, len;
	if (r->p >= r->end) return 0;
	memset(f, 0, sizeof(*f));
	if (varint(r, &key) != 0 || (key >> 3) == 0 || (key >> 3) > UINT32_MAX) return -1;
	f->num = (uint32_t)(key >> 3);
	f->wire = (int)(key & 7);
	switch (f->wire) {
	case 0: return varint(r, &f->v) == 0 ? 1 : -1;
	case 1: return fixed(r, 8, &f->v) == 0 ? 1 : -1;
	case 5: return fixed(r, 4, &f->v) == 0 ? 1 : -1;
	case 2:
		if (varint(r, &len) != 0 || len > (uint64_t)(r->end - r->p)) return -1;
		f->data = r->p;
		f->len = (size_t)len;
		r->p += len;
		return 1;
	default:
		return -1; /* groups (3, 4) are long gone from what Spotify sends */
	}
}

void pb_str(const struct pb_field *f, char *out, size_t cap)
{
	size_t n = f->wire == 2 ? f->len : 0;
	if (n >= cap) n = cap - 1;
	memcpy(out, f->data, n);
	out[n] = '\0';
}

static void put_varint(struct pbw *w, uint64_t v)
{
	do {
		if (w->n == sizeof(w->b)) {
			w->over = 1;
			return;
		}
		uint8_t c = (uint8_t)(v & 0x7f);
		v >>= 7;
		w->b[w->n++] = (uint8_t)(c | (v ? 0x80 : 0));
	} while (v);
}

void pbw_varint(struct pbw *w, uint32_t num, uint64_t v)
{
	put_varint(w, (uint64_t)num << 3);
	put_varint(w, v);
}

void pbw_str(struct pbw *w, uint32_t num, const char *s)
{
	size_t len = strlen(s);
	if (!len) return;
	put_varint(w, (uint64_t)num << 3 | 2);
	put_varint(w, len);
	if (sizeof(w->b) - w->n < len) {
		w->over = 1;
		return;
	}
	memcpy(w->b + w->n, s, len);
	w->n += len;
}
