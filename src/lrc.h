/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file lrc.h
 * @brief librespot-c, wired to the host.
 *
 * librespot-c is a process-wide singleton with its own thread. This file
 * owns its lifecycle and the callbacks it needs from us: a TCP connector
 * and a log sink, which goes to the host log at debug level. */

#ifndef SPOTIFY_LRC_H
#define SPOTIFY_LRC_H

#include "mm_plugin.h"

/** Start librespot-c. @returns 0, or -1 with the reason logged. */
int lrc_start(const struct mm_host *h);
void lrc_stop(void);

/** "librespot-c <major>.<minor>" */
const char *lrc_version(void);

#endif
