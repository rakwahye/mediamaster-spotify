/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* credentials.json: librespot's format in, the same format out. */
#include "creds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, why) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, why); return 1; } } while (0)

static char dir[64], path[128];

static void put(const char *text)
{
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
}

int main(void)
{
	struct creds c, d;
	char err[256];
	struct stat st;

	snprintf(dir, sizeof(dir), "/tmp/mm-creds-test.%d", (int)getpid());
	mkdir(dir, 0700);
	snprintf(path, sizeof(path), "%s/credentials.json", dir);

	/* "AQID/w==" is 01 02 03 ff; "AQID_w" is the same in the URL-safe alphabet. */
	put("{\"username\":\"someone\",\"auth_type\":1,\"auth_data\":\"AQID/w==\"}");
	CHECK(creds_load(path, &c, err, sizeof(err)) == 0, "a librespot file loads");
	CHECK(!strcmp(c.username, "someone") && c.blob_len == 4 && !memcmp(c.blob, "\x01\x02\x03\xff", 4),
	      "username and blob decode");
	put("{\"username\":\"u\",\"auth_type\":1,\"encoded_auth_blob\":\"AQID_w\"}");
	CHECK(creds_load(path, &c, err, sizeof(err)) == 0 && c.blob_len == 4 && c.blob[3] == 0xff,
	      "the older key name and URL-safe base64 are accepted");

	put("{\"username\":\"u\",\"auth_type\":0,\"auth_data\":\"AQID\"}");
	CHECK(creds_load(path, &c, err, sizeof(err)) != 0 && strstr(err, "auth_type 0"), "a password file is refused by type");
	put("{\"username\":\"u\",\"auth_type\":1}");
	CHECK(creds_load(path, &c, err, sizeof(err)) != 0 && strstr(err, "not a librespot"), "missing auth_data is refused");
	put("{\"username\":\"u\",\"auth_type\":1,\"auth_data\":\"not base64!\"}");
	CHECK(creds_load(path, &c, err, sizeof(err)) != 0 && strstr(err, "base64"), "bad base64 is refused");
	put("not json");
	CHECK(creds_load(path, &c, err, sizeof(err)) != 0, "garbage is refused");
	unlink(path);
	CHECK(creds_load(path, &c, err, sizeof(err)) != 0 && strstr(err, "cannot read"), "a missing file is refused");

	memset(&c, 0, sizeof(c));
	snprintf(c.username, sizeof(c.username), "someone");
	for (size_t i = 0; i < 146; i++) c.blob[i] = (uint8_t)(i * 7 + 3); /* real blobs are 146 bytes */
	c.blob_len = 146;
	CHECK(creds_save(path, &c, err, sizeof(err)) == 0, "save");
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "saved file is 0600");
	char tmp[160];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	CHECK(access(tmp, F_OK) != 0, "no temporary file is left behind");
	CHECK(creds_load(path, &d, err, sizeof(err)) == 0 && !strcmp(d.username, c.username) &&
	      d.blob_len == c.blob_len && !memcmp(d.blob, c.blob, c.blob_len),
	      "what is saved loads back identically");

	snprintf(tmp, sizeof(tmp), "%s/nope/credentials.json", dir);
	CHECK(creds_save(tmp, &c, err, sizeof(err)) != 0 && err[0], "an unwritable path fails with a reason");

	unlink(path);
	rmdir(dir);
	printf("creds: all checks passed\n");
	return 0;
}
