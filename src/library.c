/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file library.c
 * @brief Lists from spclient. Endpoint shapes as librespot (MIT) uses
 * them in core/src/spclient.rs; message fields from its
 * playlist4_external.proto, collection2v2.proto and context*.proto, read
 * here by hand. The collection's media type and set names ("collection"
 * holds Liked Songs and saved albums, "artist" followed artists) as
 * go-librespot documents them; facts only, no code taken. */

#include "library.h"
#include "pb.h"
#include "proto/extended_metadata.pb-c.h"
#include "proto/metadata.pb-c.h"
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BATCH 100        /* tracks per extended-metadata request */
#define MAX_PAGES 500    /* context pages followed */
#define ROOTLIST_PAGE 120
#define COLLECTION_PAGE 300
#define COLLECTION_TYPE "application/vnd.collection-v2.spotify.proto"
#define RECENT 20 /* tracks radio leaves out, and names to Spotify */

/* ------------------------------------------------------------------ tracks */

void lib_tracks_free(struct lib_tracks *t)
{
	for (size_t i = 0; i < t->n; i++) {
		free(t->v[i].uri);
		free(t->v[i].title);
		free(t->v[i].artist);
		free(t->v[i].album);
	}
	free(t->v);
	memset(t, 0, sizeof(*t));
}

static int tracks_add(struct lib_tracks *t, const char *uri)
{
	if (t->n == t->cap) {
		size_t cap = t->cap ? t->cap * 2 : 128;
		struct lib_track *v = realloc(t->v, cap * sizeof(*v));
		if (!v) return -1;
		t->v = v;
		t->cap = cap;
	}
	memset(&t->v[t->n], 0, sizeof(t->v[t->n]));
	if (!(t->v[t->n].uri = strdup(uri))) return -1;
	t->n++;
	return 0;
}

/* ------------------------------------------------------------------ context-resolve */

/* A key in snake_case, or the camelCase protobuf's JSON mapping uses. */
static json_object *member(json_object *o, const char *snake, const char *camel)
{
	json_object *v = NULL;
	if (json_object_object_get_ex(o, snake, &v)) return v;
	if (camel && json_object_object_get_ex(o, camel, &v)) return v;
	return NULL;
}

void lib_pages_free(struct lib_pages *p)
{
	for (size_t i = 0; i < p->n; i++) free(p->path[i]);
	free(p->path);
	memset(p, 0, sizeof(*p));
}

/* "hm://context-resolve/v1/..." → "context-resolve/v1/..." on spclient. */
static int link_to(json_object *page, const char *snake, const char *camel, struct lib_pages *more)
{
	json_object *u = member(page, snake, camel);
	const char *s = u ? json_object_get_string(u) : NULL;
	if (!s || strncmp(s, "hm://", 5) != 0 || !s[5]) return 0;
	if (more->n == more->cap) {
		size_t cap = more->cap ? more->cap * 2 : 16;
		char **v = realloc(more->path, cap * sizeof(*v));
		if (!v) return -1;
		more->path = v;
		more->cap = cap;
	}
	if (!(more->path[more->n] = strdup(s + 5))) return -1;
	more->n++;
	return 0;
}

static int page_tracks(json_object *page, struct lib_tracks *t)
{
	json_object *tracks = member(page, "tracks", NULL);
	size_t n = tracks && json_object_is_type(tracks, json_type_array) ? json_object_array_length(tracks) : 0;
	for (size_t i = 0; i < n; i++) {
		json_object *u = member(json_object_array_get_idx(tracks, i), "uri", NULL);
		const char *s = u ? json_object_get_string(u) : NULL;
		/* Episodes, local files and removed entries are not tracks we play. */
		if (s && !strncmp(s, "spotify:track:", 14) && tracks_add(t, s) != 0) return -1;
	}
	return 0;
}

/* A page's tracks, or, for a page that only names where they are, that. */
static int page(json_object *pg, struct lib_tracks *t, struct lib_pages *more)
{
	json_object *tracks = member(pg, "tracks", NULL);
	int rc;
	if (tracks && json_object_is_type(tracks, json_type_array) && json_object_array_length(tracks))
		rc = page_tracks(pg, t);
	else
		rc = link_to(pg, "page_url", "pageUrl", more);
	return rc ? rc : link_to(pg, "next_page_url", "nextPageUrl", more);
}

int lib_parse_context(const char *json, struct lib_tracks *t, struct lib_pages *more)
{
	json_object *root = json_tokener_parse(json);
	int rc = -1;
	if (!root || !json_object_is_type(root, json_type_object)) goto out;

	json_object *pages = member(root, "pages", NULL);
	if (pages && json_object_is_type(pages, json_type_array)) {
		size_t n = json_object_array_length(pages);
		rc = 0;
		for (size_t i = 0; i < n && rc == 0; i++) rc = page(json_object_array_get_idx(pages, i), t, more);
	} else if (member(root, "tracks", NULL)) { /* a later page on its own */
		rc = page(root, t, more);
	}
out:
	json_object_put(root);
	return rc;
}

/* The context, then each page it links to, depth-first in order: a
 * linked page's own links come before the ones after it. */
static int fetch_pages(const struct spclient *c, const char *path, struct lib_tracks *t, int *budget,
                       char *err, size_t errlen)
{
	struct spclient_reply r;
	struct lib_pages more = { NULL, 0, 0 };
	if (--*budget < 0) return 0;
	if (spclient_get(c, path, "application/json", &r, err, errlen) != 0) return -1;
	int rc = lib_parse_context((const char *)r.body, t, &more);
	free(r.body);
	if (rc != 0) snprintf(err, errlen, "%s: not a context", path);
	for (size_t i = 0; rc == 0 && i < more.n; i++) rc = fetch_pages(c, more.path[i], t, budget, err, errlen);
	lib_pages_free(&more);
	return rc;
}

static int context_tracks(const struct spclient *c, const char *uri, int budget, struct lib_tracks *t,
                          char *err, size_t errlen)
{
	char path[1024];
	snprintf(path, sizeof(path), "context-resolve/v1/%s", uri);
	int rc = fetch_pages(c, path, t, &budget, err, errlen);
	if (spclient_debug) spclient_debug("%s: %zu tracks", uri, t->n);
	return rc;
}

/* ------------------------------------------------------------------ rootlist */

#define MAX_LISTS 2000

struct rootpage {
	char uri[MAX_LISTS][96];
	char name[MAX_LISTS][128];
	char owner[MAX_LISTS][64];
	int n_items, n_meta;
};

/* MetaItem: attributes (2) → ListAttributes name (1); owner_username (5). */
static void meta_item(const struct pb_field *f, struct rootpage *rp)
{
	struct pb r = pb_of(f->data, f->len), a;
	struct pb_field g, h;
	int i = rp->n_meta++;
	if (i >= MAX_LISTS) return;
	while (pb_next(&r, &g) == 1) {
		if (g.num == 2 && g.wire == 2) {
			a = pb_of(g.data, g.len);
			while (pb_next(&a, &h) == 1)
				if (h.num == 1 && h.wire == 2) pb_str(&h, rp->name[i], sizeof(rp->name[i]));
		} else if (g.num == 5 && g.wire == 2) {
			pb_str(&g, rp->owner[i], sizeof(rp->owner[i]));
		}
	}
}

int lib_parse_rootlist(const uint8_t *body, size_t len,
                       void (*on_list)(void *ctx, const char *uri, const char *name, const char *owner),
                       void *ctx, int *total)
{
	struct rootpage *rp = calloc(1, sizeof(*rp));
	struct pb r = pb_of(body, len), li, it;
	struct pb_field f, g, h;
	int rc = 0, ok;
	if (!rp) return -1;
	*total = 0;

	/* SelectedListContent: length (2), contents (5) → ListItems:
	 * items (3) → Item uri (1); meta_items (4), one per item. */
	while (rc == 0 && (ok = pb_next(&r, &f)) != 0) {
		if (ok < 0) { rc = -1; break; }
		if (f.num == 2 && f.wire == 0) *total = (int)f.v;
		if (f.num != 5 || f.wire != 2) continue;
		li = pb_of(f.data, f.len);
		while ((ok = pb_next(&li, &g)) == 1) {
			if (g.num == 3 && g.wire == 2) {
				it = pb_of(g.data, g.len);
				while (pb_next(&it, &h) == 1)
					if (h.num == 1 && h.wire == 2 && rp->n_items < MAX_LISTS)
						pb_str(&h, rp->uri[rp->n_items], sizeof(rp->uri[0]));
				rp->n_items++;
			} else if (g.num == 4 && g.wire == 2) {
				meta_item(&g, rp);
			}
		}
		if (ok < 0) rc = -1;
	}
	for (int i = 0; rc == 0 && i < rp->n_items && i < MAX_LISTS; i++) {
		/* Folders arrive as start-group / end-group markers: flattened. */
		if (strncmp(rp->uri[i], "spotify:playlist:", 17) != 0) continue;
		on_list(ctx, rp->uri[i], i < rp->n_meta ? rp->name[i] : "", i < rp->n_meta ? rp->owner[i] : "");
	}
	free(rp);
	return rc;
}

struct emit_ctx { mm_item_fn emit; void *ctx; int stop; int count; };

static void emit_list(void *arg, const char *uri, const char *name, const char *owner)
{
	struct emit_ctx *e = arg;
	struct mm_item it = { MM_ITEM_LIST, uri, name[0] ? name : NULL, owner[0] ? owner : NULL, NULL, 0 };
	e->count++;
	if (!e->stop) e->stop = e->emit(e->ctx, &it) != 0;
}

static int rootlist(const struct spclient *c, const char *user, struct emit_ctx *e, char *err, size_t errlen)
{
	char path[512];
	int total = 0;
	for (int from = 0; !e->stop; from += ROOTLIST_PAGE) {
		struct spclient_reply r;
		snprintf(path, sizeof(path),
		         "playlist/v2/user/%s/rootlist?decorate=revision,attributes,length,owner,capabilities,status_code"
		         "&from=%d&length=%d", user, from, ROOTLIST_PAGE);
		if (spclient_get(c, path, "application/x-protobuf", &r, err, errlen) != 0) return -1;
		int before = e->count;
		int rc = lib_parse_rootlist(r.body, r.len, emit_list, e, &total);
		free(r.body);
		if (rc != 0) {
			snprintf(err, errlen, "your playlists: not a rootlist");
			return -1;
		}
		if (e->count == before || from + ROOTLIST_PAGE >= total) break;
	}
	return 0;
}

/* ------------------------------------------------------------------ collection */

int lib_parse_collection(const uint8_t *body, size_t len, const char *prefix, struct lib_tracks *t,
                         char *next, size_t nextcap)
{
	/* PageResponse: items (1) → CollectionItem uri (1), is_removed (3);
	 * next_page_token (2). */
	struct pb r = pb_of(body, len), it;
	struct pb_field f, g;
	int ok;
	next[0] = '\0';
	while ((ok = pb_next(&r, &f)) == 1) {
		if (f.num == 2 && f.wire == 2) {
			pb_str(&f, next, nextcap);
		} else if (f.num == 1 && f.wire == 2) {
			char uri[128] = "";
			int removed = 0;
			it = pb_of(f.data, f.len);
			while ((ok = pb_next(&it, &g)) == 1) {
				if (g.num == 1 && g.wire == 2) pb_str(&g, uri, sizeof(uri));
				else if (g.num == 3 && g.wire == 0) removed = g.v != 0;
			}
			if (ok < 0) return -1;
			if (!removed && !strncmp(uri, prefix, strlen(prefix)) && tracks_add(t, uri) != 0) return -1;
		}
	}
	return ok < 0 ? -1 : 0;
}

static int collection(const struct spclient *c, const char *user, const char *set, const char *prefix,
                      struct lib_tracks *t, char *err, size_t errlen)
{
	char token[512] = "";
	for (int page = 0; page < MAX_PAGES; page++) {
		struct pbw w;
		struct spclient_reply r;
		char next[512];
		memset(&w, 0, sizeof(w));
		pbw_str(&w, 1, user);
		pbw_str(&w, 2, set);
		pbw_str(&w, 3, token);
		pbw_varint(&w, 4, COLLECTION_PAGE);
		if (w.over) {
			snprintf(err, errlen, "collection request too long");
			return -1;
		}
		if (spclient_post(c, "collection/v2/paging", COLLECTION_TYPE, COLLECTION_TYPE, w.b, w.n, &r, err, errlen) != 0)
			return -1;
		int rc = lib_parse_collection(r.body, r.len, prefix, t, next, sizeof(next));
		free(r.body);
		if (rc != 0) {
			snprintf(err, errlen, "your %s: not a collection page", set);
			return -1;
		}
		if (!next[0] || !strcmp(next, token)) break;
		snprintf(token, sizeof(token), "%s", next);
	}
	if (spclient_debug) spclient_debug("collection %s: %zu %s", set, t->n, prefix);
	return 0;
}

/* ------------------------------------------------------------------ titles */

static char *join_artists(Artist **a, size_t n)
{
	size_t len = 1;
	for (size_t i = 0; i < n; i++) len += (a[i]->name ? strlen(a[i]->name) : 0) + 2;
	char *s = malloc(len), *p = s;
	if (!s) return NULL;
	*p = '\0';
	for (size_t i = 0; i < n; i++) {
		if (!a[i]->name || !*a[i]->name) continue;
		p += sprintf(p, "%s%s", p == s ? "" : ", ", a[i]->name);
	}
	return s;
}

static struct lib_track *find(struct lib_tracks *t, const char *uri, size_t *hint)
{
	for (size_t k = 0; k < t->n; k++) {
		size_t i = (*hint + k) % t->n; /* replies come in request order */
		if (!strcmp(t->v[i].uri, uri)) {
			*hint = i + 1;
			return &t->v[i];
		}
	}
	return NULL;
}

/* Fill e from one entity of `kind`. */
static void name_entry(struct lib_track *e, enum lib_kind kind, const ProtobufCBinaryData *v)
{
	if (kind == LIB_TRACK) {
		Track *tr = track__unpack(NULL, v->len, v->data);
		if (!tr) return;
		if (tr->name) e->title = strdup(tr->name);
		e->artist = join_artists(tr->artist, tr->n_artist);
		if (tr->album && tr->album->name) e->album = strdup(tr->album->name);
		if (tr->has_duration && tr->duration > 0) e->duration_ms = (uint64_t)tr->duration;
		track__free_unpacked(tr, NULL);
	} else if (kind == LIB_ALBUM) {
		Album *al = album__unpack(NULL, v->len, v->data);
		if (!al) return;
		if (al->name) e->title = strdup(al->name);
		e->artist = join_artists(al->artist, al->n_artist);
		album__free_unpacked(al, NULL);
	} else {
		Artist *ar = artist__unpack(NULL, v->len, v->data);
		if (!ar) return;
		if (ar->name) e->title = strdup(ar->name);
		artist__free_unpacked(ar, NULL);
	}
}

int lib_parse_batch(const uint8_t *body, size_t len, enum lib_kind kind, struct lib_tracks *t)
{
	Spotify__Extendedmetadata__BatchedExtensionResponse *res =
		spotify__extendedmetadata__batched_extension_response__unpack(NULL, len, body);
	size_t hint = 0;
	if (!res) return -1;
	for (size_t i = 0; i < res->n_extended_metadata; i++) {
		const Spotify__Extendedmetadata__EntityExtensionDataArray *arr = res->extended_metadata[i];
		for (size_t j = 0; j < arr->n_extension_data; j++) {
			const Spotify__Extendedmetadata__EntityExtensionData *d = arr->extension_data[j];
			struct lib_track *e;
			if (!d->entity_uri || !d->extension_data || (d->header && d->header->status_code != 200)) continue;
			if (!(e = find(t, d->entity_uri, &hint)) || e->title) continue;
			name_entry(e, kind, &d->extension_data->value);
		}
	}
	spotify__extendedmetadata__batched_extension_response__free_unpacked(res, NULL);
	return 0;
}

/* Names for t[from, from + n). A failure leaves them unnamed. */
static int titles(const struct spclient *c, enum lib_kind kind, struct lib_tracks *t, size_t from, size_t n,
                  char *err, size_t errlen)
{
	static const Spotify__Extendedmetadata__ExtensionKind kinds[] = {
		SPOTIFY__EXTENDEDMETADATA__EXTENSION_KIND__TRACK_V4,
		SPOTIFY__EXTENDEDMETADATA__EXTENSION_KIND__ALBUM_V4,
		SPOTIFY__EXTENDEDMETADATA__EXTENSION_KIND__ARTIST_V4,
	};
	Spotify__Extendedmetadata__ExtensionQuery q = SPOTIFY__EXTENDEDMETADATA__EXTENSION_QUERY__INIT;
	Spotify__Extendedmetadata__ExtensionQuery *qs[1] = { &q };
	Spotify__Extendedmetadata__EntityRequest er[BATCH];
	Spotify__Extendedmetadata__EntityRequest *ers[BATCH];
	Spotify__Extendedmetadata__BatchedEntityRequest req = SPOTIFY__EXTENDEDMETADATA__BATCHED_ENTITY_REQUEST__INIT;
	struct spclient_reply r;

	q.extension_kind = kinds[kind];
	for (size_t i = 0; i < n; i++) {
		Spotify__Extendedmetadata__EntityRequest init = SPOTIFY__EXTENDEDMETADATA__ENTITY_REQUEST__INIT;
		er[i] = init;
		er[i].entity_uri = t->v[from + i].uri;
		er[i].n_query = 1;
		er[i].query = qs;
		ers[i] = &er[i];
	}
	req.n_entity_request = n;
	req.entity_request = ers;

	size_t len = spotify__extendedmetadata__batched_entity_request__get_packed_size(&req);
	uint8_t *body = malloc(len);
	if (!body) return -1;
	spotify__extendedmetadata__batched_entity_request__pack(&req, body);
	int rc = spclient_post(c, "extended-metadata/v0/extended-metadata", "application/x-protobuf",
	                       "application/x-protobuf", body, len, &r, err, errlen);
	free(body);
	if (rc != 0) return -1;
	rc = lib_parse_batch(r.body, r.len, kind, t);
	free(r.body);
	return rc;
}

static int hand_over(const struct spclient *c, enum lib_kind kind, struct lib_tracks *t, uint32_t flags,
                     mm_item_fn emit, void *ctx);

/* ------------------------------------------------------------------ radio */

static struct {
	pthread_mutex_t lock;
	char uri[RECENT][64];
	int next, n;
} REC = { PTHREAD_MUTEX_INITIALIZER, { { 0 } }, 0, 0 };

void lib_recent_add(const char *uri)
{
	if (strlen(uri) >= sizeof(REC.uri[0])) return;
	pthread_mutex_lock(&REC.lock);
	snprintf(REC.uri[REC.next], sizeof(REC.uri[0]), "%s", uri);
	REC.next = (REC.next + 1) % RECENT;
	if (REC.n < RECENT) REC.n++;
	pthread_mutex_unlock(&REC.lock);
}

/* The recent tracks, oldest first, into out. @returns how many. */
static int recent(char out[RECENT][64])
{
	pthread_mutex_lock(&REC.lock);
	int n = REC.n;
	for (int i = 0; i < n; i++)
		memcpy(out[i], REC.uri[(REC.next - n + i + RECENT) % RECENT], sizeof(out[0]));
	pthread_mutex_unlock(&REC.lock);
	return n;
}

static void collect_uris(json_object *o, struct lib_tracks *t, int *oom)
{
	if (*oom) return;
	if (json_object_is_type(o, json_type_array)) {
		size_t n = json_object_array_length(o);
		for (size_t i = 0; i < n; i++) collect_uris(json_object_array_get_idx(o, i), t, oom);
	} else if (json_object_is_type(o, json_type_object)) {
		json_object_object_foreach(o, key, val) {
			if (!strcmp(key, "uri") && json_object_is_type(val, json_type_string)) {
				const char *s = json_object_get_string(val);
				if (!strncmp(s, "spotify:track:", 14) && tracks_add(t, s) != 0) *oom = 1;
			} else {
				collect_uris(val, t, oom);
			}
		}
	}
}

int lib_parse_uris(const char *json, struct lib_tracks *t)
{
	json_object *root = json_tokener_parse(json);
	int oom = 0;
	if (!root) return -1;
	collect_uris(root, t, &oom);
	json_object_put(root);
	return oom ? -1 : 0;
}

/* Autoplay for `seed`, as Spotify's apps ask when a context runs out:
 * an AutoplayContextRequest { context_uri, recent_track_uri... }. */
static int autoplay(const struct spclient *c, const char *seed, char rec[][64], int nrec, struct lib_tracks *t,
                    char *err, size_t errlen)
{
	struct pbw w;
	struct spclient_reply r;
	struct lib_pages more = { NULL, 0, 0 };
	memset(&w, 0, sizeof(w));
	pbw_str(&w, 1, seed);
	for (int i = 0; i < nrec; i++) pbw_str(&w, 2, rec[i]);
	if (w.over) {
		snprintf(err, errlen, "autoplay request too long");
		return -1;
	}
	if (spclient_post(c, "context-resolve/v1/autoplay", "application/json", "application/x-protobuf", w.b, w.n,
	                  &r, err, errlen) != 0)
		return -1;
	int rc = lib_parse_context((const char *)r.body, t, &more);
	free(r.body);
	lib_pages_free(&more); /* the first page is plenty */
	if (rc != 0) snprintf(err, errlen, "autoplay: not a context");
	return rc;
}

/* Failing that, the seed's radio station. */
static int station(const struct spclient *c, const char *seed, char rec[][64], int nrec, struct lib_tracks *t,
                   char *err, size_t errlen)
{
	char path[2048];
	struct spclient_reply r;
	int o = snprintf(path, sizeof(path), "radio-apollo/v3/stations/%s?autoplay=true&count=50", seed);
	for (int i = 0, first = 1; i < nrec && o < (int)sizeof(path) - 40; i++) {
		if (strncmp(rec[i], "spotify:track:", 14) != 0) continue;
		o += snprintf(path + o, sizeof(path) - (size_t)o, "%s%s", first ? "&prev_tracks=" : ",", rec[i] + 14);
		first = 0;
	}
	if (spclient_get(c, path, "application/json", &r, err, errlen) != 0) return -1;
	int rc = lib_parse_uris((const char *)r.body, t);
	free(r.body);
	if (rc != 0) snprintf(err, errlen, "radio station: not JSON");
	return rc;
}

static int radio(const struct spclient *c, const char *seed, uint32_t flags, mm_item_fn emit, void *ctx,
                 char *err, size_t errlen)
{
	char rec[RECENT][64];
	int nrec = recent(rec);
	struct lib_tracks t = { NULL, 0, 0 }, kept = { NULL, 0, 0 };
	char why[256] = "";

	if (autoplay(c, seed, rec, nrec, &t, why, sizeof(why)) != 0 || !t.n) {
		if (spclient_debug) spclient_debug("radio %s: no autoplay (%s), trying its station", seed, why);
		lib_tracks_free(&t);
		if (station(c, seed, rec, nrec, &t, err, errlen) != 0) {
			lib_tracks_free(&t);
			return -1;
		}
	}
	/* Leave out the seed, what was just played, and repeats. */
	for (size_t i = 0; i < t.n; i++) {
		int skip = !strcmp(t.v[i].uri, seed);
		for (int k = 0; k < nrec && !skip; k++) skip = !strcmp(t.v[i].uri, rec[k]);
		for (size_t k = 0; k < kept.n && !skip; k++) skip = !strcmp(t.v[i].uri, kept.v[k].uri);
		if (!skip && tracks_add(&kept, t.v[i].uri) != 0) break;
	}
	lib_tracks_free(&t);
	if (spclient_debug) spclient_debug("radio %s: %zu tracks", seed, kept.n);
	return hand_over(c, LIB_TRACK, &kept, flags, emit, ctx);
}

/* ------------------------------------------------------------------ list */

/* "daft punk!" → "spotify:search:daft+punk%21", as Spotify's apps write it. */
void lib_search_uri(const char *words, char *out, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = (size_t)snprintf(out, cap, "spotify:search:");
	for (const unsigned char *p = (const unsigned char *)words; *p && o + 4 < cap; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    *p == '-' || *p == '_' || *p == '.' || *p == '~')
			out[o++] = (char)*p;
		else if (*p == ' ')
			out[o++] = '+';
		else {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		}
	}
	out[o] = '\0';
}

/* Name t's entries if wanted, emit them, free them. */
static int hand_over(const struct spclient *c, enum lib_kind kind, struct lib_tracks *t, uint32_t flags,
                     mm_item_fn emit, void *ctx)
{
	if (!(flags & MM_LIST_URIS)) {
		char why[256];
		for (size_t i = 0; i < t->n; i += BATCH)
			if (titles(c, kind, t, i, t->n - i < BATCH ? t->n - i : BATCH, why, sizeof(why)) != 0) {
				if (spclient_debug) spclient_debug("no names: %s", why);
				break;
			}
	}
	for (size_t i = 0; i < t->n; i++) {
		const struct lib_track *k = &t->v[i];
		struct mm_item it = { kind == LIB_TRACK ? MM_ITEM_TRACK : MM_ITEM_LIST, k->uri, k->title, k->artist,
		                      k->album, k->duration_ms };
		if (emit(ctx, &it) != 0) break;
	}
	lib_tracks_free(t);
	return 0;
}

/* Your saved albums or followed artists, as lists. */
static int saved(const struct spclient *c, const char *user, const char *set, const char *prefix, enum lib_kind kind,
                 uint32_t flags, mm_item_fn emit, void *ctx, char *err, size_t errlen)
{
	struct lib_tracks t = { NULL, 0, 0 };
	if (collection(c, user, set, prefix, &t, err, errlen) != 0) {
		lib_tracks_free(&t);
		return -1;
	}
	return hand_over(c, kind, &t, flags, emit, ctx);
}

int library_is_list(const char *uri)
{
	static const char *const prefixes[] = {
		"spotify:playlist:", "spotify:album:", "spotify:artist:", "spotify:user:", "spotify:search:",
		"spotify:radio:", NULL
	};
	if (!strcmp(uri, "spotify:") || !strcmp(uri, "spotify:collection") ||
	    !strcmp(uri, "spotify:collection:albums") || !strcmp(uri, "spotify:collection:artists"))
		return 1;
	for (const char *const *p = prefixes; *p; p++)
		if (!strncmp(uri, *p, strlen(*p)) && uri[strlen(*p)]) return 1;
	return 0;
}

int library_list(const struct spclient *c, const char *user, const char *uri, uint32_t flags,
                 mm_item_fn emit, void *ctx, char *err, size_t errlen)
{
	char context[512];
	struct emit_ctx e = { emit, ctx, 0, 0 };

	if (!library_is_list(uri)) return MM_NOT_A_LIST;
	if (!strcmp(uri, "spotify:")) {
		static const char *const mine[][2] = {
			{ "spotify:collection", "Liked Songs" },
			{ "spotify:collection:albums", "Your Albums" },
			{ "spotify:collection:artists", "Your Artists" },
		};
		for (size_t i = 0; i < 3; i++) {
			struct mm_item it = { MM_ITEM_LIST, mine[i][0], mine[i][1], user, NULL, 0 };
			if (emit(ctx, &it) != 0) return 0;
		}
		return rootlist(c, user, &e, err, errlen);
	}
	if (!strcmp(uri, "spotify:collection:albums") || !strcmp(uri, "spotify:collection:artists")) {
		int albums = !strcmp(uri, "spotify:collection:albums");
		return saved(c, user, albums ? "collection" : "artist", albums ? "spotify:album:" : "spotify:artist:",
		             albums ? LIB_ALBUM : LIB_ARTIST, flags, emit, ctx, err, errlen);
	}
	if (!strncmp(uri, "spotify:radio:", 14)) return radio(c, uri + 14, flags, emit, ctx, err, errlen);
	int budget = MAX_PAGES;
	if (!strcmp(uri, "spotify:collection")) {
		snprintf(context, sizeof(context), "spotify:user:%s:collection", user);
	} else if (!strncmp(uri, "spotify:search:", 15)) {
		lib_search_uri(uri + 15, context, sizeof(context));
		budget = 1; /* the best matches; the rest is noise */
	} else {
		snprintf(context, sizeof(context), "%s", uri);
	}

	struct lib_tracks t = { NULL, 0, 0 };
	if (context_tracks(c, context, budget, &t, err, errlen) != 0) {
		lib_tracks_free(&t);
		return -1;
	}
	return hand_over(c, LIB_TRACK, &t, flags, emit, ctx);
}
