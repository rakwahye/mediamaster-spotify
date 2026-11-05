/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Throwaway measurement: is librespot-c good enough for mediamaster?
 *
 *   spike <librespot credentials.json> spotify:track:<id>
 *   spike --local <file.ogg>     same analysis, seeks simulated by slicing:
 *                                proves the Ogg method without Spotify
 *
 * 1. log in with librespot's stored credentials
 * 2. download the whole track once: time to first byte, throughput,
 *    Ogg structure, and a full decode through ffmpeg as ground truth
 * 3. seek at 10/50/90 %: librespot-c only seeks before playback starts,
 *    so each seek is close + open + seek(byte) + play. Measures latency,
 *    checks the bytes match the full download, reads where the Ogg page
 *    granule says we landed, and checks ffmpeg decodes the Vorbis
 *    headers + mid-stream data.
 *
 * Set SPIKE_DEBUG=1 for librespot-c's own log. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <json-c/json.h>
#include "librespot-c.h"

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ librespot-c callbacks */

static int tcp_connect(const char *address, unsigned short port)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_family = AF_UNSPEC }, *res, *ai;
	char p[8];
	snprintf(p, sizeof(p), "%hu", port);
	if (getaddrinfo(address, p, &hints, &res) != 0) return -1;
	int fd = -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0) continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static void tcp_disconnect(int fd) { if (fd >= 0) close(fd); }

static void logmsg(const char *fmt, ...)
{
	if (!getenv("SPIKE_DEBUG")) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void hexdump(const char *msg, uint8_t *data, size_t len) { (void)msg; (void)data; (void)len; }

static struct sp_callbacks callbacks = {
	.tcp_connect = tcp_connect,
	.tcp_disconnect = tcp_disconnect,
	.hexdump = hexdump,
	.logmsg = logmsg,
};

/* ------------------------------------------------------------------ credentials */

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

static size_t b64decode(const char *in, uint8_t *out, size_t cap)
{
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;
	for (; *in && *in != '='; in++) {
		int v = b64val((unsigned char)*in);
		if (v < 0) continue;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n < cap) out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return n;
}

static int load_credentials(const char *path, char *user, size_t ucap, uint8_t *blob, size_t *blen)
{
	json_object *o = json_object_from_file(path), *u, *t, *d;
	if (!o) {
		fprintf(stderr, "cannot read %s\n", path);
		return -1;
	}
	int rc = -1;
	if (!json_object_object_get_ex(o, "username", &u) || !json_object_object_get_ex(o, "auth_type", &t) ||
	    !(json_object_object_get_ex(o, "auth_data", &d) || json_object_object_get_ex(o, "encoded_auth_blob", &d))) {
		fprintf(stderr, "%s: expected username, auth_type, auth_data\n", path);
	} else if (json_object_get_int(t) != 1) {
		fprintf(stderr, "%s: auth_type %d, need 1 (stored credentials)\n", path, json_object_get_int(t));
	} else {
		snprintf(user, ucap, "%s", json_object_get_string(u));
		*blen = b64decode(json_object_get_string(d), blob, *blen);
		rc = *blen ? 0 : -1;
	}
	json_object_put(o);
	return rc;
}

/* ------------------------------------------------------------------ reading a librespot fd */

struct got {
	size_t n;
	double first; /* seconds from t0 to the first byte */
	int eof, err;
};

/* Read until `want` bytes, EOF, error or 20 s of silence. */
static struct got drain(int fd, uint8_t *buf, size_t want, double t0)
{
	struct got g = { 0, -1, 0, 0 };
	while (g.n < want) {
		struct pollfd pf = { fd, POLLIN, 0 };
		int pr = poll(&pf, 1, 20000);
		if (pr == 0) { g.err = ETIMEDOUT; break; }
		if (pr < 0) { if (errno == EINTR) continue; g.err = errno; break; }
		ssize_t r = read(fd, buf + g.n, want - g.n);
		if (r == 0) { g.eof = 1; break; }
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR) continue;
			g.err = errno;
			break;
		}
		if (g.first < 0) g.first = now() - t0;
		g.n += (size_t)r;
	}
	return g;
}

/* ------------------------------------------------------------------ Ogg */

static size_t page_len(const uint8_t *p, size_t avail)
{
	if (avail < 27 || memcmp(p, "OggS", 4) || p[4] != 0) return 0;
	size_t segs = p[26];
	if (avail < 27 + segs) return 0;
	size_t len = 27 + segs;
	for (size_t i = 0; i < segs; i++) len += p[27 + i];
	return len <= avail ? len : 0;
}

static int64_t granule(const uint8_t *p)
{
	uint64_t g = 0;
	for (int i = 7; i >= 0; i--) g = (g << 8) | p[6 + i];
	return (int64_t)g;
}

/* First complete page at or after `from`, or -1. */
static long find_page(const uint8_t *b, size_t n, size_t from)
{
	for (size_t i = from; i + 27 <= n; i++)
		if (b[i] == 'O' && page_len(b + i, n - i)) return (long)i;
	return -1;
}

/* ------------------------------------------------------------------ ffmpeg */

/* Decode a buffer (or two, concatenated) and return frames at 48 kHz, -1 on failure. */
static long decode_frames(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
	char path[] = "/tmp/spike-XXXXXX.ogg";
	int fd = mkstemps(path, 4);
	if (fd < 0) return -1;
	int ok = write(fd, a, an) == (ssize_t)an && (!bn || write(fd, b, bn) == (ssize_t)bn);
	close(fd);
	long frames = -1;
	if (ok) {
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "ffmpeg -nostdin -v error -i '%s' -f f32le -ar 48000 -ac 2 pipe:1", path);
		FILE *p = popen(cmd, "r");
		if (p) {
			char buf[65536];
			size_t n, total = 0;
			while ((n = fread(buf, 1, sizeof(buf), p)) > 0) total += n;
			frames = pclose(p) == 0 ? (long)(total / 8) : -1;
		}
	}
	unlink(path);
	return frames;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <librespot credentials.json> spotify:track:<id>\n"
		                "       %s --local <file.ogg>\n", argv[0], argv[0]);
		return 2;
	}
	const char *track = argv[2];
	int local = !strcmp(argv[1], "--local");
	struct sp_session *s = NULL;
	uint8_t *full;
	struct got g = { 0, 0, 1, 0 };
	double t0;
	int fd;
	if (local) {
		FILE *f = fopen(track, "rb");
		if (!f) { perror(track); return 1; }
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		rewind(f);
		full = malloc((size_t)len);
		g.n = fread(full, 1, (size_t)len, f);
		fclose(f);
		printf("local            %zu bytes from %s\n", g.n, track);
		goto analyse;
	}
	char user[128];
	uint8_t blob[512];
	size_t blen = sizeof(blob);
	if (load_credentials(argv[1], user, sizeof(user), blob, &blen) != 0) return 1;

	struct sp_sysinfo si;
	memset(&si, 0, sizeof(si));
	FILE *ur = fopen("/dev/urandom", "rb");
	unsigned char id[20] = { 0 };
	if (ur) { if (fread(id, 1, sizeof(id), ur) != sizeof(id)) { /* zeros are fine */ } fclose(ur); }
	for (int i = 0; i < 20; i++) sprintf(si.device_id + 2 * i, "%02x", id[i]);

	if (librespotc_init(&si, &callbacks) < 0) {
		fprintf(stderr, "init: %s\n", librespotc_last_errmsg());
		return 1;
	}
	t0 = now();
	s = librespotc_login_stored_cred(user, blob, blen);
	if (!s) {
		fprintf(stderr, "login: %s\n", librespotc_last_errmsg());
		return 1;
	}
	printf("login            %.2f s (stored credentials, user %.3s...)\n", now() - t0, user);
	librespotc_bitrate_set(s, SP_BITRATE_320);

	/* Whole track: ground truth. */
	t0 = now();
	fd = librespotc_open(track, s);
	if (fd < 0) {
		fprintf(stderr, "open: %s\n", librespotc_last_errmsg());
		return 1;
	}
	double t_open = now() - t0;
	struct sp_metadata md;
	if (librespotc_metadata_get(&md, fd) < 0 || !md.file_len) {
		fprintf(stderr, "metadata: %s\n", librespotc_last_errmsg());
		return 1;
	}
	full = malloc(md.file_len + 65536);
	librespotc_write(fd, NULL, NULL);
	g = drain(fd, full, md.file_len + 65536, t0);
	double t_all = now() - t0;
	librespotc_close(fd);
	printf("open             %.2f s, first byte %.2f s\n", t_open, g.first);
	printf("download         %zu of %zu bytes in %.2f s (%.0f kB/s)%s\n", g.n, md.file_len, t_all,
	       g.n / 1024.0 / t_all, g.eof ? "" : "  ** did not reach EOF **");
	if (g.err) printf("                 read error: %s\n", strerror(g.err));

analyse:;
	/* Ogg layout. */
	size_t n = g.n, off = 0, header_end = 0, pages = 0;
	int64_t last = 0;
	unsigned rate = 0;
	while (off < n) {
		size_t pl = page_len(full + off, n - off);
		if (!pl) break;
		int64_t gr = granule(full + off);
		if (pages == 0 && pl > 27 + full[26] + 16 && !memcmp(full + off + 27 + full[off + 26] + 1, "vorbis", 6)) {
			const uint8_t *pk = full + off + 27 + full[off + 26];
			rate = (unsigned)pk[12] | (unsigned)pk[13] << 8 | (unsigned)pk[14] << 16 | (unsigned)pk[15] << 24;
		}
		if (!header_end && gr > 0) header_end = off;
		if (gr > 0) last = gr;
		off += pl;
		pages++;
	}
	if (!rate || !header_end) {
		printf("ogg              ** not a Vorbis stream as expected (rate %u, header_end %zu) **\n", rate, header_end);
		return 1;
	}
	double dur = (double)last / rate;
	printf("ogg              %zu pages, %u Hz, headers end at byte %zu, duration %.3f s%s\n",
	       pages, rate, header_end, dur, off == n ? "" : "  ** trailing bytes **");
	long fr = decode_frames(full, n, NULL, 0);
	printf("decode whole     %ld frames at 48 kHz = %.3f s (%s)\n", fr, fr / 48000.0,
	       fr > 0 && (fr / 48000.0 - dur) < 0.1 && (dur - fr / 48000.0) < 0.1 ? "matches" : "** MISMATCH **");

	/* Seeks. */
	const double fracs[] = { 0.10, 0.50, 0.90 };
	int failures = 0;
	uint8_t *got = malloc(256 * 1024);
	for (size_t i = 0; i < sizeof(fracs) / sizeof(fracs[0]); i++) {
		double target = fracs[i] * dur;
		size_t pos = header_end + (size_t)(fracs[i] * (double)(n - header_end));
		t0 = now();
		double lat;
		if (local) {
			g.n = n - pos < 128 * 1024 ? n - pos : 128 * 1024;
			memcpy(got, full + pos, g.n);
			g.first = lat = 0;
		} else {
			fd = librespotc_open(track, s);
			if (fd < 0 || librespotc_seek(fd, pos) < 0) {
				printf("seek %5.1f s     ** %s **\n", target, librespotc_last_errmsg());
				failures++;
				if (fd >= 0) librespotc_close(fd);
				continue;
			}
			librespotc_write(fd, NULL, NULL);
			g = drain(fd, got, 128 * 1024, t0);
			lat = now() - t0;
			librespotc_close(fd);
		}

		int exact = g.n && pos + g.n <= n && !memcmp(got, full + pos, g.n);
		long pg = find_page(got, g.n, 0);
		double landed = -1;
		long dfr = -1;
		if (pg >= 0) {
			/* A page's granule is the sample at its end; the previous page's
			 * end is where its audio starts. Close enough: report the end. */
			landed = (double)granule(got + pg) / rate;
			dfr = decode_frames(full, header_end, got + pg, g.n - (size_t)pg);
		}
		int ok = exact && pg >= 0 && dfr > 0;
		failures += !ok;
		printf("seek %5.1f s     latency %.2f s (first byte %.2f s), bytes %s, landed %.3f s "
		       "(linear guess off by %+.3f s), headers+tail decode %s\n",
		       target, lat, g.first, exact ? "match" : "** DIFFER **", landed,
		       landed - target, dfr > 0 ? "ok" : "** FAILED **");
	}
	free(got);
	free(full);
	if (s) {
		librespotc_logout(s);
		librespotc_deinit();
	}
	printf("result           %s\n", failures ? "PROBLEMS (see **)" : "all good");
	return failures ? 1 : 0;
}
