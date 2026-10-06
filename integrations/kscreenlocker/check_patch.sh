#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s /path/to/clean/kscreenlocker-v6.7.5-checkout\n' "$0" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_checkout=$1
expected_commit=057b3774d9ad322cfccc2683ea057aed87e0f878
patch_file=$script_dir/patches/0001-face-auth-factor-kscreenlocker-v6.7.5.patch
actual_commit=$(git -C "$source_checkout" rev-parse HEAD)

if [[ $actual_commit != "$expected_commit" ]]; then
    printf 'Expected KScreenLocker commit %s, got %s\n' "$expected_commit" "$actual_commit" >&2
    exit 1
fi

git -C "$source_checkout" apply --check "$patch_file"
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/kfaceauth-kscreenlocker-check.XXXXXX")
trap 'rm -rf -- "$temporary_root"' EXIT
git clone --quiet --no-hardlinks "$source_checkout" "$temporary_root/source"
git -C "$temporary_root/source" checkout --quiet --detach "$expected_commit"
git -C "$temporary_root/source" apply "$patch_file"
python3 "$script_dir/tests/test_patch_contract.py" "$temporary_root/source"

printf 'Patch applies cleanly to KScreenLocker %s and contract checks pass.\n' "$expected_commit"
