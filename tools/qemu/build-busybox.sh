#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=1.36.1
sha=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
cache="$root/build/busybox"
source_dir="$cache/busybox-$version"
mkdir -p "$cache" "$root/artifacts"
archive="$cache/busybox-$version.tar.bz2"
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 3 -o "$archive.part" "https://busybox.net/downloads/busybox-$version.tar.bz2"
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum -c -
if [[ ! -d "$source_dir" ]]; then
    tar -xjf "$archive" -C "$cache"
fi
if [[ ! -f "$source_dir/.config" ]]; then
    make -C "$source_dir" defconfig
    # This generated configuration selects static linking, and avoids obsolete
    # CBQ definitions removed from newer host kernel headers.
    sed -i -e 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
        -e 's/CONFIG_TC=y/# CONFIG_TC is not set/' "$source_dir/.config"
    make -C "$source_dir" oldconfig </dev/null
fi
make -C "$source_dir" -j"${JOBS:-8}"
{
    printf 'busybox_source_sha256=%s\n' "$sha"
    "$source_dir/busybox" | sed -n '1p'
    sha256sum "$source_dir/.config" "$source_dir/busybox"
} > "$root/artifacts/busybox-build.txt"
