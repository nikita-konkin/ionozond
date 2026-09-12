#!/usr/bin/env bash
#
# Put the console in the applications menu, so it starts by clicking rather
# than by remembering a path under ~/.cache.
#
#   bash 19-install-desktop.sh            # menu entry
#   bash 19-install-desktop.sh --desktop  # ... and an icon on the desktop
#
# No sudo: everything goes under ~/.local, which is the per-user half of the
# XDG search path. A system-wide entry would need root and would point at one
# user's build directory anyway.
set -u

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$HOME/.cache/ionozond-build}"
BINARY="${BINARY:-$BUILD/ionozond}"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor/32x32/apps"
ENTRY="$APPS/ionozond.desktop"

say() { printf '%s\n' "$*"; }

echo "--- the console ------------------------------------------------------"
if [ ! -x "$BINARY" ]; then
    say "  *** $BINARY is not there yet."
    say "  *** Build it first:  bash tools/build-native.sh"
    exit 1
fi
say "  $BINARY"

# ---- the icon -------------------------------------------------------------
# 32x32, which is all the original .ico ever held. It will look soft on a
# HiDPI dock; dropping a larger PNG at the same path is all that takes to fix.
mkdir -p "$ICONS"
if [ -f "$HERE/res/ico/ionozond.png" ]; then
    cp "$HERE/res/ico/ionozond.png" "$ICONS/ionozond.png"
    say "  icon  $ICONS/ionozond.png"
fi

# ---- the entry ------------------------------------------------------------
mkdir -p "$APPS"
sed "s#@EXEC@#$BINARY#" "$HERE/tools/ionozond.desktop.in" > "$ENTRY"
chmod 0644 "$ENTRY"
say "  entry $ENTRY"

# Refresh the menu. Absent on a minimal install, and not worth failing over.
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$APPS" >/dev/null 2>&1
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1

# ---- optionally, the desktop itself ---------------------------------------
if [ "${1:-}" = "--desktop" ]; then
    # GNOME on 24.04 shows a desktop launcher only when the file is executable
    # AND carries the trusted metadata; without both it appears as an
    # "Untrusted application launcher" that refuses to run. Older GTK and
    # every non-GNOME desktop ignore the metadata and just want +x.
    DESK=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
    mkdir -p "$DESK"
    cp "$ENTRY" "$DESK/ionozond.desktop"
    chmod +x "$DESK/ionozond.desktop"
    command -v gio >/dev/null 2>&1 && \
        gio set "$DESK/ionozond.desktop" metadata::trusted true 2>/dev/null
    say "  desktop $DESK/ionozond.desktop"
fi

echo
say "  It should appear in the applications menu straight away. If it does"
say "  not, the shell caches the menu until it is restarted -- log out and"
say "  in, or just run $ENTRY's Exec line once to confirm the path is right."
