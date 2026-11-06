#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
kernel="$root/build/kernel/out"
busybox_path=${BUSYBOX:-/usr/bin/busybox}
if ! file "$busybox_path" | grep -q 'statically linked'; then
    printf 'BUSYBOX must point to a static BusyBox binary\n' >&2
    exit 1
fi
make -C "$kernel" M="$root/examples/modules/hello" modules
make -C "$kernel" M="$root/driver" modules
cc -static -std=c11 -O2 -Wall -Wextra -Werror -I"$root/include/uapi" \
    "$root/tests/kernel/device_test.c" -o "$root/build/device_test"
image_dir="$root/build/rootfs"
mkdir -p "$image_dir/bin" "$image_dir/dev" "$image_dir/proc" "$image_dir/sys" "$image_dir/tmp" "$root/artifacts"
cp "$busybox_path" "$image_dir/bin/busybox"
for applet in sh mount grep insmod rmmod dmesg poweroff cat echo mkdir sleep; do
    ln -sf busybox "$image_dir/bin/$applet"
done
install -m 755 "$root/tools/qemu/init" "$image_dir/init"
cp "$root/examples/modules/hello/mini_hello.ko" "$image_dir/mini_hello.ko"
cp "$root/driver/mini_binder.ko" "$image_dir/mini_binder.ko"
cp "$root/build/device_test" "$image_dir/device_test"
(cd "$image_dir" && find . -print0 | LC_ALL=C sort -z | cpio --null --create --format=newc --owner=0:0) | gzip -n > "$root/build/initramfs.cpio.gz"
{
    "$busybox_path" | sed -n '1p'
    sha256sum "$busybox_path" "$root/build/initramfs.cpio.gz"
    file "$root/examples/modules/hello/mini_hello.ko"
} > "$root/artifacts/rootfs-build.txt"
