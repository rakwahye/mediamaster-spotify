#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# The host must not know this plugin exists: its repository names no
# service, so the knowledge of which plugin does what stays in config
# and in the plugins. This plugin checks that for its own name, and for
# the library it is built on, against a mediamaster checkout: $MM_SRC,
# or ../mediamaster when that is one. Every commit on its main branch is
# checked, messages included, not just the tip.
# Usage: sh tests/host-clean.sh
set -u
src=${MM_SRC:-$(dirname "$0")/../../mediamaster}
if ! git -C "$src" rev-parse --verify main >/dev/null 2>&1; then
	echo "host-clean: skipped (no mediamaster checkout at $src; set MM_SRC)"
	exit 0
fi
words='spotify|librespot|ncspot'
bad=0
for c in $(git -C "$src" rev-list main); do
	hits=$(git -C "$src" grep -niE "$words" "$c" -- . 2>/dev/null | sed "s/^$c:/  /" | head -5)
	msg=$(git -C "$src" show -s --format=%B "$c" | grep -niE "$words" | head -3)
	if [ -n "$hits$msg" ]; then
		echo "host-clean: $(git -C "$src" show -s --format='%h %s' "$c" | cut -c1-70) names us:"
		[ -n "$hits" ] && echo "$hits"
		[ -n "$msg" ] && echo "$msg" | sed 's/^/  message: /'
		bad=1
	fi
done
[ $bad -eq 0 ] || exit 1
echo "host-clean: ok ($(git -C "$src" rev-list --count main) commits of $(cd "$src" && pwd))"
