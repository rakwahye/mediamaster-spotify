/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Track metadata → tags, offline: ids, and a Track message packed with
 * the same generated code that parses it. */
#include "meta.h"
#include "proto/metadata.pb-c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, why) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, why); return 1; } } while (0)

/* The n-th value of key in m (0-based), or NULL. */
static const char *tag(const struct meta *m, const char *key, int nth)
{
	for (const char *const *kv = m->kv; kv[0]; kv += 2)
		if (!strcmp(kv[0], key) && nth-- == 0) return kv[1];
	return NULL;
}

int main(void)
{
	char hex[33];

	/* Ids: checked against Python's arbitrary-precision arithmetic. */
	CHECK(meta_id_hex("4pNiQlCV92Bv25fvluKzKh", hex) == 0 && !strcmp(hex, "9118fb3c79a543c680ddd864116fe9d1"), "id");
	CHECK(meta_id_hex("4ZXymCRP1pyiPD5oFjWeyt", hex) == 0 && !strcmp(hex, "a4436c6da4fa4ee3a13c5ca86ad9fb51"), "id");
	CHECK(meta_id_hex("0000000000000000000000", hex) == 0 && !strcmp(hex, "00000000000000000000000000000000"), "zero");
	CHECK(meta_id_hex("zzzzzzzzzzzzzzzzzzzzzz", hex) != 0, "past 128 bits");
	CHECK(meta_id_hex("4pNiQlCV92Bv25fvluKzK", hex) != 0, "too short");
	CHECK(meta_id_hex("4pNiQlCV92Bv25fvluKzK!", hex) != 0, "not base62");

	/* A track as spclient sends it. */
	Artist a1 = ARTIST__INIT, a2 = ARTIST__INIT, aa = ARTIST__INIT;
	a1.name = (char *)"Kodomo";
	a2.name = (char *)"Guest";
	aa.name = (char *)"Kodomo";
	Artist *track_artists[] = { &a1, &a2 }, *album_artists[] = { &aa };

	uint8_t small_id[20], big_id[20];
	memset(small_id, 0x11, sizeof(small_id));
	memset(big_id, 0xab, sizeof(big_id));
	Image small = IMAGE__INIT, big = IMAGE__INIT, sized = IMAGE__INIT;
	small.has_file_id = 1; small.file_id.data = small_id; small.file_id.len = 20;
	small.has_width = 1; small.width = 64;
	big.has_file_id = 1; big.file_id.data = big_id; big.file_id.len = 20;
	big.has_width = 1; big.width = 640;
	sized.has_file_id = 1; sized.file_id.data = small_id; sized.file_id.len = 20;
	sized.has_size = 1; sized.size = IMAGE__SIZE__DEFAULT; /* no width: ~300 */
	Image *images[] = { &small, &big, &sized };
	ImageGroup cover = IMAGE_GROUP__INIT;
	cover.n_image = 3;
	cover.image = images;

	Date date = DATE__INIT;
	date.has_year = 1; date.year = 2014;
	date.has_month = 1; date.month = 3;
	date.has_day = 1; date.day = 7;

	Album album = ALBUM__INIT;
	album.name = (char *)"Still Life";
	album.n_artist = 1;
	album.artist = album_artists;
	album.date = &date;
	album.cover_group = &cover;

	Track t = TRACK__INIT;
	t.name = (char *)"Concept 15";
	t.n_artist = 2;
	t.artist = track_artists;
	t.album = &album;
	t.has_number = 1; t.number = 1;
	t.has_disc_number = 1; t.disc_number = 1;

	size_t len = track__get_packed_size(&t);
	uint8_t *buf = malloc(len);
	track__pack(&t, buf);

	struct meta m;
	CHECK(meta_from_track(buf, len, &m) == 0, "parses");
	CHECK(!strcmp(tag(&m, "title", 0), "Concept 15"), "title");
	CHECK(!strcmp(tag(&m, "artist", 0), "Kodomo") && !strcmp(tag(&m, "artist", 1), "Guest") && !tag(&m, "artist", 2),
	      "each artist, in order");
	CHECK(!strcmp(tag(&m, "album", 0), "Still Life") && !strcmp(tag(&m, "albumartist", 0), "Kodomo"), "album");
	CHECK(!strcmp(tag(&m, "tracknumber", 0), "1") && !strcmp(tag(&m, "discnumber", 0), "1"), "numbers");
	CHECK(!strcmp(tag(&m, "date", 0), "2014-03-07"), "date");
	CHECK(!strcmp(tag(&m, "art", 0), "https://i.scdn.co/image/abababababababababababababababababababab"),
	      "the widest cover");

	/* Only a year; no album at all; not a track. */
	date.has_month = date.has_day = 0;
	len = track__get_packed_size(&t);
	buf = realloc(buf, len);
	track__pack(&t, buf);
	CHECK(meta_from_track(buf, len, &m) == 0 && !strcmp(tag(&m, "date", 0), "2014"), "year only");

	t.album = NULL;
	t.has_number = 0;
	len = track__get_packed_size(&t);
	buf = realloc(buf, len);
	track__pack(&t, buf);
	CHECK(meta_from_track(buf, len, &m) == 0 && tag(&m, "title", 0) && !tag(&m, "album", 0) &&
	      !tag(&m, "art", 0) && !tag(&m, "tracknumber", 0), "a bare track");

	static const uint8_t junk[] = { 0xff, 0xff, 0xff, 0xff, 0x0f };
	CHECK(meta_from_track(junk, sizeof(junk), &m) != 0, "junk refused");

	free(buf);
	printf("meta: all checks passed\n");
	return 0;
}
