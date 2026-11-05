/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file spotify.c
 * @brief Spotify for mediamaster.
 *
 * A mediamaster plugin is one shared object exporting one function,
 * mm_plugin_entry(), which returns a static `struct mm_plugin`. The
 * struct names the plugin, states its policy attributes, and points at
 * the interface tables it implements. Everything the plugin needs from
 * the daemon arrives in `struct mm_host` at init.
 *
 * Config is read under the plugin's own name ("spotify.<key>"):
 *   spotify.cache = <dir>    credentials and audio cache
 *                            (default $XDG_CACHE_HOME/mediamaster/spotify)
 *
 * Commands:
 *   spotify                  login state, cache location, library version
 *
 * Files: spotify.c (this: the plugin), lrc.c (librespot-c glue). */

#include "mm_plugin.h"
#include "lrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct spotify {
	const struct mm_host *h;
	char cache[1024];
};

static void default_cache(char *out, size_t cap)
{
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0] == '/')
		snprintf(out, cap, "%s/mediamaster/spotify", xdg);
	else
		snprintf(out, cap, "%s/.cache/mediamaster/spotify", home ? home : "/tmp");
}

static int logged_in(const struct spotify *sp)
{
	char path[1100];
	struct stat st;
	snprintf(path, sizeof(path), "%s/credentials", sp->cache);
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int verb_spotify(void *priv, struct mm_verb_ctx *c)
{
	const struct spotify *sp = priv;
	snprintf(c->reply, c->reply_len, "%s, cache %s, %s",
	         logged_in(sp) ? "logged in" : "logged out", sp->cache, lrc_version());
	return MM_OK;
}

static int spotify_init(const struct mm_host *h, void **priv)
{
	struct spotify *sp = calloc(1, sizeof(*sp));
	if (!sp) return -1;
	sp->h = h;

	const char *cache = h->config_get(h, "cache");
	if (cache && *cache) snprintf(sp->cache, sizeof(sp->cache), "%s", cache);
	else default_cache(sp->cache, sizeof(sp->cache));

	if (lrc_start(h) != 0) {
		free(sp);
		return -1;
	}
	if (h->register_verb(h, "spotify", "\tlogin state and cache location", verb_spotify, sp) != 0) {
		lrc_stop();
		free(sp);
		return -1;
	}
	*priv = sp;
	return 0;
}

static void spotify_fini(void *priv)
{
	lrc_stop();
	free(priv);
}

static const struct mm_plugin plugin = {
	.abi         = MM_ABI,
	.attrs       = MM_ATTR_NETWORK | MM_ATTR_NEEDS_LOGIN |
	               MM_ATTR_RATE_LIMITED | MM_ATTR_UNOFFICIAL_API,
	.name        = "spotify",
	.version     = "0.1.0",
	.description = "Spotify, via librespot-c",
	.init        = spotify_init,
	.fini        = spotify_fini,
};

MM_PLUGIN_DEFINE(spotify)
{
	return &plugin;
}
