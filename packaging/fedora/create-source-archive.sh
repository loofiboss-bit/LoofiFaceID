#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
identity_file="${repo_root}/cmake/ProjectIdentity.cmake"
package_name="$(
    sed -nE 's/^set\(KFACEAUTH_PROJECT_ID "([^"]+)"\)$/\1/p' "${identity_file}"
)"
version="$(
    sed -nE 's/^set\(KFACEAUTH_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\)$/\1/p' "${identity_file}"
)"

if [[ -z "${package_name}" || -z "${version}" ]]; then
    echo "Unable to read the project identity" >&2
    exit 1
fi

output="${1:-${repo_root}/${package_name}-${version}.tar.gz}"
source_date_epoch="${SOURCE_DATE_EPOCH:-$(git -C "${repo_root}" log -1 --format=%ct 2>/dev/null || printf '0')}"
temporary_archive="$(mktemp --tmpdir kfaceauth-archive.XXXXXX.tar.gz)"
trap 'rm -f -- "${temporary_archive}"' EXIT

tar \
    --sort=name \
    --mtime="@${source_date_epoch}" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --mode='u+rwX,go+rX,go-w' \
    --transform="s,^\.,${package_name}-${version}," \
    --exclude='./.git' \
    --exclude='./.cache' \
    --exclude='./.directory' \
    --exclude='./.agents' \
    --exclude='./.mypy_cache' \
    --exclude='./.pytest_cache' \
    --exclude='./.vscode' \
    --exclude='*/__pycache__' \
    --exclude='./build*' \
    --exclude='./redhat-linux-build' \
    --exclude='./engine/target' \
    --exclude='./rpmbuild' \
    --exclude='./*.rpm' \
    --exclude='./*.src.rpm' \
    --exclude='./*.tar.gz' \
    -C "${repo_root}" \
    -czf "${temporary_archive}" \
    .

mv -- "${temporary_archive}" "${output}"
chmod 0644 "${output}"
trap - EXIT

echo "${output}"
