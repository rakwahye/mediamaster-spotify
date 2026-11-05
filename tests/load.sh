#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Load the plugin into the pinned mediamaster and check it comes up.
# Usage: tests/load.sh <mediamaster-build-dir> <plugin-build-dir>
set -eu

HOST=$1
PLUG=$2
T=$(mktemp -d /tmp/mm-spotify-test.XXXXXX)
PID=
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
fail() { echo "FAIL load: $*" >&2; sed 's/^/  log| /' "$T/log" >&2; exit 1; }

export MM_SOCK="$T/mm.sock"
M="$HOST/mmctl"

"$HOST/mediamaster" -c /dev/null -o "host.socket=$MM_SOCK" -o "host.plugin_dir=$PLUG" \
	-o host.sink=null -o "spotify.cache=$T/cache" 2>"$T/log" &
PID=$!
i=0
until "$M" version >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -lt 50 ] || fail "daemon did not answer"
	sleep 0.1
done

"$M" plugins | grep -q "^spotify .*active.*attrs=network,login,rate-limited,unofficial-api .*spotify.so$" \
	|| fail "spotify.so is not active with its attributes"
[ "$("$M" spotify)" = "ok logged out, cache $T/cache" ] || fail "spotify verb: $("$M" spotify)"

mkdir -p "$T/cache"
: >"$T/cache/credentials"
[ "$("$M" spotify)" = "ok logged in, cache $T/cache" ] || fail "credentials not noticed"

"$M" quit >/dev/null
wait "$PID" || fail "daemon exit status"
PID=
echo "load: all checks passed"
