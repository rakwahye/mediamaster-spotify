/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file spclient.c
 * @brief GET against spclient with libcurl. The headers are the ones
 * librespot-c sends for its own spclient requests. */

#include "spclient.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BODY (8u << 20)
#define TIMEOUT_MS 5000L

static size_t collect(char *data, size_t size, size_t n, void *arg)
{
	struct spclient_reply *r = arg;
	size_t add = size * n;
	if (r->len + add > MAX_BODY) return 0; /* aborts the transfer */
	uint8_t *b = realloc(r->body, r->len + add + 1);
	if (!b) return 0;
	memcpy(b + r->len, data, add);
	r->body = b;
	r->len += add;
	r->body[r->len] = 0;
	return add;
}

int spclient_get(const struct spclient *c, const char *path, const char *accept,
                 struct spclient_reply *r, char *err, size_t errlen)
{
	char url[1024], h_accept[128], h_auth[600], h_client[600];
	struct curl_slist *hdr = NULL;
	memset(r, 0, sizeof(*r));

	CURL *curl = curl_easy_init();
	if (!curl) {
		snprintf(err, errlen, "curl unavailable");
		return -1;
	}
	snprintf(url, sizeof(url), "https://%s:%u/%s", c->host, (unsigned)c->port, path);
	snprintf(h_accept, sizeof(h_accept), "Accept: %s", accept);
	snprintf(h_auth, sizeof(h_auth), "Authorization: Bearer %s", c->access_token);
	snprintf(h_client, sizeof(h_client), "Client-Token: %s", c->client_token);
	hdr = curl_slist_append(hdr, h_accept);
	hdr = curl_slist_append(hdr, h_auth);
	hdr = curl_slist_append(hdr, h_client);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TIMEOUT_MS);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

	CURLcode rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->status);
	curl_slist_free_all(hdr);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK)
		snprintf(err, errlen, "%s: %s", path, curl_easy_strerror(rc));
	else if (r->status < 200 || r->status > 299)
		snprintf(err, errlen, "%s: HTTP %ld", path, r->status);
	else
		return 0;
	free(r->body);
	r->body = NULL;
	r->len = 0;
	return -1;
}
