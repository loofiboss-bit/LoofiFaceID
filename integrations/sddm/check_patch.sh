#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s /path/to/clean/sddm-v0.21.0-checkout\n' "$0" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_checkout=$1
expected_commit=63780fcd79f1dbf81a30eef48c28c699ab15aded
patch_file=$script_dir/patches/0001-face-auth-v0.21.0.patch
actual_commit=$(git -C "$source_checkout" rev-parse HEAD)

if [[ $actual_commit != "$expected_commit" ]]; then
    printf 'Expected SDDM commit %s, got %s\n' "$expected_commit" "$actual_commit" >&2
    exit 1
fi

if [[ -n $(git -C "$source_checkout" status --porcelain) ]]; then
    printf 'The SDDM checkout must be clean before checking this patch.\n' >&2
    exit 1
fi

git -C "$source_checkout" apply --check "$patch_file"
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/kfaceauth-sddm-check.XXXXXX")
trap 'rm -rf -- "$temporary_root"' EXIT
git clone --quiet --no-hardlinks "$source_checkout" "$temporary_root/source"
git -C "$temporary_root/source" checkout --quiet --detach "$expected_commit"
git -C "$temporary_root/source" apply "$patch_file"
python3 "$script_dir/tests/test_patch_contract.py" "$temporary_root/source"

printf 'Patch applies cleanly to SDDM %s and contract checks pass.\n' "$expected_commit"
