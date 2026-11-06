#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
accel=${ACCEL:-tcg}
limit=${VM_TIMEOUT:-120}
scenario=${LAB_CASE:-normal}
run_id=${RUN_ID:-$(date -u +%Y%m%dT%H%M%S-%N)}
[[ "$run_id" =~ ^[a-zA-Z0-9_.-]+$ ]] || { echo 'Invalid RUN_ID' >&2; exit 2; }
case "$scenario" in normal|assert|no-marker|hang|panic) ;; *) echo 'Invalid LAB_CASE' >&2; exit 2 ;; esac
suffix=""
[[ "$scenario" = normal ]] || suffix="-$scenario"
log_tag=${LOG_TAG:-$accel$suffix}
[[ "$log_tag" =~ ^[a-zA-Z0-9_.-]+$ ]] || { echo 'Invalid LOG_TAG' >&2; exit 2; }
log="$root/artifacts/qemu-$log_tag.log"
mkdir -p "$root/artifacts"
case "$accel" in tcg|kvm) ;; *) echo 'ACCEL must be tcg or kvm' >&2; exit 2 ;; esac
set +e
timeout --signal=TERM --kill-after=5 "$limit" qemu-system-x86_64 \
    -machine "q35,accel=$accel" -cpu max -m 512 -smp 2 -nographic -no-reboot \
    -monitor none -nic none \
    -kernel "${KERNEL_IMAGE:-$root/build/kernel/out/arch/x86/boot/bzImage}" \
    -initrd "$root/build/initramfs.cpio.gz" \
    -append "console=ttyS0 rdinit=/init panic=1 oops=panic mini_lab=$scenario mini_run_id=$run_id" > "$log" 2>&1
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
