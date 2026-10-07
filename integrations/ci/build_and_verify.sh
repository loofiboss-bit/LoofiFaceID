#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a disposable, patched upstream checkout; never install on the host.
set -euo pipefail

if [[ $# -ne 2 || ( $1 != sddm && $1 != kscreenlocker ) ]]; then
    printf 'Usage: %s {sddm|kscreenlocker} /path/to/clean/upstream-checkout\n' "$0" >&2
    exit 2
fi
component=$1
source_checkout=$(realpath -- "$2")
repository_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ -n $(git -C "$source_checkout" status --porcelain) ]]; then
    printf 'The upstream checkout must be clean.\n' >&2
    exit 1
fi
"$repository_root/integrations/$component/check_patch.sh" "$source_checkout"
work_root=$(mktemp -d "${TMPDIR:-/tmp}/kfaceauth-integration-build.XXXXXX")
trap 'rm -rf -- "$work_root"' EXIT
git clone --quiet --no-hardlinks "$source_checkout" "$work_root/source"
if [[ $component == sddm ]]; then
    patch_name=0001-face-auth-v0.21.0.patch
else
    patch_name=0001-face-auth-factor-kscreenlocker-v6.7.5.patch
fi
git -C "$work_root/source" apply "$repository_root/integrations/$component/patches/$patch_name"

common_options=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    -DCMAKE_INSTALL_LIBDIR=lib64 -DBUILD_TESTING=ON)
if [[ $component == sddm ]]; then
    component_options=(-DBUILD_WITH_QT6=ON -DINSTALL_PAM_CONFIGURATION=OFF
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
    option_name=SDDM_ENABLE_EXPERIMENTAL_FACE_AUTH
    test_pattern='^(Configuration|QMLThemeConfig|Session)$'
else
    component_options=()
    option_name=KSCREENLOCKER_ENABLE_EXPERIMENTAL_FACE_AUTH
    test_pattern='^kscreenlocker-facePamAuthenticatorTest$'
fi
export QT_QPA_PLATFORM=offscreen
for configuration in standard experimental; do
    build_root="$work_root/build-$configuration"
    stage_root="$work_root/stage-$configuration"
    options=("${common_options[@]}" "${component_options[@]}")
    # Omit the option in the standard build to test its real upstream default.
    if [[ $configuration == experimental ]]; then
        options+=("-D$option_name=ON")
    fi
    cmake -S "$work_root/source" -B "$build_root" "${options[@]}"
    expected_value=OFF
    [[ $configuration != experimental ]] || expected_value=ON
    if ! grep -qx "$option_name:BOOL=$expected_value" "$build_root/CMakeCache.txt"; then
        printf 'Unexpected experimental default for %s.\n' "$component" >&2
        exit 1
    fi
    cmake --build "$build_root" --parallel 2
    ctest --test-dir "$build_root" --output-on-failure --no-tests=error -R "$test_pattern"
    if [[ $component == sddm ]]; then
        ctest --test-dir "$build_root" --output-on-failure --no-tests=error \
            -R '^FaceAuthenticationStatus$'
    fi
    DESTDIR="$stage_root" cmake --install "$build_root"
    python3 "$repository_root/integrations/ci/verify_staged_integration.py" \
        "$component" "$configuration" "$stage_root"
done
