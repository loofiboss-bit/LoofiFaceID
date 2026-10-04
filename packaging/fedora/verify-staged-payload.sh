#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 || ( $2 != standard && $2 != experimental-auth ) ]]; then
    echo "Usage: $0 STAGE_ROOT standard|experimental-auth" >&2
    exit 2
fi

stage_root=$(realpath "$1")
[[ -d $stage_root ]] || {
    echo "Staged install root not found: $stage_root" >&2
    exit 2
}

fail() {
    echo "Staged payload verification failed: $1" >&2
    exit 1
}

mapfile -t files < <(cd "$stage_root" && find . -type f -printf '/%P\n' | sort)

auth_pattern='(^|/)(kfaceauthd|kfaceauth-sync-vault|kfaceauth-pam-setup|pam_kfaceauth\.so|kfaceauth\.service|kfaceauth\.socket|kfaceauth\.conf|org\.kde\.kfaceauth\.policy)(/|$)'
forbidden_state_pattern='^/(etc/pam\.d|etc/kfaceauth|var/lib/kfaceauth)(/|$)'

if [[ $2 == standard ]]; then
    for path in "${files[@]}"; do
        if [[ $path =~ $auth_pattern || $path == /usr/share/kfaceauth/selinux/* || $path =~ $forbidden_state_pattern ]]; then
            fail "standard install contains experimental authentication content: $path"
        fi
    done
    echo "Standard staged payload boundary verified"
    exit 0
fi

expected_auth_files=(
    /usr/bin/kfaceauth-pam-setup
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
)
printf '%s\n' "${files[@]}" | grep -Eq '^/usr/lib(64)?/security/pam_kfaceauth\.so$' \
    || fail "experimental staged payload is missing pam_kfaceauth.so"
actual_auth_files=()
for path in "${files[@]}"; do
    if [[ $path =~ $auth_pattern || $path == /usr/share/kfaceauth/selinux/* ]]; then
        actual_auth_files+=("$path")
    fi
done
mapfile -t actual_auth_files < <(printf '%s\n' "${actual_auth_files[@]}" | sort)
mapfile -t expected_auth_files < <(
    printf '%s\n' "${expected_auth_files[@]}" /usr/lib64/security/pam_kfaceauth.so | sort
)
if [[ $(printf '%s\n' "${actual_auth_files[@]}") != $(printf '%s\n' "${expected_auth_files[@]}") ]]; then
    diff -u \
        <(printf '%s\n' "${expected_auth_files[@]}") \
        <(printf '%s\n' "${actual_auth_files[@]}") >&2 || true
    fail "experimental staged payload contains an unexpected set of authentication files"
fi

for path in "${files[@]}"; do
    if [[ $path =~ $forbidden_state_pattern ]]; then
        fail "staged install contains host PAM rules or provisioned state: $path"
    fi
done

echo "Experimental staged payload boundary verified"
