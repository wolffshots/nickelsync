#!/bin/sh
# Build NickelSync for Qt 5 and Qt 6 and write two packages:
#
#   Kobo.tgz      firmware 4.x and 6.x: imageformats/libnickelsync.so (Qt 5)
#                 and imageformats/libnickelsync-qt6.so (Qt 6). Both firmwares
#                 extract it into /usr/local/Kobo. The mod writes its files on
#                 the user partition at the first start.
#   KoboRoot.tgz  firmware 4.x only (Qt 5), as before. kpm uses this file.
#
# Run it in the repository root, with Podman or Docker (ENGINE=docker).
set -eu

QT5_IMAGE=ghcr.io/pgaskin/nickeltc:1.0
QT6_IMAGE=ghcr.io/nicoverbruggen/kobuild:qt6-v0.5.3@sha256:b939301323e1bed0bf6b0bee43b8791db2efdf01231d4245c1f972d57433644a
ENGINE=${ENGINE:-podman}

cd "$(dirname "$0")/.."
# git can fail in the container (for example in a worktree), so get the
# version here.
VERSION=${VERSION:-$(git describe --tags --always --dirty)}
SRC=$PWD
command -v cygpath >/dev/null && SRC=$(cygpath -w "$PWD")
# Git Bash on Windows: do not change the container paths.
export MSYS_NO_PATHCONV=1

run() {
    image=$1
    shift
    "$ENGINE" run --rm -v "$SRC":/src -w /src "$image" "$@"
}

# Both builds use the same object files, so each build starts with clean.
run "$QT6_IMAGE" make clean all NH_QT_MAJOR=6 VERSION="$VERSION"
mkdir -p build
mv libnickelsync-qt6.so build/
run "$QT5_IMAGE" make clean all koboroot VERSION="$VERSION"
cp libnickelsync.so build/
run "$QT5_IMAGE" tar czf Kobo.tgz --owner=root --group=root --mode=0755 \
    --transform=s,^build/,imageformats/, build/libnickelsync.so build/libnickelsync-qt6.so
rm -r build

tar tvzf Kobo.tgz
sha256sum Kobo.tgz KoboRoot.tgz
