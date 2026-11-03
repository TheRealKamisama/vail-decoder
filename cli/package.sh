#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the built vail-cli into an archive for a release, from the top of the repository:
#
#   cli/package.sh PLATFORM [TAG]
#
# PLATFORM goes in the archive's name, such as linux-x86_64; one starting with "windows" gets a .zip. A TAG must name
# the version that was built, as vail-cli-vX.Y.Z, so that a release cannot carry the wrong binary.
set -e
platform=$1
tag=$2
case $platform in
    windows*) exe=vail-cli.exe ;;
    "") echo "usage: cli/package.sh PLATFORM [TAG]" >&2; exit 2 ;;
    *) exe=vail-cli ;;
esac
set -- $(cli/build/$exe --version | tr -d '\r')
version=$2
if [ -n "$tag" ] && [ "$tag" != "vail-cli-v$version" ]; then
    echo "package.sh: tag $tag, but the binary is version $version" >&2
    exit 1
fi
dir=vail-cli-$version-$platform
rm -rf "$dir"
mkdir "$dir"
cp "cli/build/$exe" cli/vail-cli.1 cli/README.md LICENSE "$dir/"
case $platform in
    windows*) rm -f "$dir.zip"; zip -qr "$dir.zip" "$dir"; echo "$dir.zip" ;;
    *) tar czf "$dir.tar.gz" "$dir"; echo "$dir.tar.gz" ;;
esac
rm -rf "$dir"
