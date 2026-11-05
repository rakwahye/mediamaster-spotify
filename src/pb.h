/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file pb.h
 * @brief Just enough protobuf wire format to read a few fields.
 *
 * For messages librespot-c has no generated code for (playlists): walk
 * the fields of one message, look at the ones wanted, skip the rest.
 * Wire format per protobuf's encoding docs. */

#ifndef SPOTIFY_PB_H
#define SPOTIFY_PB_H

#include <stddef.h>
#include <stdint.h>

struct pb {
	const uint8_t *p, *end;
};

struct pb_field {
	uint32_t num;
	int wire;            /* 0 varint, 1 64-bit, 2 length-delimited, 5 32-bit */
	uint64_t v;          /* varint, fixed64 or fixed32 value */
	const uint8_t *data; /* length-delimited: bytes, string or a message */
	size_t len;
};

static inline struct pb pb_of(const uint8_t *data, size_t len)
{
	struct pb r = { data, data + len };
	return r;
}

/** Next field into f. @returns 1, 0 at the end, -1 if malformed. */
int pb_next(struct pb *r, struct pb_field *f);

/** A length-delimited field as a NUL-terminated copy (truncated to fit). */
void pb_str(const struct pb_field *f, char *out, size_t cap);

#endif
