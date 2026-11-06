#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
accel=${ACCEL:-tcg}
limit=${VM_TIMEOUT:-120}
log="$root/artifacts/qemu-$accel.log"
mkdir -p "$root/artifacts"
case "$accel" in tcg|kvm) ;; *) echo 'ACCEL must be tcg or kvm' >&2; exit 2 ;; esac
set +e
timeout --signal=TERM --kill-after=5 "$limit" qemu-system-x86_64 \
    -machine "q35,accel=$accel" -cpu max -m 512 -smp 2 -nographic -no-reboot \
    -monitor none -nic none \
    -kernel "$root/build/kernel/out/arch/x86/boot/bzImage" \
    -initrd "$root/build/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 oops=panic' > "$log" 2>&1
status=$?
set -e
cat "$log"
if [[ "$status" -ne 0 ]]; then
    printf 'VM execution failed (exit=%s), log=%s\n' "$status" "$log" >&2
    exit "$status"
fi
if grep -Eq 'MINI_LAB_FAIL|Kernel panic|BUG:|WARNING:|Oops:' "$log"; then
    echo 'VM reported an assertion failure or kernel diagnostic' >&2
    exit 1
fi
if ! grep -q '^MINI_LAB_PASS' "$log"; then
    echo 'VM did not reach its successful test marker' >&2
    exit 1
fi
printf 'PASS: VM assertions and log checks (%s)\n' "$accel"
