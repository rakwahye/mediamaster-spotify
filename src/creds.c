/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file creds.c
 * @brief librespot-format credentials.json, read and written with json-c. */

#include "creds.h"
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define AUTH_STORED_CREDENTIALS 1

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(int ch)
{
	const char *p = ch ? strchr(B64, ch) : NULL;
	if (p) return (int)(p - B64);
	if (ch == '-') return 62; /* URL-safe alphabet, accepted on read */
	if (ch == '_') return 63;
	return -1;
}

/* @returns decoded length, or 0 on bad input or overflow. */
static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;
	for (; *in && *in != '='; in++) {
		int v = b64_value((unsigned char)*in);
		if (v < 0) return 0;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n == cap) return 0;
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return n;
}

static void b64_encode(const uint8_t *in, size_t len, char *out)
{
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < len) v |= in[i + 2];
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? B64[v & 63] : '=';
	}
	out[o] = '\0';
}

int creds_load(const char *path, struct creds *c, char *err, size_t errlen)
{
	json_object *root, *user, *type, *data;
	int rc = -1;

	memset(c, 0, sizeof(*c));
	root = json_object_from_file(path);
	if (!root) {
		snprintf(err, errlen, "cannot read %s", path);
		return -1;
	}
	if (!json_object_object_get_ex(root, "username", &user) ||
	    !json_object_object_get_ex(root, "auth_type", &type) ||
	    !(json_object_object_get_ex(root, "auth_data", &data) ||
	      json_object_object_get_ex(root, "encoded_auth_blob", &data))) {
		snprintf(err, errlen, "%s: not a librespot credentials file", path);
	} else if (json_object_get_int(type) != AUTH_STORED_CREDENTIALS) {
		snprintf(err, errlen, "%s: auth_type %d, need %d (stored credentials)",
		         path, json_object_get_int(type), AUTH_STORED_CREDENTIALS);
	} else if (strlen(json_object_get_string(user)) >= sizeof(c->username)) {
		snprintf(err, errlen, "%s: username too long", path);
	} else if (!(c->blob_len = b64_decode(json_object_get_string(data), c->blob, sizeof(c->blob)))) {
		snprintf(err, errlen, "%s: auth_data is not valid base64", path);
	} else {
		snprintf(c->username, sizeof(c->username), "%s", json_object_get_string(user));
		rc = 0;
	}
	json_object_put(root);
	return rc;
}

int creds_save(const char *path, const struct creds *c, char *err, size_t errlen)
{
	char b64[CREDS_BLOB_MAX * 4 / 3 + 4], tmp[1100];
	json_object *root = json_object_new_object();
	int rc = -1, fd;

	b64_encode(c->blob, c->blob_len, b64);
	json_object_object_add(root, "username", json_object_new_string(c->username));
	json_object_object_add(root, "auth_type", json_object_new_int(AUTH_STORED_CREDENTIALS));
	json_object_object_add(root, "auth_data", json_object_new_string(b64));
	const char *text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
	} else {
		size_t len = strlen(text);
		int ok = write(fd, text, len) == (ssize_t)len && fsync(fd) == 0;
		ok = close(fd) == 0 && ok;
		if (ok && rename(tmp, path) == 0) rc = 0;
		else {
			snprintf(err, errlen, "%s: %s", path, strerror(errno));
			unlink(tmp);
		}
	}
	json_object_put(root);
	return rc;
}
