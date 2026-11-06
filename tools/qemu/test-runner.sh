#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
for scenario in assert no-marker panic hang; do
    expected=1
    [[ "$scenario" != hang ]] || expected=124
    set +e
    ACCEL=tcg LAB_CASE="$scenario" VM_TIMEOUT=5 bash tools/qemu/run.sh > "$root/artifacts/runner-$scenario.txt" 2>&1
    status=$?
    set -e
    if [[ "$status" != "$expected" ]]; then
        printf 'Runner case %s: expected=%s got=%s\n' "$scenario" "$expected" "$status" >&2
        exit 1
    fi
    case "$scenario" in
        assert) grep -q 'MINI_LAB_FAIL: deliberate runner assertion' artifacts/runner-assert.txt ;;
        no-marker) grep -q 'did not reach its successful test marker' artifacts/runner-no-marker.txt ;;
        panic) grep -q 'Kernel panic' artifacts/runner-panic.txt ;;
        hang) grep -q 'MINI_LAB_BOOT' artifacts/runner-hang.txt ;;
    esac
    printf 'PASS: runner rejects %s (exit=%s)\n' "$scenario" "$status"
done
set +e
KERNEL_IMAGE="$root/build/missing-kernel" LOG_TAG=tcg-startup ACCEL=tcg bash tools/qemu/run.sh > artifacts/runner-startup.txt 2>&1
status=$?
set -e
[[ "$status" != 0 ]] || { echo 'Missing kernel reported success' >&2; exit 1; }
grep -q 'VM execution failed' artifacts/runner-startup.txt
echo 'PASS: runner rejects startup failure'
