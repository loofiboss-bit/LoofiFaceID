#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ $# -eq 1 ]] || { printf 'Usage: %s /path/to/clean/plasma-desktop-v6.7.5\n' "$0" >&2; exit 2; }
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
expected_commit=43f55fff3480c6eb5f60133f045bfc74b9c67d84
[[ $(git -C "$1" rev-parse HEAD) == "$expected_commit" ]] || { echo 'Incorrect Plasma Desktop baseline' >&2; exit 1; }
[[ -z $(git -C "$1" status --porcelain) ]] || { echo 'The upstream checkout must be clean' >&2; exit 1; }
patch="$script_dir/patches/0001-face-control-plasma-desktop-v6.7.5.patch"
git -C "$1" apply --check "$patch"
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/kfaceauth-plasma-check.XXXXXX")
trap 'rm -rf -- "$temporary_root"' EXIT
git clone --quiet --no-hardlinks "$1" "$temporary_root/source"
git -C "$temporary_root/source" apply "$patch"
python3 "$script_dir/tests/test_patch_contract.py" "$temporary_root/source"
