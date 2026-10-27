#!/usr/bin/env bash
# Adds "Scope Recorder" (the C++ app) to your desktop's application launcher, with its icon.
# Per-user: writes only under ~/.local/share. Run after building cpp/build/scope_recorder.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/cpp/build/scope_recorder"
[ -x "$BIN" ] || { echo "Build the C++ app first: $BIN not found" >&2; exit 1; }
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"

install -Dm644 "$ROOT/cpp/resources/scope-recorder.svg" "$DATA/icons/hicolor/scalable/apps/scope-recorder.svg"
# Some launchers (e.g. hyprlauncher) skip icon directories without an index.theme.
if [ ! -e "$DATA/icons/hicolor/index.theme" ] && [ -e /usr/share/icons/hicolor/index.theme ]; then
    cp /usr/share/icons/hicolor/index.theme "$DATA/icons/hicolor/index.theme"
fi

sed -e "s|@BIN@|$BIN|" -e "s|@ROOT@|$ROOT/cpp|" "$ROOT/packaging/scope-recorder.desktop.in" \
    > "$DATA/applications/scope-recorder.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$DATA/applications" || true
echo "Installed $DATA/applications/scope-recorder.desktop"
