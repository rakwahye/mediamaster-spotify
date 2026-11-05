/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file lrc.c
 * @brief librespot-c lifecycle and callbacks. */

#include "lrc.h"
#include "librespot-c.h"
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const struct mm_host *s_host; /* librespot-c is global; so is its log sink */
static int s_running;

static int tcp_connect(const char *address, unsigned short port)
{
	struct addrinfo hints, *res, *ai;
	char service[8];
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%hu", port);
	if (getaddrinfo(address, service, &hints, &res) != 0) return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0) continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static void tcp_disconnect(int fd)
{
	if (fd >= 0) close(fd);
}

static void logmsg(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	size_t n = strlen(line);
	while (n && line[n - 1] == '\n') line[--n] = '\0';
	if (s_host && n) s_host->log(s_host, MM_LOG_DEBUG, "librespot-c: %s", line);
}

static void hexdump(const char *msg, uint8_t *data, size_t len)
{
	(void)msg;
	(void)data;
	(void)len;
}

static struct sp_callbacks s_callbacks = {
	.tcp_connect = tcp_connect,
	.tcp_disconnect = tcp_disconnect,
	.hexdump = hexdump,
	.logmsg = logmsg,
};

int lrc_start(const struct mm_host *h)
{
	struct sp_sysinfo si;

	if (s_running) return 0;
	s_host = h;
	memset(&si, 0, sizeof(si)); /* zeros: librespot-c presents a whitelisted client */
	if (librespotc_init(&si, &s_callbacks) < 0) {
		h->log(h, MM_LOG_WARN, "librespot-c: %s", librespotc_last_errmsg());
		s_host = NULL;
		return -1;
	}
	s_running = 1;
	return 0;
}

void lrc_stop(void)
{
	if (!s_running) return;
	librespotc_deinit();
	s_running = 0;
	s_host = NULL;
}

const char *lrc_version(void)
{
	static char v[32];
	if (!v[0]) snprintf(v, sizeof(v), "librespot-c %d.%d", LIBRESPOT_C_VERSION_MAJOR, LIBRESPOT_C_VERSION_MINOR);
	return v;
}
