#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 STANDARD_RPM [EXPERIMENTAL_AUTH_RPM]" >&2
    exit 2
fi

standard_rpm=$(realpath "$1")
if [[ ! -f $standard_rpm ]]; then
    echo "Standard RPM not found: $standard_rpm" >&2
    exit 2
fi

fail() {
    echo "RPM payload verification failed: $1" >&2
    exit 1
}

assert_package_name() {
    local rpm_path=$1
    local expected=$2
    local actual
    actual=$(rpm -qp --queryformat '%{NAME}' "$rpm_path")
    [[ $actual == "$expected" ]] || fail "expected package $expected, got $actual"
}

assert_no_auth_payload() {
    local rpm_path=$1
    local file_list=$2
    if grep -Eq '(^|/)(kfaceauthd|kfaceauth-sync-vault|kfaceauth-pam-setup|pam_kfaceauth\.so|kfaceauth\.service|kfaceauth\.socket|kfaceauth\.conf|org\.kde\.kfaceauth\.policy)(/|$)' <<<"$file_list" \
        || grep -Eq '^/usr/share/kfaceauth/selinux(/|$)' <<<"$file_list" \
        || grep -Eq '^/(etc/pam\.d|etc/kfaceauth|var/lib/kfaceauth)(/|$)' <<<"$file_list"; then
        fail "$rpm_path contains experimental authentication files or state"
    fi
}

assert_no_scriptlets() {
    local rpm_path=$1
    local scripts
    scripts=$(rpm -qp --scripts "$rpm_path")
    [[ -z $scripts ]] || fail "$rpm_path has package scriptlets"
}

assert_package_name "$standard_rpm" kfaceauth
standard_files=$(rpm -qpl "$standard_rpm")
assert_no_auth_payload "$standard_rpm" "$standard_files"
assert_no_scriptlets "$standard_rpm"

allowed_docs=(
    ANVANDARGUIDE-SV.md
    ARCHITECTURE.md
    BUILDING.md
    CHANGELOG.md
    README.md
    THREAT-BOUNDARY.md
    TROUBLESHOOTING.md
    USER-GUIDE.md
)
while IFS= read -r path; do
    if [[ $path =~ ^/usr/share/doc/[^/]+$ ]]; then
        continue
    fi
    [[ $path == /usr/share/doc/* ]] || continue
    case " ${allowed_docs[*]} " in
        *" $(basename -- "$path") "*) ;;
        *) fail "$standard_rpm includes non-allowlisted documentation: $path" ;;
    esac
done <<<"$standard_files"
for doc in "${allowed_docs[@]}"; do
    grep -Eq "^/usr/share/doc/[^/]+/${doc}$" <<<"$standard_files" \
        || fail "$standard_rpm is missing allowlisted documentation: $doc"
done

if [[ $# -eq 2 ]]; then
    experimental_rpm=$(realpath "$2")
    [[ -f $experimental_rpm ]] || fail "experimental RPM not found: $experimental_rpm"
    assert_package_name "$experimental_rpm" kfaceauth-experimental-auth
    experimental_files=$(rpm -qpl "$experimental_rpm")
    assert_no_scriptlets "$experimental_rpm"

    expected_auth_files=(
        /usr/bin/kfaceauth-pam-setup
        /usr/lib64/security/pam_kfaceauth.so
        /usr/libexec/kfaceauthd
        /usr/libexec/kfaceauth-sync-vault
        /usr/lib/systemd/system/kfaceauth.service
        /usr/lib/systemd/system/kfaceauth.socket
        /usr/lib/sysusers.d/kfaceauth.conf
        /usr/share/kfaceauth/selinux/kfaceauth.fc
        /usr/share/kfaceauth/selinux/kfaceauth.te
        /usr/share/kfaceauth/selinux/kfaceauth.pp
        /usr/share/kfaceauth/selinux/kfaceauth_sddm.te
        /usr/share/kfaceauth/selinux/kfaceauth_sddm.pp
        /usr/share/polkit-1/actions/org.kde.kfaceauth.policy
        /usr/share/kfaceauth/selinux
    )
    for path in "${expected_auth_files[@]}"; do
        grep -Fxq "$path" <<<"$experimental_files" \
            || fail "$experimental_rpm is missing intended opt-in file: $path"
    done
    if grep -Eq '^/(etc/pam\.d|etc/kfaceauth|var/lib/kfaceauth)(/|$)' <<<"$experimental_files"; then
        fail "$experimental_rpm contains host PAM rules or provisioned user/system state"
    fi
    expected_sorted=$(printf '%s\n' "${expected_auth_files[@]}" | sort)
    actual_sorted=$(
        printf '%s\n' "$experimental_files" \
            | grep -Ev '^/usr/lib/\.build-id(/|$)' \
            | sort
    )
    if [[ $actual_sorted != "$expected_sorted" ]]; then
        diff -u \
            <(printf '%s\n' "$expected_sorted") \
            <(printf '%s\n' "$actual_sorted") >&2 || true
        fail "$experimental_rpm contains files outside the intended PAM, daemon, systemd, Polkit, and SELinux payload"
    fi
    echo "Standard and experimental RPM payload boundaries verified"
else
    echo "Standard RPM payload boundary verified"
fi
