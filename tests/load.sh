#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Load the plugin into the pinned mediamaster and check it comes up.
# Spotify itself is not needed: login is exercised up to its failure.
# Usage: tests/load.sh <mediamaster-build-dir> <plugin-build-dir>
set -eu

HOST=$1
PLUG=$2
T=$(mktemp -d /tmp/mm-spotify-test.XXXXXX)
PID=
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
fail() { echo "FAIL load: $*" >&2; sed 's/^/  log| /' "$T/log" >&2; exit 1; }

# A plugin exports its entry point and nothing else.
exports=$(nm -D --defined-only "$PLUG/spotify.so" | awk '{print $3}')
[ "$exports" = mm_plugin_entry ] || { echo "FAIL load: spotify.so exports: $exports" >&2; exit 1; }

export MM_SOCK="$T/mm.sock"
M="$HOST/mmctl"
C="$T/xdg/mediamaster/spotify" # parents do not exist yet, like the default

start() {
	"$HOST/mediamaster" -c /dev/null -o "host.socket=$MM_SOCK" -o "host.plugin_dir=$PLUG" \
		-o host.sink=null -o "spotify.cache=$C" "$@" 2>>"$T/log" &
	PID=$!
	i=0
	until "$M" version >/dev/null 2>&1; do
		i=$((i + 1)); [ $i -lt 50 ] || fail "daemon did not answer"
		sleep 0.1
	done
}
stop() {
	"$M" quit >/dev/null
	wait "$PID" || fail "daemon exit status"
	PID=
}

start
"$M" plugins | grep -q "^spotify .*active.*attrs=network,login,rate-limited,unofficial-api .*spotify.so$" \
	|| fail "spotify.so is not active with its attributes"
[ "$("$M" spotify)" = "ok logged out, cache $C, librespot-c 0.9" ] || fail "status: $("$M" spotify)"

# A stable device id, created once.
id=$(cat "$C/device_id")
echo "$id" | grep -qx '[0-9a-f]\{40\}' || fail "device_id: $id"
[ "$(stat -c %a "$C")" = 700 ] && [ "$(stat -c %a "$C/device_id")" = 600 ] || fail "cache modes: $(stat -c '%a %n' "$C" "$C/device_id")"

# No credentials anywhere: a clear error, no network.
out=$("$M" spotify login 2>&1) && fail "login without credentials succeeded"
echo "$out" | grep -q "no credentials: set spotify.credentials" || fail "login without credentials: $out"
if "$M" spotify bogus >/dev/null 2>&1; then fail "unknown subcommand accepted"; fi

# The source: spotify: URIs reach it, and fail cleanly without a login.
out=$("$M" play spotify:track:4uLU6hMCjMI75M1A2tKUQC 2>&1) && fail "play without credentials succeeded"
echo "$out" | grep -q "spotify could not open" || fail "play without credentials: $out"
grep -q "no credentials" "$T/log" || fail "play did not try to log in"
out=$("$M" play spotify:episode:1 2>&1) && fail "episode played as a track"
grep -q "not a track: spotify:episode:1" "$T/log" || fail "non-track URI: $out"

# The catalog: a track is not a list (no network); lists need a login.
"$M" browse spotify:track:4uLU6hMCjMI75M1A2tKUQC 2>&1 | grep -q "is a track" || fail "browse of a track"
out=$("$M" browse spotify: 2>&1) && fail "browse without credentials succeeded"
echo "$out" | grep -q "no credentials" || fail "browse without credentials: $out"
"$M" plugins | grep -q '^spotify .*ifaces=source,catalog' || fail "catalog not listed: $("$M" plugins)"
stop

# Imported credentials that Spotify will not take (here: cannot reach it).
printf '{"username":"someone","auth_type":1,"auth_data":"AQID"}' >"$T/import.json"
cp "$T/import.json" "$T/import.orig"
start -o "spotify.credentials=$T/import.json"
[ "$(cat "$C/device_id")" = "$id" ] || fail "device_id changed across restarts"
t0=$(date +%s)
out=$("$M" spotify login 2>&1) && fail "a bogus login succeeded"
[ $(( $(date +%s) - t0 )) -le 30 ] || fail "a failing login took more than 30 s"
echo "$out" | grep -q "^err login failed: " || fail "failed login: $out"
"$M" spotify | grep -q "^ok logged out (login failed: " || fail "status after failure: $("$M" spotify)"
cmp -s "$T/import.json" "$T/import.orig" || fail "the imported file was modified"
[ ! -e "$C/credentials.json" ] || fail "credentials saved after a failed login"

# Logout removes mediamaster's own saved login, never the import.
printf '{"username":"x","auth_type":1,"auth_data":"AQID"}' >"$C/credentials.json"
[ "$("$M" spotify logout)" = "ok logged out" ] || fail "logout"
[ ! -e "$C/credentials.json" ] || fail "logout left the saved credentials"
[ -e "$T/import.json" ] || fail "logout touched the imported file"
stop

echo "load: all checks passed"
