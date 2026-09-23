#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 PATH_TO_BINARY_RPM [LEGACY_FIXTURE_SPEC_OR_RPM]" >&2
    exit 2
fi

rpm_path="$(realpath "$1")"
if [[ ! -f "${rpm_path}" ]]; then
    echo "RPM not found: ${rpm_path}" >&2
    exit 2
fi

legacy_fixture="$(
    realpath "${2:-$(dirname -- "${BASH_SOURCE[0]}")/tests/plasma-irlume-3.0.0-fixture.spec}"
)"
if [[ ! -f "${legacy_fixture}" ]]; then
    echo "Legacy transition fixture not found: ${legacy_fixture}" >&2
    exit 2
fi

fixture_topdir=""
smoke_root=""
cleanup() {
    if [[ -n "${smoke_root}" ]]; then
        rm -rf -- "${smoke_root}"
    fi
    if [[ -n "${fixture_topdir}" ]]; then
        rm -rf -- "${fixture_topdir}"
    fi
}
trap cleanup EXIT

if [[ "${legacy_fixture}" == *.rpm ]]; then
    legacy_rpm="${legacy_fixture}"
    legacy_nevra="$(rpm -qp --queryformat '%{NAME}-%{VERSION}' "${legacy_rpm}")"
    if [[ "${legacy_nevra}" != "plasma-irlume-3.0.0" ]]; then
        echo "Expected a plasma-irlume 3.0.0 fixture RPM, got ${legacy_nevra}" >&2
        exit 2
    fi
else
    if ! command -v rpmbuild >/dev/null 2>&1; then
        echo "rpmbuild is required for the isolated v3-to-v4 transition test" >&2
        exit 2
    fi

    fixture_topdir="$(mktemp -d)"
    mkdir -p \
        "${fixture_topdir}/BUILD" \
        "${fixture_topdir}/BUILDROOT" \
        "${fixture_topdir}/RPMS" \
        "${fixture_topdir}/SOURCES" \
        "${fixture_topdir}/SPECS" \
        "${fixture_topdir}/SRPMS"
    rpmbuild -bb "${legacy_fixture}" --define "_topdir ${fixture_topdir}"
    legacy_rpm="$(
        find "${fixture_topdir}/RPMS" \
            -type f -name 'plasma-irlume-3.0.0-1.*.rpm' -print -quit
    )"
    if [[ -z "${legacy_rpm}" || ! -s "${legacy_rpm}" ]]; then
        echo "Failed to build the isolated plasma-irlume 3.0.0 transition fixture" >&2
        exit 1
    fi
fi

if [[ "${EUID}" -ne 0 ]]; then
    if ! command -v podman >/dev/null 2>&1; then
        echo "Run as root or install Podman for the isolated RPM transaction" >&2
        exit 2
    fi

    script_path="$(realpath "${BASH_SOURCE[0]}")"
    podman run --rm \
        --network=none \
        --security-opt label=disable \
        --volume "${rpm_path}:/tmp/kfaceauth.rpm:ro" \
        --volume "${legacy_rpm}:/tmp/plasma-irlume-3.0.0.rpm:ro" \
        --volume "${script_path}:/tmp/rpm-smoke-test.sh:ro" \
        fedora:44 \
        bash /tmp/rpm-smoke-test.sh \
            /tmp/kfaceauth.rpm \
            /tmp/plasma-irlume-3.0.0.rpm
    exit
fi

package_name="$(rpm -qp --queryformat '%{NAME}' "${rpm_path}")"
if [[ "${package_name}" != "kfaceauth" ]]; then
    echo "Expected kfaceauth RPM, got ${package_name}" >&2
    exit 1
fi

if rpm -qp --scripts "${rpm_path}" | grep -Eiq 'pam|authselect|biometric'; then
    echo "Package scriptlets must not mutate authentication state" >&2
    exit 1
fi

smoke_root="$(mktemp -d)"
mkdir -p \
    "${smoke_root}/usr/lib/sysimage/rpm" \
    "${smoke_root}/home/test/.config" \
    "${smoke_root}/etc/pam.d"
printf 'user-setting\n' >"${smoke_root}/home/test/.config/kfaceauth-preserve-me"
printf 'legacy-user-setting\n' >"${smoke_root}/home/test/.config/plasma-irlume.conf"
printf 'auth required pam_unix.so\n' >"${smoke_root}/etc/pam.d/kfaceauth-sentinel"

pam_hash_before="$(sha256sum "${smoke_root}/etc/pam.d/kfaceauth-sentinel")"
user_hash_before="$(sha256sum "${smoke_root}/home/test/.config/kfaceauth-preserve-me")"
legacy_user_hash_before="$(sha256sum "${smoke_root}/home/test/.config/plasma-irlume.conf")"

rpm --root "${smoke_root}" --initdb
rpm --root "${smoke_root}" --nodeps --install "${legacy_rpm}"
mapfile -t legacy_paths < <(rpm --root "${smoke_root}" -ql plasma-irlume)
for path in "${legacy_paths[@]}"; do
    if [[ ! -f "${smoke_root}${path}" ]]; then
        echo "Legacy fixture payload is missing before upgrade: ${path}" >&2
        exit 1
    fi
done

rpm --root "${smoke_root}" --nodeps --upgrade "${rpm_path}"
if rpm --root "${smoke_root}" -qa --queryformat '%{NAME}\n' | grep -Fxq plasma-irlume; then
    echo "Legacy plasma-irlume package remains installed after upgrade" >&2
    exit 1
fi
for path in "${legacy_paths[@]}"; do
    if [[ -e "${smoke_root}${path}" || -L "${smoke_root}${path}" ]]; then
        echo "Legacy plasma-irlume payload remains after upgrade: ${path}" >&2
        exit 1
    fi
done
rpm --root "${smoke_root}" --nodeps --upgrade --replacepkgs "${rpm_path}"

required_paths=(
    "/usr/libexec/kfaceauth-camera-preview-worker"
    "/usr/libexec/kfaceauth-vision-worker"
    "/usr/libexec/kfaceauth-identity-worker"
    "/usr/share/applications/kcm_kfaceauth.desktop"
    "/usr/share/kfaceauth/models/manifest.kfaceauth"
    "/usr/share/kfaceauth/models/files/face_detection_yunet_2023mar.onnx"
    "/usr/share/kfaceauth/models/files/face_recognition_sface_2021dec.onnx"
)

for path in "${required_paths[@]}"; do
    if [[ ! -f "${smoke_root}${path}" ]]; then
        echo "Installed payload is missing ${path}" >&2
        exit 1
    fi
done

if find \
    "${smoke_root}/usr/libexec/kfaceauth-camera-preview-worker" \
    "${smoke_root}/usr/libexec/kfaceauth-vision-worker" \
    "${smoke_root}/usr/libexec/kfaceauth-identity-worker" \
    -perm /6000 -print -quit | grep -q .; then
    echo "KFaceAuth workers must not be setuid or setgid" >&2
    exit 1
fi
if command -v getcap >/dev/null 2>&1 &&
    getcap \
        "${smoke_root}/usr/libexec/kfaceauth-camera-preview-worker" \
        "${smoke_root}/usr/libexec/kfaceauth-vision-worker" \
        "${smoke_root}/usr/libexec/kfaceauth-identity-worker" | grep -q .; then
    echo "KFaceAuth workers must not have file capabilities" >&2
    exit 1
fi

payload="$(rpm -qlp "${rpm_path}")"
for pattern in \
    '/kauth/' \
    '/dbus-1/system-services/' \
    '/dbus-1/system.d/' \
    '/polkit-1/actions/' \
    'kfaceauth-auth-helper'; do
    if grep -Fq "${pattern}" <<<"${payload}"; then
        echo "Installed payload contains forbidden privileged path: ${pattern}" >&2
        exit 1
    fi
done

installed_model="${smoke_root}/usr/share/kfaceauth/models/files/face_detection_yunet_2023mar.onnx"
if [[ "$(sha256sum "${installed_model}" | cut -d' ' -f1)" != \
    "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4" ]]; then
    echo "Installed YuNet model failed checksum verification" >&2
    exit 1
fi
installed_sface="${smoke_root}/usr/share/kfaceauth/models/files/face_recognition_sface_2021dec.onnx"
if [[ "$(sha256sum "${installed_sface}" | cut -d' ' -f1)" != \
    "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79" ]]; then
    echo "Installed SFace model failed checksum verification" >&2
    exit 1
fi

if [[ -e "${smoke_root}/home/test/.local/share/kfaceauth" ]]; then
    echo "Installation or upgrade created user profile state" >&2
    exit 1
fi

plugin_path="$(
    rpm --root "${smoke_root}" -ql kfaceauth \
        | grep -E '/plasma/kcms/systemsettings/kcm_kfaceauth\.so$'
)"
if [[ -z "${plugin_path}" || ! -f "${smoke_root}${plugin_path}" ]]; then
    echo "Installed payload is missing the KCM plugin" >&2
    exit 1
fi

if [[ "$(sha256sum "${smoke_root}/etc/pam.d/kfaceauth-sentinel")" != "${pam_hash_before}" ]]; then
    echo "PAM sentinel changed during installation" >&2
    exit 1
fi
[[ "$(sha256sum "${smoke_root}/home/test/.config/kfaceauth-preserve-me")" == "${user_hash_before}" ]]
[[ "$(sha256sum "${smoke_root}/home/test/.config/plasma-irlume.conf")" == "${legacy_user_hash_before}" ]]
[[ ! -e "${smoke_root}/home/test/.local/share/kfaceauth" ]]

mapfile -t packaged_paths < <(
    rpm --root "${smoke_root}" -ql kfaceauth \
        | while IFS= read -r path; do
              [[ -f "${smoke_root}${path}" || -L "${smoke_root}${path}" ]] && printf '%s\n' "${path}"
          done
)

rpm --root "${smoke_root}" --erase kfaceauth

if [[ "$(sha256sum "${smoke_root}/etc/pam.d/kfaceauth-sentinel")" != "${pam_hash_before}" ]]; then
    echo "PAM sentinel changed during removal" >&2
    exit 1
fi
for path in "${packaged_paths[@]}"; do
    if [[ -e "${smoke_root}${path}" || -L "${smoke_root}${path}" ]]; then
        echo "Package-owned file remains after uninstall: ${path}" >&2
        exit 1
    fi
done
[[ "$(sha256sum "${smoke_root}/home/test/.config/kfaceauth-preserve-me")" == "${user_hash_before}" ]]
[[ "$(sha256sum "${smoke_root}/home/test/.config/plasma-irlume.conf")" == "${legacy_user_hash_before}" ]]

rpm --root "${smoke_root}" --nodeps --install "${rpm_path}"
rpm --root "${smoke_root}" --nodeps --upgrade --replacepkgs "${rpm_path}"
rpm --root "${smoke_root}" --erase kfaceauth
[[ "$(sha256sum "${smoke_root}/etc/pam.d/kfaceauth-sentinel")" == "${pam_hash_before}" ]]
[[ "$(sha256sum "${smoke_root}/home/test/.config/kfaceauth-preserve-me")" == "${user_hash_before}" ]]
[[ "$(sha256sum "${smoke_root}/home/test/.config/plasma-irlume.conf")" == "${legacy_user_hash_before}" ]]
[[ ! -e "${smoke_root}/home/test/.local/share/kfaceauth" ]]

echo "RPM v3-to-v5.1 transition and clean install/reinstall/remove lifecycle smoke test passed"
