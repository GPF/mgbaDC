#!/bin/sh
set -e

# Compile two representative Dreamcast include orders and make sure the
# PATH_MAX-sized directory member does not move mCore::init.
if [ -f /opt/toolchains/dc/kos/environ.sh ]; then
	. /opt/toolchains/dc/kos/environ.sh
fi
set -u

: "${KOS_CC:=kos-cc}"
: "${KOS_SYSROOT:=/opt/toolchains/dc/kos-ports}"
: "${MGBA_ROOT:=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

probe() {
	order=$1
	case "$order" in
	core)
		prefix='#include <mgba-util/common.h>
#include <mgba/core/core.h>'
		;;
	network)
		prefix='#include <sys/socket.h>
#include <mgba/core/core.h>'
		;;
	esac
	printf '%s\nconst unsigned layout[] = { sizeof(struct mCore), offsetof(struct mCore, init), PATH_MAX };\n' "$prefix" |
	"$KOS_CC" --sysroot="$KOS_SYSROOT" -D_GNU_SOURCE -D__DREAMCAST__ -I"$MGBA_ROOT/include" -I"$MGBA_ROOT/src" -I/opt/toolchains/dc/kos/include -x c -S -o "$tmp" -
	sed -n '/^_layout:/,/^\.text/p' "$tmp" |
		sed '/^_layout:/d;/^\.text/d' |
		tr -d '\t' |
		sed '/^\.text/d'
}

core=$(probe core)
network=$(probe network)
if [ "$core" != "$network" ]; then
	printf 'mCore layout mismatch:\ncore: %s\nnetwork: %s\n' "$core" "$network" >&2
	exit 1
fi
printf 'mCore layout consistent: %s\n' "$core"
