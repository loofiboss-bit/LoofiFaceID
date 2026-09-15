#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 SOURCE_ARCHIVE RPMBUILD_ROOT OUTPUT_DIRECTORY" >&2
    exit 2
fi

source_archive="$(realpath "$1")"
rpmbuild_root="$(realpath "$2")"
output_directory="$3"

if [[ ! -s "${source_archive}" || "$(basename "${source_archive}")" != "kfaceauth-5.0.0.tar.gz" ]]; then
    echo "Expected a non-empty kfaceauth-5.0.0.tar.gz source archive" >&2
    exit 1
fi
if [[ ! -d "${rpmbuild_root}/RPMS" || ! -d "${rpmbuild_root}/SRPMS" ]]; then
    echo "RPM build root is incomplete: ${rpmbuild_root}" >&2
    exit 1
fi

binary_rpms=()
while IFS= read -r candidate; do
    if [[ "$(rpm -qp --queryformat '%{NAME}' "${candidate}")" == "kfaceauth" &&
          "$(rpm -qp --queryformat '%{VERSION}' "${candidate}")" == "5.0.0" &&
          "$(rpm -qp --queryformat '%{ARCH}' "${candidate}")" != "src" ]]; then
        binary_rpms+=("${candidate}")
    fi
done < <(find "${rpmbuild_root}/RPMS" -type f -name '*.rpm' -print)
if [[ ${#binary_rpms[@]} -ne 1 ]]; then
    echo "Expected exactly one binary kfaceauth 5.0.0 RPM, found ${#binary_rpms[@]}" >&2
    exit 1
fi

mapfile -t source_rpms < <(
    find "${rpmbuild_root}/SRPMS" -maxdepth 1 -type f -name 'kfaceauth-5.0.0-*.src.rpm' -print
)
if [[ ${#source_rpms[@]} -ne 1 || ! -s "${source_rpms[0]:-}" ]]; then
    echo "Expected exactly one non-empty kfaceauth 5.0.0 source RPM" >&2
    exit 1
fi

mkdir -p "${output_directory}"
output_directory="$(realpath "${output_directory}")"
if find "${output_directory}" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
    echo "Output directory must be empty: ${output_directory}" >&2
    exit 1
fi

install -m 0644 "${source_archive}" "${output_directory}/"
install -m 0644 "${binary_rpms[0]}" "${output_directory}/"
install -m 0644 "${source_rpms[0]}" "${output_directory}/"
(
    cd "${output_directory}"
    sha256sum -- \
        "./$(basename "${source_archive}")" \
        "./$(basename "${binary_rpms[0]}")" \
        "./$(basename "${source_rpms[0]}")" \
        >SHA256SUMS
)

"$(dirname -- "${BASH_SOURCE[0]}")/verify-release-artifacts.sh" "${output_directory}"
