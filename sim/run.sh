#!/usr/bin/env bash
# Build and run the NickelSync desktop sim. Run it in WSL Ubuntu.
#
#   ./run.sh [options]               interactive window (WSLg)
#   ./run.sh --screenshot out [...]  offscreen: one scenario, png's to out/
#   ./run.sh --all out               offscreen: every scenario, one folder each
#
# Options go to the sim. See main.cc for the list.
#
# NickelSync uses the device paths /mnt/onboard/... and /tmp/nickelsync. The
# sim needs /mnt/onboard to be writable. Create it once:
#   wsl -d Ubuntu -u root -- sh -c 'mkdir -p /mnt/onboard && chown <user> /mnt/onboard'
set -euo pipefail

SIM="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ONBOARD=/mnt/onboard

if [ ! -w "$ONBOARD" ]; then
    echo "run.sh: $ONBOARD is missing or not writable. Create it once with:" >&2
    echo "  wsl -d Ubuntu -u root -- sh -c 'mkdir -p $ONBOARD && chown $(id -un) $ONBOARD'" >&2
    exit 1
fi

make -s -C "$SIM"

# Screenshot runs use the Qt VNC platform, not offscreen: its screen can have
# the size of the Kobo screen, and Qt places popups inside the screen. The
# offscreen screen is always 800x600. The VNC server listens on localhost only.
SIZE=758x1008
prev=""
for a in "$@"; do
    [ "$prev" = "--size" ] && SIZE="$a"
    prev="$a"
done
PLATFORM="vnc:size=$SIZE:addr=127.0.0.1:port=${VNC_PORT:-5917}"

# reset clears the state of the last run: the Nickel config with LastSyncTime,
# the NickelSync config and log.
reset() {
    rm -rf "$ONBOARD/.kobo" "$ONBOARD/.adds/nickelsync"
    mkdir -p "$ONBOARD/.kobo/Kobo"
}

if [ "${1:-}" = "--all" ]; then
    out="${2:?usage: run.sh --all <dir>}"
    rc=0
    run() {
        local name="$1"; shift
        reset
        echo "== $name"
        rm -rf "$out/$name"
        if ! QT_QPA_PLATFORM="$PLATFORM" "$SIM/nickelsync-sim" --screenshot "$out/$name" "$@"; then
            echo "== $name: FAIL" >&2
            rc=1
        fi
    }
    run full-wifi-off
    run full-wifi-on        --wifi on
    run full-server-newer   --server newer
    run quick               --pick "Sync reading position"
    run quick-server-newer  --pick "Sync reading position" --server newer
    run wifi-fail           --wifi fail
    run bar-hardcover       --bar-icons hardcover
    run bar-crowded         --bar-icons hardcover,kpm,other
    exit $rc
fi

reset
for a in "$@"; do
    if [ "$a" = "--screenshot" ]; then
        export QT_QPA_PLATFORM="$PLATFORM"
    fi
done
exec "$SIM/nickelsync-sim" "$@"
