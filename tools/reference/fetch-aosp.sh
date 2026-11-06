#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
destination=${AOSP_REF_DIR:-$root/build/reference/aosp-frameworks-native}
commit=bfcf75076e562945bae131a4929f29a90d0d2481
mkdir -p "$destination"
files=(Parcel.cpp Parcel.h IPCThreadState.cpp ProcessState.cpp)
paths=(libs/binder/Parcel.cpp libs/binder/include/binder/Parcel.h libs/binder/IPCThreadState.cpp libs/binder/ProcessState.cpp)
hashes=(6bc86cdf9347df82f46c06874f4f441d501de5e9d57263025deee02ebe263baf 1370bd744f4f392511e4d16c34f333f826497a0ce5101c8189d924fd0615dbc4 aaf58ce5087c882a75e64698c4d9d752c55e0e31d8011edb9a45e1f0231dae44 4146f161a7aac9e6ef1496798986325496d913a2231e63fe5d8e00be9b5e8ee1)
for index in "${!files[@]}"; do
    target="$destination/${files[$index]}"
    if [[ ! -f "$target" ]]; then
        curl -fsSL --retry 2 --max-time 30 \
            -H 'Accept: application/vnd.github.raw+json' \
            "https://api.github.com/repos/aosp-mirror-neo/platform_frameworks_native/contents/${paths[$index]}?ref=$commit" \
            -o "$target.part"
        printf '%s  %s\n' "${hashes[$index]}" "$target.part" | sha256sum -c -
        mv "$target.part" "$target"
    fi
    printf '%s  %s\n' "${hashes[$index]}" "$target" | sha256sum -c -
done
printf 'AOSP frameworks/native commit=%s (android-14.0.0_r1)\n' "$commit"
