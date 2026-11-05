/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The list parsers, offline: context-resolve JSON, the rootlist in
 * protobuf wire format (written by hand here), and an extended-metadata
 * batch (packed with the same generated code that parses it). */
#include "library.h"
#include "pb.h"
#include "proto/extended_metadata.pb-c.h"
#include "proto/metadata.pb-c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, why) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, why); return 1; } } while (0)

/* ------------------------------------------------------------------ a tiny writer */

struct w { uint8_t b[4096]; size_t n; };

static void w_varint(struct w *w, uint64_t v)
{
	do {
		uint8_t c = v & 0x7f;
		v >>= 7;
		w->b[w->n++] = (uint8_t)(c | (v ? 0x80 : 0));
	} while (v);
}

static void w_key(struct w *w, uint32_t num, int wire) { w_varint(w, (uint64_t)num << 3 | (unsigned)wire); }

static void w_bytes(struct w *w, uint32_t num, const void *p, size_t len)
{
	w_key(w, num, 2);
	w_varint(w, len);
	memcpy(w->b + w->n, p, len);
	w->n += len;
}

static void w_str(struct w *w, uint32_t num, const char *s) { w_bytes(w, num, s, strlen(s)); }
static void w_msg(struct w *w, uint32_t num, const struct w *m) { w_bytes(w, num, m->b, m->n); }

/* Item { uri } and MetaItem { attributes { name }, length, owner } */
static void add_list(struct w *items, const char *uri, const char *name, const char *owner)
{
	struct w it = { {0}, 0 }, meta = { {0}, 0 }, attrs = { {0}, 0 };
	w_str(&it, 1, uri);
	w_msg(items, 3, &it);
	w_str(&attrs, 1, name);
	w_msg(&meta, 2, &attrs);
	w_key(&meta, 3, 0);
	w_varint(&meta, 42);
	if (owner) w_str(&meta, 5, owner);
	w_msg(items, 4, &meta);
}

struct seen { int n; char uri[8][64], name[8][64], owner[8][64]; };

static void on_list(void *ctx, const char *uri, const char *name, const char *owner)
{
	struct seen *s = ctx;
	if (s->n == 8) return;
	snprintf(s->uri[s->n], 64, "%s", uri);
	snprintf(s->name[s->n], 64, "%s", name);
	snprintf(s->owner[s->n], 64, "%s", owner);
	s->n++;
}

/* ------------------------------------------------------------------ cases */

static int context(void)
{
	struct lib_tracks t = { NULL, 0, 0 };
	struct lib_pages more = { NULL, 0, 0 };

	CHECK(lib_parse_context(
	          "{\"uri\":\"spotify:playlist:p\",\"metadata\":{\"context_description\":\"Mix\"},\"pages\":["
	          "{\"tracks\":[{\"uri\":\"spotify:track:aaaaaaaaaaaaaaaaaaaaaa\",\"uid\":\"1\"},"
	          "{\"uri\":\"spotify:episode:eeeeeeeeeeeeeeeeeeeeee\"},"
	          "{\"uri\":\"spotify:local:Artist:Album:Song:180\"},"
	          "{\"uid\":\"no uri\"},"
	          "{\"uri\":\"spotify:track:bbbbbbbbbbbbbbbbbbbbbb\"}]},"
	          "{\"page_url\":\"hm://context-resolve/v1/page/xyz/1\"},"
	          "{\"tracks\":[{\"uri\":\"spotify:track:cccccccccccccccccccccc\"}],"
	          "\"next_page_url\":\"hm://context-resolve/v1/page/xyz?from=100\"}]}",
	          &t, &more) == 0, "a context");
	CHECK(t.n == 3 && !strcmp(t.v[0].uri, "spotify:track:aaaaaaaaaaaaaaaaaaaaaa") &&
	      !strcmp(t.v[2].uri, "spotify:track:cccccccccccccccccccccc"), "only tracks, across pages, in order");
	CHECK(more.n == 2 && !strcmp(more.path[0], "context-resolve/v1/page/xyz/1") &&
	      !strcmp(more.path[1], "context-resolve/v1/page/xyz?from=100"),
	      "a track-less page's page_url and the next page, as spclient paths, in order");
	lib_pages_free(&more);

	CHECK(lib_parse_context("{\"tracks\":[{\"uri\":\"spotify:track:dddddddddddddddddddddd\"}],"
	                        "\"nextPageUrl\":\"hm://context-resolve/v1/page/xyz?from=200\"}",
	                        &t, &more) == 0 && t.n == 4, "a later page on its own, camelCase");
	CHECK(more.n == 1 && !strcmp(more.path[0], "context-resolve/v1/page/xyz?from=200"), "camelCase next");
	lib_pages_free(&more);
	CHECK(lib_parse_context("{\"pages\":[{\"tracks\":[],\"page_url\":\"https://elsewhere\"}]}", &t, &more) == 0 &&
	      !more.n && t.n == 4, "the last page; a link that is not hm:// is not followed");
	CHECK(lib_parse_context("not json", &t, &more) != 0, "not JSON");
	CHECK(lib_parse_context("{\"error\":1}", &t, &more) != 0, "not a context");
	lib_pages_free(&more);
	lib_tracks_free(&t);
	return 0;
}

static int rootlist(void)
{
	struct w items = { {0}, 0 }, top = { {0}, 0 };
	add_list(&items, "spotify:playlist:one", "Morning", "jer");
	add_list(&items, "spotify:start-group:abc:Folder", "Folder", NULL);
	add_list(&items, "spotify:playlist:two", "Night", "someone");
	add_list(&items, "spotify:end-group:abc", "", NULL);
	w_key(&items, 1, 0);           /* ListItems.pos */
	w_varint(&items, 0);
	w_str(&top, 1, "revision");    /* SelectedListContent.revision */
	w_key(&top, 2, 0);             /* .length */
	w_varint(&top, 4);
	w_msg(&top, 5, &items);        /* .contents */
	w_key(&top, 15, 0);            /* .timestamp */
	w_varint(&top, 1700000000000ull);

	struct seen s;
	int total;
	memset(&s, 0, sizeof(s));
	CHECK(lib_parse_rootlist(top.b, top.n, on_list, &s, &total) == 0, "parses");
	CHECK(total == 4, "the count");
	CHECK(s.n == 2, "playlists only, folders flattened");
	CHECK(!strcmp(s.uri[0], "spotify:playlist:one") && !strcmp(s.name[0], "Morning") && !strcmp(s.owner[0], "jer"),
	      "first, with its name and owner");
	CHECK(!strcmp(s.uri[1], "spotify:playlist:two") && !strcmp(s.name[1], "Night"), "names stay with their playlist");

	CHECK(lib_parse_rootlist(top.b, top.n - 3, on_list, &s, &total) != 0, "cut short: malformed");
	return 0;
}

static int batch(void)
{
	struct lib_tracks t = { NULL, 0, 0 };
	t.cap = 4;
	t.v = calloc(4, sizeof(*t.v));
	t.v[t.n++].uri = strdup("spotify:track:aaaaaaaaaaaaaaaaaaaaaa");
	t.v[t.n++].uri = strdup("spotify:track:bbbbbbbbbbbbbbbbbbbbbb");
	t.v[t.n++].uri = strdup("spotify:track:cccccccccccccccccccccc");

	Artist a1 = ARTIST__INIT, a2 = ARTIST__INIT;
	a1.name = (char *)"One";
	a2.name = (char *)"Two";
	Artist *as[] = { &a1, &a2 };
	Album al = ALBUM__INIT;
	al.name = (char *)"LP";
	Track tr = TRACK__INIT;
	tr.name = (char *)"Song B";
	tr.n_artist = 2;
	tr.artist = as;
	tr.album = &al;
	tr.has_duration = 1;
	tr.duration = 201000;
	size_t tl = track__get_packed_size(&tr);
	uint8_t *tb = malloc(tl);
	track__pack(&tr, tb);

	Google__Protobuf__Any any = GOOGLE__PROTOBUF__ANY__INIT;
	any.type_url = (char *)"type.googleapis.com/spotify.metadata.Track";
	any.value.data = tb;
	any.value.len = tl;
	Spotify__Extendedmetadata__EntityExtensionDataHeader ok = SPOTIFY__EXTENDEDMETADATA__ENTITY_EXTENSION_DATA_HEADER__INIT;
	Spotify__Extendedmetadata__EntityExtensionDataHeader gone = SPOTIFY__EXTENDEDMETADATA__ENTITY_EXTENSION_DATA_HEADER__INIT;
	ok.status_code = 200;
	gone.status_code = 404;
	Spotify__Extendedmetadata__EntityExtensionData d1 = SPOTIFY__EXTENDEDMETADATA__ENTITY_EXTENSION_DATA__INIT;
	Spotify__Extendedmetadata__EntityExtensionData d2 = SPOTIFY__EXTENDEDMETADATA__ENTITY_EXTENSION_DATA__INIT;
	d1.header = &ok;
	d1.entity_uri = (char *)"spotify:track:bbbbbbbbbbbbbbbbbbbbbb";
	d1.extension_data = &any;
	d2.header = &gone;
	d2.entity_uri = (char *)"spotify:track:cccccccccccccccccccccc";
	d2.extension_data = &any;
	Spotify__Extendedmetadata__EntityExtensionData *ds[] = { &d1, &d2 };
	Spotify__Extendedmetadata__EntityExtensionDataArray arr = SPOTIFY__EXTENDEDMETADATA__ENTITY_EXTENSION_DATA_ARRAY__INIT;
	arr.n_extension_data = 2;
	arr.extension_data = ds;
	Spotify__Extendedmetadata__EntityExtensionDataArray *arrs[] = { &arr };
	Spotify__Extendedmetadata__BatchedExtensionResponse res = SPOTIFY__EXTENDEDMETADATA__BATCHED_EXTENSION_RESPONSE__INIT;
	res.n_extended_metadata = 1;
	res.extended_metadata = arrs;

	size_t len = spotify__extendedmetadata__batched_extension_response__get_packed_size(&res);
	uint8_t *body = malloc(len);
	spotify__extendedmetadata__batched_extension_response__pack(&res, body);

	CHECK(lib_parse_batch(body, len, LIB_TRACK, &t) == 0, "parses");
	CHECK(!t.v[0].title, "no entry: untitled");
	CHECK(t.v[1].title && !strcmp(t.v[1].title, "Song B") && !strcmp(t.v[1].artist, "One, Two") &&
	      !strcmp(t.v[1].album, "LP") && t.v[1].duration_ms == 201000, "titled from its Track");
	CHECK(!t.v[2].title, "a 404 entry is skipped");
	static const uint8_t junk[] = { 0x0a, 0xff };
	CHECK(lib_parse_batch(junk, sizeof(junk), LIB_TRACK, &t) != 0, "junk");

	/* The same reply read as albums: an Album's name and artists. */
	Album named = ALBUM__INIT;
	named.name = (char *)"Fever Daydream";
	named.n_artist = 1;
	named.artist = as;
	size_t alen = album__get_packed_size(&named);
	uint8_t *ab = malloc(alen);
	album__pack(&named, ab);
	any.value.data = ab;
	any.value.len = alen;
	d1.entity_uri = (char *)"spotify:album:aaaaaaaaaaaaaaaaaaaaaa";
	len = spotify__extendedmetadata__batched_extension_response__get_packed_size(&res);
	body = realloc(body, len);
	spotify__extendedmetadata__batched_extension_response__pack(&res, body);
	struct lib_tracks al_t = { NULL, 0, 0 };
	al_t.cap = 1;
	al_t.v = calloc(1, sizeof(*al_t.v));
	al_t.v[al_t.n++].uri = strdup("spotify:album:aaaaaaaaaaaaaaaaaaaaaa");
	CHECK(lib_parse_batch(body, len, LIB_ALBUM, &al_t) == 0 && al_t.v[0].title &&
	      !strcmp(al_t.v[0].title, "Fever Daydream") && !strcmp(al_t.v[0].artist, "One"), "an album named");
	lib_tracks_free(&al_t);
	free(ab);

	free(body);
	free(tb);
	lib_tracks_free(&t);
	return 0;
}

static int collection(void)
{
	/* PageResponse { items: CollectionItem { uri, added_at, is_removed } ..., next_page_token } */
	struct w page = { {0}, 0 }, a = { {0}, 0 }, b = { {0}, 0 }, c = { {0}, 0 }, d = { {0}, 0 };
	w_str(&a, 1, "spotify:track:aaaaaaaaaaaaaaaaaaaaaa");
	w_key(&a, 2, 0); w_varint(&a, 1700000000);
	w_str(&b, 1, "spotify:album:bbbbbbbbbbbbbbbbbbbbbb");
	w_str(&c, 1, "spotify:album:cccccccccccccccccccccc");
	w_key(&c, 3, 0); w_varint(&c, 1);           /* removed */
	w_str(&d, 1, "spotify:album:dddddddddddddddddddddd");
	w_msg(&page, 1, &a);
	w_msg(&page, 1, &b);
	w_msg(&page, 1, &c);
	w_msg(&page, 1, &d);
	w_str(&page, 2, "token-2");
	w_str(&page, 3, "sync");

	struct lib_tracks t = { NULL, 0, 0 };
	char next[64];
	CHECK(lib_parse_collection(page.b, page.n, "spotify:album:", &t, next, sizeof(next)) == 0, "parses");
	CHECK(t.n == 2 && !strcmp(t.v[0].uri, "spotify:album:bbbbbbbbbbbbbbbbbbbbbb") &&
	      !strcmp(t.v[1].uri, "spotify:album:dddddddddddddddddddddd"), "albums only, removed ones left out, in order");
	CHECK(!strcmp(next, "token-2"), "the next page's token");
	lib_tracks_free(&t);
	CHECK(lib_parse_collection(page.b, a.n + 1, "spotify:album:", &t, next, sizeof(next)) != 0, "cut short");
	lib_tracks_free(&t);
	CHECK(lib_parse_collection(page.b, 0, "spotify:album:", &t, next, sizeof(next)) == 0 && !t.n && !next[0],
	      "empty: the last page");

	/* The request side: what pbw writes, pb reads back. */
	struct pbw rq;
	struct pb rd;
	struct pb_field f;
	char s1[32], s2[32];
	memset(&rq, 0, sizeof(rq));
	pbw_str(&rq, 1, "jer");
	pbw_str(&rq, 2, "artist");
	pbw_str(&rq, 3, "");          /* empty: left out */
	pbw_varint(&rq, 4, 300);
	rd = pb_of(rq.b, rq.n);
	CHECK(pb_next(&rd, &f) == 1 && f.num == 1, "username");
	pb_str(&f, s1, sizeof(s1));
	CHECK(pb_next(&rd, &f) == 1 && f.num == 2, "set");
	pb_str(&f, s2, sizeof(s2));
	CHECK(!strcmp(s1, "jer") && !strcmp(s2, "artist"), "strings round trip");
	CHECK(pb_next(&rd, &f) == 1 && f.num == 4 && f.v == 300 && pb_next(&rd, &f) == 0 && !rq.over, "limit, no token");
	return 0;
}

static int search(void)
{
	char u[128];
	lib_search_uri("daft punk", u, sizeof(u));
	CHECK(!strcmp(u, "spotify:search:daft+punk"), "spaces as +");
	lib_search_uri("AC/DC & co.~_-", u, sizeof(u));
	CHECK(!strcmp(u, "spotify:search:AC%2FDC+%26+co.~_-"), "reserved characters encoded");
	lib_search_uri("Bj\xc3\xb6rk", u, sizeof(u));
	CHECK(!strcmp(u, "spotify:search:Bj%C3%B6rk"), "UTF-8 byte by byte");
	CHECK(library_is_list("spotify:search:x") && !library_is_list("spotify:search:") &&
	      !library_is_list("spotify:track:x") && library_is_list("spotify:collection:artists"), "which URIs are lists");
	return 0;
}

static int radio(void)
{
	struct lib_tracks t = { NULL, 0, 0 };
	/* A station as it might come back: tracks wherever they sit, other
	 * kinds of uri and duplicates included; the reader keeps every track. */
	CHECK(lib_parse_uris("{\"title\":\"Radio\",\"uri\":\"spotify:station:track:x\","
	                     "\"tracks\":[{\"uri\":\"spotify:track:aaaaaaaaaaaaaaaaaaaaaa\",\"artists\":[{\"uri\":\"spotify:artist:z\"}]},"
	                     "{\"metadata\":{\"x\":1},\"uri\":\"spotify:track:bbbbbbbbbbbbbbbbbbbbbb\"}],"
	                     "\"nextPage\":{\"items\":[{\"uri\":\"spotify:track:aaaaaaaaaaaaaaaaaaaaaa\"}]}}", &t) == 0,
	      "parses");
	CHECK(t.n == 3 && !strcmp(t.v[0].uri, "spotify:track:aaaaaaaaaaaaaaaaaaaaaa") &&
	      !strcmp(t.v[1].uri, "spotify:track:bbbbbbbbbbbbbbbbbbbbbb"), "tracks only, wherever they are, in order");
	CHECK(lib_parse_uris("[", &t) != 0, "not JSON");
	lib_tracks_free(&t);

	CHECK(!library_is_list("spotify:radio:spotify:track:aaaaaaaaaaaaaaaaaaaaaa"), "radio is an op, not a list");
	char w[256];
	lib_words_from_uri("/home/u/Music/youtube/666/Carpenter_Brut-Leather_Terror-01-Opening_Title.mp3", w, sizeof(w));
	CHECK(!strcmp(w, "Carpenter Brut Leather Terror Opening Title"), "file name to words");
	lib_words_from_uri("/m/03. Song (Live) [2019].flac", w, sizeof(w));
	CHECK(!strcmp(w, "Song Live"), "numbers and brackets dropped");
	lib_words_from_uri("/m/.flac", w, sizeof(w));
	CHECK(!w[0] || !strcmp(w, "flac"), "nothing much to go on");
	for (int i = 0; i < 30; i++) { /* more than it keeps: no overflow */
		char u[64];
		snprintf(u, sizeof(u), "spotify:track:%022d", i);
		lib_recent_add(u);
	}
	lib_recent_add("spotify:track:this-one-is-far-too-long-to-be-a-track-id-at-all-really");
	return 0;
}

static int wire(void)
{
	static const uint8_t bad_len[] = { 0x0a, 0x05, 'a' };       /* length past the end */
	static const uint8_t bad_var[] = { 0x08, 0xff, 0xff };       /* varint never ends */
	static const uint8_t group[] = { 0x0b };                     /* start group */
	struct pb r;
	struct pb_field f;
	r = pb_of(bad_len, sizeof(bad_len));
	CHECK(pb_next(&r, &f) < 0, "length past the end");
	r = pb_of(bad_var, sizeof(bad_var));
	CHECK(pb_next(&r, &f) < 0, "unterminated varint");
	r = pb_of(group, sizeof(group));
	CHECK(pb_next(&r, &f) < 0, "groups refused");
	r = pb_of(bad_len, 0);
	CHECK(pb_next(&r, &f) == 0, "empty is the end");
	return 0;
}

int main(void)
{
	if (context() || rootlist() || batch() || collection() || search() || radio() || wire()) return 1;
	printf("library: all checks passed\n");
	return 0;
}
