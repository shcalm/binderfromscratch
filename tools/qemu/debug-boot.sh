#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
port=${GDB_PORT:-12345}
[[ "$port" =~ ^[0-9]+$ ]] && (( port > 1023 && port < 65536 )) || exit 2
mkdir -p "$root/artifacts"
qemu-system-x86_64 -machine q35,accel=tcg -cpu max -m 512 -smp 1 \
    -nographic -no-reboot -monitor none -nic none \
    -kernel "$root/build/kernel/out/arch/x86/boot/bzImage" \
    -initrd "$root/build/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init nokaslr panic=1' \
    -S -gdb "tcp:127.0.0.1:$port" > "$root/artifacts/gdb-boot.log" 2>&1 &
vm_pid=$!
cleanup() {
    kill -TERM "$vm_pid" 2>/dev/null || true
    wait "$vm_pid" 2>/dev/null || true
}
trap cleanup EXIT
timeout --signal=TERM --kill-after=5 30 gdb -q -batch \
    "$root/build/kernel/out/vmlinux" \
    -ex 'set pagination off' \
    -ex 'set tcp connect-timeout 5' \
    -ex "target remote 127.0.0.1:$port" \
    -ex 'hbreak start_kernel' -ex continue -ex bt -ex detach \
    > "$root/artifacts/gdb-session.log" 2>&1
cat "$root/artifacts/gdb-session.log"
grep -Eq 'Breakpoint [0-9]+,.*start_kernel' "$root/artifacts/gdb-session.log"
grep -q '#0.*start_kernel' "$root/artifacts/gdb-session.log"
echo 'PASS: kernel function breakpoint and stack captured (no source-line DWARF)'
