#!/usr/bin/env bash

set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
version="$(python3 "${repo_root}/tools/verify_project_identity.py" --field version)"
archive_name="$(python3 "${repo_root}/tools/verify_project_identity.py" --field archive)"

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 ARTIFACT_DIRECTORY" >&2
    exit 2
fi

artifact_directory="$(realpath "$1")"
if [[ ! -d "${artifact_directory}" ]]; then
    echo "Artifact directory not found: ${artifact_directory}" >&2
    exit 2
fi

mapfile -t release_files < <(
    find "${artifact_directory}" -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort
)
if [[ ${#release_files[@]} -ne 4 ]]; then
    echo "Expected exactly four release files, found ${#release_files[@]}" >&2
    printf 'Found: %s\n' "${release_files[*]:-none}" >&2
    exit 1
fi

expected_archive="${archive_name}"
if [[ ! -s "${artifact_directory}/${expected_archive}" ]]; then
    echo "Missing or empty source archive: ${expected_archive}" >&2
    exit 1
fi
if [[ ! -s "${artifact_directory}/SHA256SUMS" ]]; then
    echo "Missing or empty SHA256SUMS" >&2
    exit 1
fi

mapfile -t source_rpms < <(
    find "${artifact_directory}" -maxdepth 1 -type f -name "kfaceauth-${version}-*.src.rpm" -printf '%f\n'
)
if [[ ${#source_rpms[@]} -ne 1 || ! -s "${artifact_directory}/${source_rpms[0]:-}" ]]; then
    echo "Expected exactly one non-empty kfaceauth source RPM" >&2
    exit 1
fi

binary_rpms=()
while IFS= read -r candidate; do
    if [[ "$(rpm -qp --queryformat '%{NAME}' "${artifact_directory}/${candidate}")" == "kfaceauth" &&
          "$(rpm -qp --queryformat '%{VERSION}' "${artifact_directory}/${candidate}")" == "${version}" &&
          "$(rpm -qp --queryformat '%{SOURCEPACKAGE}' "${artifact_directory}/${candidate}")" != "1" ]]; then
        binary_rpms+=("${candidate}")
    fi
done < <(
    find "${artifact_directory}" -maxdepth 1 -type f -name "kfaceauth-${version}-*.rpm" \
        ! -name '*.src.rpm' -printf '%f\n'
)
if [[ ${#binary_rpms[@]} -ne 1 || ! -s "${artifact_directory}/${binary_rpms[0]:-}" ]]; then
    echo "Expected exactly one non-empty binary kfaceauth RPM" >&2
    exit 1
fi

if [[ "$(rpm -qp --queryformat '%{NAME} %{VERSION} %{SOURCEPACKAGE}' "${artifact_directory}/${source_rpms[0]}")" != "kfaceauth ${version} 1" ]]; then
    echo "Source RPM metadata differs from canonical identity" >&2
    exit 1
fi

expected_payloads="$(
    printf './%s\n' "${expected_archive}" "${binary_rpms[0]}" "${source_rpms[0]}" | LC_ALL=C sort
)"
manifest_payloads="$(
    awk 'NF == 2 { print $2 }' "${artifact_directory}/SHA256SUMS" | LC_ALL=C sort
)"
if [[ "${manifest_payloads}" != "${expected_payloads}" ]]; then
    echo "SHA256SUMS must contain each release payload exactly once" >&2
    exit 1
fi
if [[ "$(printf '%s\n' "${manifest_payloads}" | sort -u | wc -l)" -ne 3 ]]; then
    echo "SHA256SUMS contains duplicate payload entries" >&2
    exit 1
fi

(
    cd "${artifact_directory}"
    sha256sum --check --strict SHA256SUMS
)

echo "Complete KFaceAuth release artifact set verified"
