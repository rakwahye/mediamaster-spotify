/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file lrc/tokens.c
 * @brief The one place the plugin reads librespot-c's internals.
 *
 * librespot-c fetches the spclient address and both tokens for its own
 * requests but does not export them. Rather than patch it, this file is
 * built with librespot-c's own flags against its internal header, so it
 * always matches the pinned revision: if upstream reshapes the session,
 * the build breaks here, not at run time. When librespot-c gains an
 * accessor, this file goes away.
 *
 * The fields are written only on librespot-c's thread while a login or
 * a track open runs. The caller reads after librespotc_open returned
 * (its completion is synchronised) and while holding the plugin's
 * channel, so no other open can start meanwhile. */

#include "librespot-c-internal.h"
#include "../spclient.h"
#include <stdio.h>
#include <string.h>

int spclient_from_session(struct sp_session *ss, struct spclient *out)
{
	if (!ss || !ss->spclient.address[0] || !ss->http_accesstoken.value[0] || !ss->http_clienttoken.value[0])
		return -1;
	snprintf(out->host, sizeof(out->host), "%s", ss->spclient.address);
	out->port = ss->spclient.port ? ss->spclient.port : 443;
	snprintf(out->access_token, sizeof(out->access_token), "%s", ss->http_accesstoken.value);
	snprintf(out->client_token, sizeof(out->client_token), "%s", ss->http_clienttoken.value);
	out->expires = ss->http_accesstoken.received_ts + ss->http_accesstoken.expires_after_seconds;
	return 0;
}
