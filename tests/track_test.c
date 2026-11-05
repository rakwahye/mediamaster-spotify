/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* One track, fed from a pipe the way librespot-c feeds it: PCM out, the
 * length reported once known and the host is reading, the fd released
 * exactly once, and interrupt, cut and close mid-download. */
#define _GNU_SOURCE /* pipe2 */
#include "track.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, why) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, why); return 1; } } while (0)

/* ------------------------------------------------------------------ fake host */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int updates, released, released_complete, read_started;
static uint64_t updated_ms;
static void *updated_stream;

static void h_log(const struct mm_host *h, int level, const char *fmt, ...)
{
	va_list ap;
	(void)h;
	(void)level;
	if (!getenv("VERBOSE")) return; /* expected warnings; VERBOSE=1 shows them */
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void h_update(const struct mm_host *h, void *stream, const struct mm_stream_info *info)
{
	(void)h;
	pthread_mutex_lock(&lock);
	updates++;
	updated_ms = info->duration_ms;
	updated_stream = stream;
	if (!read_started) updates = 1000; /* too early: the host has no handle yet */
	pthread_mutex_unlock(&lock);
}

static struct mm_host host = {
	.abi = MM_ABI, .size = sizeof(struct mm_host), .log = h_log, .stream_update = h_update,
};

static void release(void *arg, int fd, int complete)
{
	(void)arg;
	close(fd);
	pthread_mutex_lock(&lock);
	released++;
	released_complete = complete;
	pthread_mutex_unlock(&lock);
}

static int frees;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_open, cancelled, resumes; /* the resumable source, below */
static size_t resumed_from;

static void count_free(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&lock);
	frees++;
	pthread_mutex_unlock(&lock);
}

static const struct track_io io_plain = { release, NULL, NULL, count_free };

/* A consistent copy of what the callbacks saw. */
struct seen { int updates, released, complete; uint64_t ms; void *stream; };

static struct seen seen(void)
{
	pthread_mutex_lock(&lock);
	struct seen v = { updates, released, released_complete, updated_ms, updated_stream };
	pthread_mutex_unlock(&lock);
	return v;
}

/* The download thread reports after it lets go of the decoder, so the
 * report can trail the last PCM by a moment. */
static struct seen settle(void)
{
	for (int i = 0; i < 200; i++) {
		struct seen v = seen();
		if (v.complete && v.updates) return v;
		usleep(5000);
	}
	return seen();
}

static void reset(void)
{
	pthread_mutex_lock(&lock);
	updates = released = released_complete = read_started = 0;
	frees = gate_open = cancelled = resumes = 0;
	resumed_from = 0;
	updated_ms = 0;
	updated_stream = NULL;
	pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ source */

static uint8_t *file;
static size_t file_len;

struct writer {
	int fd;
	size_t start;    /* first byte to send */
	size_t stop_at;  /* bytes to send before pausing forever (until closed) */
	long delay_us;   /* between 16 KiB chunks */
	int close_at_end;
	pthread_t th;
};

static void *write_file(void *arg)
{
	struct writer *w = arg;
	size_t pos = w->start;
	while (pos < w->stop_at) {
		size_t n = w->stop_at - pos < 16384 ? w->stop_at - pos : 16384;
		ssize_t r = write(w->fd, file + pos, n);
		if (r <= 0) break; /* reader went away */
		pos += (size_t)r;
		if (w->delay_us) {
			struct timespec ts = { 0, w->delay_us * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	if (w->close_at_end) close(w->fd);
	return NULL;
}

/* A pipe like librespot-c's: nonblocking read end, written at a pace. */
static int feed_start(struct writer *w, size_t stop_at, long delay_us)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0) return -1;
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	w->fd = p[1];
	w->start = 0;
	w->stop_at = stop_at;
	w->delay_us = delay_us;
	w->close_at_end = stop_at == file_len;
	pthread_create(&w->th, NULL, write_file, w);
	return p[0];
}

/* Everything, then silence with the fd still open. */
static int feed_start_open(struct writer *w)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0) return -1;
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	w->fd = p[1];
	w->start = 0;
	w->stop_at = file_len;
	w->delay_us = 0;
	w->close_at_end = 0;
	pthread_create(&w->th, NULL, write_file, w);
	return p[0];
}

/* A resumable source: resume waits for the gate (the channel coming
 * free), then delivers the rest of the file from `from`. */
static struct writer rest;

static int fake_resume(void *arg, size_t from)
{
	(void)arg;
	pthread_mutex_lock(&lock);
	resumes++;
	resumed_from = from;
	while (!gate_open && !cancelled) pthread_cond_wait(&gate_cond, &lock);
	int go = !cancelled;
	pthread_mutex_unlock(&lock);
	if (!go) return -1;
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0) return -1;
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	rest.fd = p[1];
	rest.start = from;
	rest.stop_at = file_len;
	rest.delay_us = 0;
	rest.close_at_end = 1;
	pthread_create(&rest.th, NULL, write_file, &rest);
	return p[0];
}

static void fake_cancel(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&lock);
	cancelled = 1;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&lock);
}

static void gate(int open)
{
	pthread_mutex_lock(&lock);
	gate_open = open;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&lock);
}

static int resumes_now(size_t *from)
{
	pthread_mutex_lock(&lock);
	int n = resumes;
	if (from) *from = resumed_from;
	pthread_mutex_unlock(&lock);
	return n;
}

static const struct track_io io_resumable = { release, fake_resume, fake_cancel, count_free };

/* Read exactly `frames` frames (fewer only at the end). */
static long read_n(struct track *t, float *dst, long frames)
{
	long got = 0, r;
	while (got < frames && (r = track_read(t, dst + got * 2, (uint32_t)(frames - got))) > 0) got += r;
	return got;
}

/* How far `got` sits from ref[at...], in frames, searching +-span: the
 * offset with the least error, and that error (max abs, left channel,
 * skipping the resampler's first `settle` frames). */
static long offset_of(const float *ref, long ref_frames, long at, const float *got, long n,
                      long span, long settle, float *err)
{
	long best = 0;
	float best_err = 1e9f;
	for (long d = -span; d <= span; d++) {
		float e = 0;
		for (long i = settle; i < n; i++) {
			long j = at + d + i;
			if (j < 0 || j >= ref_frames) { e = 1e9f; break; }
			float x = ref[j * 2] - got[i * 2];
			if (x < 0) x = -x;
			if (x > e) e = x;
			if (e >= best_err) break;
		}
		if (e < best_err) { best_err = e; best = d; }
	}
	*err = best_err;
	return best;
}

static long drain(struct track *t, int note_read)
{
	static float pcm[4096 * 2];
	long total = 0, r;
	if (note_read) {
		pthread_mutex_lock(&lock);
		read_started = 1;
		pthread_mutex_unlock(&lock);
	}
	while ((r = track_read(t, pcm, 4096)) > 0) total += r;
	return r < 0 ? r : total;
}

int main(int argc, char **argv)
{
	CHECK(argc == 4, "usage: track_test <ffmpeg> <file.ogg> <duration_ms>");
	const char *ffmpeg = argv[1];
	uint64_t want_ms = strtoull(argv[3], NULL, 10);
	signal(SIGPIPE, SIG_IGN); /* the writer outliving a closed track */

	FILE *f = fopen(argv[2], "rb");
	CHECK(f, "open test file");
	fseek(f, 0, SEEK_END);
	file_len = (size_t)ftell(f);
	rewind(f);
	file = malloc(file_len);
	CHECK(fread(file, 1, file_len, f) == file_len, "read test file");
	fclose(f);

	struct writer w;
	struct track *t;

	/* 1. A whole track, slower than decoding: every frame, then the length. */
	reset();
	struct track_src src = { feed_start(&w, file_len, 50000), file_len, &io_plain, NULL };
	t = track_open(&host, "full", ffmpeg, &src);
	CHECK(t, "open");
	long frames = drain(t, 1);
	pthread_join(w.th, NULL);
	long want_frames = (long)(want_ms * 48);
	CHECK(frames > want_frames - 480 && frames < want_frames + 480, "all frames, at 48 kHz");
	struct seen v = settle();
	CHECK(v.released == 1 && v.complete == 1, "fd released once, complete");
	CHECK(v.updates == 1 && v.ms == want_ms && v.stream == t, "exact length reported once");
	CHECK(track_read(t, (float[2]){0}, 1) == 0, "stays at the end");
	track_close(t);
	v = seen();
	CHECK(v.released == 1 && v.updates == 1, "close adds nothing");

	/* 2. Downloaded before the first read: the report waits for it.
	 * Size unknown, so EOF is what completes it. */
	reset();
	src.fd = feed_start(&w, file_len, 0);
	src.size = 0;
	t = track_open(&host, "early", ffmpeg, &src);
	CHECK(t, "open");
	pthread_join(w.th, NULL);
	for (int i = 0; i < 200 && !seen().released; i++) usleep(5000);
	usleep(20000);
	v = seen();
	CHECK(v.released == 1 && v.updates == 0, "complete, not yet reported");
	pthread_mutex_lock(&lock);
	read_started = 1;
	pthread_mutex_unlock(&lock);
	float one[2 * 64];
	CHECK(track_read(t, one, 64) > 0, "first read");
	v = seen();
	CHECK(v.updates == 1 && v.ms == want_ms, "reported on the first read");
	track_close(t);

	/* 3. Interrupt: a blocked read returns 0 at once, then reading goes on. */
	reset();
	src.size = file_len;
	src.fd = feed_start(&w, file_len / 3, 0);
	t = track_open(&host, "interrupt", ffmpeg, &src);
	CHECK(t, "open");
	float pcm[2 * 1024];
	long got = 0, r;
	do { r = track_read(t, pcm, 1024); got += r; } while (r > 0 && got < 48000 / 4);
	CHECK(got > 0, "some audio before the stall");
	track_interrupt(t);
	CHECK(track_read(t, pcm, 1024) == 0, "interrupted");
	CHECK(seen().released == 0, "still downloading");

	/* 4. Cut: the rest plays out, the length is never claimed. */
	track_cut(t);
	frames = drain(t, 1);
	CHECK(frames > 0, "what arrived still plays");
	v = seen();
	CHECK(v.released == 1 && v.complete == 0, "released, incomplete");
	CHECK(v.updates == 0, "no length for a cut track");
	track_close(t);
	pthread_join(w.th, NULL); /* the reader is gone: the writer stops */
	close(w.fd);

	/* 5. Close mid-download, mid-decode. */
	reset();
	src.fd = feed_start(&w, file_len / 2, 0);
	t = track_open(&host, "close", ffmpeg, &src);
	CHECK(t, "open");
	CHECK(track_read(t, pcm, 1024) > 0, "playing");
	track_close(t);
	v = seen();
	CHECK(v.released == 1 && v.complete == 0 && v.updates == 0, "released on close");
	pthread_join(w.th, NULL); /* the reader is gone: the writer stops */
	close(w.fd);

	/* 6. Cut, then resumed where it stopped: the whole track plays. */
	reset();
	struct track_src rsrc = { feed_start(&w, file_len / 3, 0), file_len, &io_resumable, NULL };
	t = track_open(&host, "resume", ffmpeg, &rsrc);
	CHECK(t, "open");
	CHECK(track_read(t, pcm, 1024) > 0, "playing");
	track_cut(t);
	for (int i = 0; i < 200 && !resumes_now(NULL); i++) usleep(5000);
	size_t from;
	CHECK(resumes_now(&from) == 1 && from == file_len / 3, "waits to resume at the byte it stopped");
	v = seen();
	CHECK(v.released == 1 && v.complete == 0, "the cut fd released first");
	gate(1);
	frames = drain(t, 1) + 1024;
	v = settle();
	CHECK(frames > want_frames - 2000 && frames < want_frames + 2000, "every frame, across the gap");
	CHECK(v.released == 2 && v.complete == 1, "resumed fd released, complete");
	CHECK(v.updates == 1 && v.ms == want_ms, "exact length after a resume");
	track_close(t);
	CHECK(frees == 1, "freed once");
	pthread_join(w.th, NULL); /* the reader is gone: the writer stops */
	close(w.fd);
	pthread_join(rest.th, NULL);

	/* 7. Closed while waiting to resume: the wait ends, nothing resumes. */
	reset();
	rsrc.fd = feed_start(&w, file_len / 3, 0);
	t = track_open(&host, "cancel", ffmpeg, &rsrc);
	CHECK(t, "open");
	CHECK(track_read(t, pcm, 1024) > 0, "playing");
	track_cut(t);
	for (int i = 0; i < 200 && !resumes_now(NULL); i++) usleep(5000);
	CHECK(resumes_now(NULL) == 1, "waiting to resume");
	track_close(t);
	v = seen();
	CHECK(v.released == 1 && v.updates == 0 && frees == 1, "closed cleanly, freed once");
	pthread_join(w.th, NULL); /* the reader is gone: the writer stops */
	close(w.fd);

	/* 8. Closed mid-download with resume on offer: no resume attempt. */
	reset();
	rsrc.fd = feed_start(&w, file_len / 3, 0);
	t = track_open(&host, "closing", ffmpeg, &rsrc);
	CHECK(t, "open");
	track_close(t);
	CHECK(resumes_now(NULL) <= 1 && seen().released == 1 && frees == 1, "close wins over resume");
	pthread_join(w.th, NULL); /* the reader is gone: the writer stops */
	close(w.fd);

	/* 9. A sender that never closes: the expected size completes it. */
	reset();
	src.fd = feed_start_open(&w);
	t = track_open(&host, "noeof", ffmpeg, &src);
	CHECK(t, "open");
	frames = drain(t, 1);
	v = settle();
	CHECK(frames > want_frames - 480 && frames < want_frames + 480, "all frames without EOF");
	CHECK(v.released == 1 && v.complete == 1 && v.updates == 1 && v.ms == want_ms, "complete at the size");
	track_close(t);
	pthread_join(w.th, NULL);
	close(w.fd);

	/* 10. Seeks land on the sample: compared with a straight decode. */
	static float ref[20 * 48000 * 2 + 4096 * 2], got_pcm[8192 * 2];
	reset();
	src.fd = feed_start(&w, file_len, 0);
	src.size = file_len;
	t = track_open(&host, "reference", ffmpeg, &src);
	CHECK(t, "open");
	long ref_frames = read_n(t, ref, (long)(sizeof(ref) / sizeof(ref[0]) / 2));
	track_close(t);
	pthread_join(w.th, NULL);
	CHECK(ref_frames > want_frames - 480, "reference decode");

	static const uint64_t targets[] = { 37, 1234, 7777, 15000, 19500 };
	for (size_t k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
		reset();
		src.fd = feed_start(&w, file_len, 0);
		t = track_open(&host, "seek", ffmpeg, &src);
		CHECK(t, "open");
		CHECK(read_n(t, got_pcm, 2048) == 2048, "playing");
		for (int i = 0; i < 200 && !seen().complete; i++) usleep(5000);
		CHECK(track_seek(t, targets[k]) == 0, "seek");
		long n = read_n(t, got_pcm, 8192);
		float err;
		long at = (long)targets[k] * 48;
		long off = offset_of(ref, ref_frames, at, got_pcm, n < 4096 ? n : 4096, 960, 64, &err);
		if (getenv("VERBOSE") || off != 0 || err != 0)
			fprintf(stderr, "seek %llu ms: %ld frames off, error %g\n", (unsigned long long)targets[k], off, err);
		CHECK(n >= 4096 && off == 0 && err == 0, "seek gives the very samples of a straight decode");
		track_close(t);
		pthread_join(w.th, NULL);
	}

	/* 11. A seek past what has arrived plays from there once it does. */
	reset();
	src.fd = feed_start(&w, file_len, 20000); /* about half a second */
	t = track_open(&host, "ahead", ffmpeg, &src);
	CHECK(t, "open");
	CHECK(read_n(t, got_pcm, 1024) == 1024, "playing");
	CHECK(!seen().complete, "still downloading");
	CHECK(track_seek(t, 18000) == 0, "seek ahead"); /* ~1 s has arrived */
	long n = read_n(t, got_pcm, 4096);
	float err;
	long off = offset_of(ref, ref_frames, 18000L * 48, got_pcm, n, 960, 64, &err);
	if (off != 0 || err != 0) fprintf(stderr, "seek ahead: %ld frames off, error %g\n", off, err);
	CHECK(n == 4096 && off == 0 && err == 0, "lands on the sample when the bytes arrive");
	track_close(t);
	pthread_join(w.th, NULL);

	/* 12. No decoder: open fails and leaves the fd to the caller. */
	reset();
	int p[2];
	CHECK(pipe(p) == 0, "pipe");
	struct track_src none = { p[0], 0, &io_plain, NULL };
	CHECK(!track_open(&host, "nodecoder", "/nonexistent/ffmpeg", &none), "no ffmpeg");
	CHECK(seen().released == 0 && frees == 0 && fcntl(p[0], F_GETFD) >= 0, "fd and arg untouched");
	close(p[0]);
	close(p[1]);

	free(file);
	printf("track: ok (%ld frames, %llu ms)\n", want_frames, (unsigned long long)want_ms);
	return 0;
}
