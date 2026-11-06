#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=6.6
sha=d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0
cache="$root/build/kernel"
source_dir="$cache/linux-$version"
output="$cache/out"
jobs=${JOBS:-8}
mkdir -p "$cache" "$root/artifacts"
archive="$cache/linux-$version.tar.xz"
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 3 -o "$archive.part" "${KERNEL_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/kernel}/v6.x/linux-$version.tar.xz"
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum -c -
if [[ ! -d "$source_dir" ]]; then
    tar -xJf "$archive" -C "$cache"
fi
mkdir -p "$output"
if [[ ! -f "$output/.config" ]]; then
    make -C "$source_dir" O="$output" x86_64_defconfig
    "$source_dir/scripts/kconfig/merge_config.sh" -m -O "$output" "$output/.config" "$root/tools/kernel/config"
    make -C "$source_dir" O="$output" olddefconfig
fi
make -C "$source_dir" O="$output" -j"$jobs" bzImage modules
cp "$output/.config" "$root/artifacts/kernel.config"
{
    printf 'kernel_source_sha256=%s\n' "$sha"
    make -s -C "$source_dir" O="$output" kernelrelease
    "${CC:-cc}" --version | sed -n '1p'
    make --version | sed -n '1p'
    sha256sum "$output/arch/x86/boot/bzImage" "$output/.config"
} > "$root/artifacts/kernel-build.txt"
