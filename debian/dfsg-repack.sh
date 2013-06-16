#!/bin/bash

set -e

if [ ! -f "$3" ] && [ ! -f "$1" ]; then
    echo "This script must be run via uscan or by manually specifying the tarball" >&2
    exit 1
fi

tarball=

[ -f "$3" ] && tarball="$3"
[ -z "$tarball" -a -f "$1" ] && tarball="$1"

fname="$(basename "$tarball")"
tarball="$(readlink -f "$tarball")"

tdir="$(mktemp -d)"
trap '[ ! -d "$tdir" ] || rm -r "$tdir"' EXIT

xzcat "$tarball" | \
    tar --wildcards \
        --delete '*/ext/json/*' \
    > "$tdir/${fname/.xz}"
xz "$tdir/${fname/.xz}"

mv "$tdir/$fname" "${tarball/orig.tar.xz}.dfsg.orig.tar.xz"
