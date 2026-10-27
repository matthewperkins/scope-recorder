#!/usr/bin/env bash
# Unpacks the IC Imaging Control 4 SDK (headers + libic4core.so) into cpp/third_party/ic4/
# for building the C++ app. No root needed; nothing is installed system-wide.
#
# The SDK is The Imaging Source's proprietary software and is not included in this repository.
# Download "IC Imaging Control 4 SDK" for Linux (ic4_<version>_amd64.tar.gz) from
#   https://www.theimagingsource.com/en-us/support/download/
# then run:
#   scripts/fetch_ic4_sdk.sh path/to/ic4_<version>_amd64.tar.gz
set -euo pipefail

TARBALL="${1:?Usage: $0 path/to/ic4_<version>_amd64.tar.gz}"
[ -f "$TARBALL" ] || { echo "Not found: $TARBALL" >&2; exit 1; }
DEST="$(cd "$(dirname "$0")/.." && pwd)/cpp/third_party/ic4"
if [ -e "$DEST" ]; then
    echo "$DEST already exists; move it aside first to re-fetch." >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf -- "${TMP:?}"' EXIT

tar -xzf "$TARBALL" -C "$TMP"
unpack_deb() {  # $1 = package name prefix, $2 = output dir
    local deb
    deb=$(find "$TMP" -name "${1}_*_amd64.deb" | head -n1)
    [ -n "$deb" ] || { echo "No ${1}_*.deb in $TARBALL" >&2; exit 1; }
    mkdir -p "$2"
    (cd "$2" && ar x "$deb" && tar -xf data.tar.*)
}
unpack_deb ic4 "$TMP/ic4"
unpack_deb ic4-dev "$TMP/ic4-dev"

mkdir -p "$DEST/lib"
cp -r "$TMP/ic4-dev/usr/include" "$DEST/"
cp "$TMP/ic4/usr/lib/x86_64-linux-gnu/libic4core.so" "$DEST/lib/"
cp "$TMP/ic4/usr/share/theimagingsource/ic4/license.txt" "$DEST/"
echo "IC4 SDK unpacked into $DEST"
