#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

KFACEAUTH_PAM_DIRECTORY=/etc/pam.d
KFACEAUTH_PAM_TEMPLATE_DIRECTORY=/usr/share/kfaceauth/pam
KFACEAUTH_SELINUX_POLICY_DIR=/usr/share/kfaceauth/selinux
KFACEAUTH_RUNTIME_DIRECTORY=/run/kfaceauth
KFACEAUTH_SOCKET_PATH=${KFACEAUTH_RUNTIME_DIRECTORY}/kfaceauthd.sock
KFACEAUTH_SYSTEM_VAULT_ROOT=/var/lib/kfaceauth
KFACEAUTH_SYSTEM_KEY_ROOT=/etc/kfaceauth/keys
KFACEAUTH_PAM_MODULE_PATHS=(/usr/lib64/security/pam_kfaceauth.so /usr/lib/security/pam_kfaceauth.so)
KFACEAUTH_LEGACY_BEGIN='# BEGIN kfaceauth experimental authentication'
KFACEAUTH_LEGACY_END='# END kfaceauth experimental authentication'

declare -a installed_modules=()
declare -a legacy_pam_files=()
declare -a legacy_pam_backups=()
socket_was_enabled=0
socket_was_active=0
service_was_active=0
socket_state_touched=0
created_pam_service=

fail() {
    printf 'kfaceauth-pam-setup: %s\n' "$1" >&2
    exit 1
}

target_service() {
    case "$1" in
        sddm) printf '%s\n' sddm-kfaceauth ;;
        plasma-lock) printf '%s\n' kde-kfaceauth ;;
        *) fail 'target must be sddm or plasma-lock' ;;
    esac
}

has_admin_rights() {
    [[ $EUID -eq 0 ]]
}

module_installed() {
    semodule -l | awk -v module="$1" '$1 == module { found = 1 } END { exit !found }'
}

rollback_modules() {
    local index failed=0 module
    for ((index = ${#installed_modules[@]} - 1; index >= 0; index--)); do
        module=${installed_modules[index]}
        if module_installed "$module"; then
            semodule -r "$module" || failed=1
        fi
    done
    return "$failed"
}

capture_socket_state() {
    socket_was_enabled=0
    socket_was_active=0
    service_was_active=0
    systemctl is-enabled --quiet kfaceauth.socket && socket_was_enabled=1 || true
    systemctl is-active --quiet kfaceauth.socket && socket_was_active=1 || true
    systemctl is-active --quiet kfaceauth.service && service_was_active=1 || true
}

restore_socket_state() {
    local failed=0
    if [[ $socket_was_active -eq 1 ]]; then
        systemctl start kfaceauth.socket || failed=1
    else
        systemctl stop kfaceauth.socket || failed=1
    fi
    if [[ $service_was_active -eq 1 ]]; then
        systemctl start kfaceauth.service || failed=1
    else
        systemctl stop kfaceauth.service || failed=1
    fi
    if [[ $socket_was_enabled -eq 1 ]]; then
        systemctl enable kfaceauth.socket || failed=1
    else
        systemctl disable kfaceauth.socket || failed=1
    fi
    return "$failed"
}

restorecon_if_present() {
    local path=$1
    [[ -e $path ]] || return 0
    restorecon -R -v "$path"
}

install_policy_modules() {
    local target=$1
    local mode module package
    command -v getenforce >/dev/null 2>&1 || return 1
    mode=$(getenforce) || return 1
    [[ $mode == Disabled ]] && return 0
    command -v semodule >/dev/null 2>&1 || return 1
    command -v restorecon >/dev/null 2>&1 || return 1

    local modules=(kfaceauth)
    [[ $target == sddm ]] && modules+=(kfaceauth_sddm)
    for module in "${modules[@]}"; do
        package="${KFACEAUTH_SELINUX_POLICY_DIR}/${module}.pp"
        [[ -r $package ]] || {
            printf 'kfaceauth-pam-setup: packaged SELinux module is missing: %s\n' "$module" >&2
            return 1
        }
        if ! module_installed "$module"; then
            installed_modules+=("$module")
        fi
        if ! semodule -i "$package"; then
            printf 'kfaceauth-pam-setup: could not install SELinux module %s\n' "$module" >&2
            return 1
        fi
    done
    restorecon_if_present "$KFACEAUTH_RUNTIME_DIRECTORY" || return 1
    [[ ! -e $KFACEAUTH_SOCKET_PATH ]] || restorecon -v "$KFACEAUTH_SOCKET_PATH" || return 1
    restorecon_if_present "$KFACEAUTH_SYSTEM_VAULT_ROOT" || return 1
    restorecon_if_present "$KFACEAUTH_SYSTEM_KEY_ROOT" || return 1
}

install_dedicated_pam_service() {
    local target=$1 service template destination temp
    service=$(target_service "$target")
    template="${KFACEAUTH_PAM_TEMPLATE_DIRECTORY}/${service}"
    destination="${KFACEAUTH_PAM_DIRECTORY}/${service}"
    [[ -d $KFACEAUTH_PAM_DIRECTORY && ! -L $KFACEAUTH_PAM_DIRECTORY ]] || {
        printf 'kfaceauth-pam-setup: PAM service directory is unavailable or unsafe\n' >&2
        return 1
    }
    [[ -f $template && ! -L $template ]] || {
        printf 'kfaceauth-pam-setup: packaged dedicated PAM service is missing or unsafe: %s\n' "$service" >&2
        return 1
    }
    [[ $(stat -c '%u:%g:%a' -- "$template") == 0:0:644 ]] || {
        printf 'kfaceauth-pam-setup: packaged dedicated PAM service has unsafe ownership or permissions: %s\n' "$service" >&2
        return 1
    }
    if [[ -e $destination || -L $destination ]]; then
        [[ -f $destination && ! -L $destination ]] || {
            printf 'kfaceauth-pam-setup: refusing an unsafe existing dedicated PAM service: %s\n' "$service" >&2
            return 1
        }
        [[ $(stat -c '%u:%g:%a' -- "$destination") == 0:0:644 ]] || {
            printf 'kfaceauth-pam-setup: existing dedicated PAM service has unsafe ownership or permissions: %s\n' "$service" >&2
            return 1
        }
        cmp -s -- "$template" "$destination" || {
            printf 'kfaceauth-pam-setup: dedicated PAM service was customized; preserving it: %s\n' "$service" >&2
            return 1
        }
        return 0
    fi

    temp=$(mktemp "${KFACEAUTH_PAM_DIRECTORY}/.${service}.kfaceauth.XXXXXX") || return 1
    if ! cat -- "$template" >"$temp" || ! chmod 0644 "$temp" || ! chown root:root "$temp"; then
        rm -f -- "$temp"
        return 1
    fi
    # Hard-link publication cannot overwrite a service file created concurrently.
    if ! ln -- "$temp" "$destination"; then
        rm -f -- "$temp"
        if [[ -f $destination && ! -L $destination ]] && cmp -s -- "$template" "$destination"; then
            return 0
        fi
        return 1
    fi
    rm -f -- "$temp"
    [[ $(stat -c '%u:%g:%a' -- "$destination") == 0:0:644 ]] || {
        rm -f -- "$destination"
        return 1
    }
    created_pam_service=$destination
}

rollback_prepare() {
    local reason=$1 failed=0
    restore_legacy_pam_backups || failed=1
    if [[ -n $created_pam_service ]]; then
        rm -f -- "$created_pam_service" || failed=1
    fi
    if [[ $socket_state_touched -eq 1 ]]; then
        restore_socket_state || failed=1
    fi
    rollback_modules || failed=1
    if [[ $failed -eq 0 ]]; then
        fail "$reason; the prior authentication state was restored"
    fi
    fail "$reason and rollback was incomplete; inspect the dedicated PAM service, socket, and SELinux state"
}

restore_legacy_pam_backups() {
    local index file backup temp failed=0
    for ((index = ${#legacy_pam_files[@]} - 1; index >= 0; index--)); do
        file=${legacy_pam_files[index]}
        backup=${legacy_pam_backups[index]}
        temp=$(mktemp "${file}.kfaceauth-rollback.XXXXXX") || {
            failed=1
            continue
        }
        if ! cp -p -- "$backup" "$temp" || ! mv -f -- "$temp" "$file"; then
            rm -f -- "$temp"
            failed=1
            continue
        fi
        rm -f -- "$backup" || failed=1
    done
    if [[ $failed -eq 0 ]]; then
        legacy_pam_files=()
        legacy_pam_backups=()
    fi
    return "$failed"
}

migrate_legacy_global_rule() {
    local file=$1 backup temp
    if [[ ! -e $file && ! -L $file ]]; then
        return 0
    fi
    [[ -f $file && ! -L $file ]] || {
        printf 'kfaceauth-pam-setup: refusing an unsafe PAM service during legacy migration: %s\n' "$file" >&2
        return 1
    }

    if ! grep -Fqx "$KFACEAUTH_LEGACY_BEGIN" "$file" && ! grep -Fqx "$KFACEAUTH_LEGACY_END" "$file"; then
        if awk '$1 == "auth" && index($0, "pam_kfaceauth.so") { found = 1 } END { exit !found }' "$file"; then
            printf 'kfaceauth-pam-setup: found an unmanaged global face-authentication rule; preserving %s\n' \
                "$file" >&2
            return 1
        fi
        return 0
    fi

    [[ $(stat -c '%u' -- "$file") == 0 ]] || {
        printf 'kfaceauth-pam-setup: refusing a non-root-owned legacy PAM service: %s\n' "$file" >&2
        return 1
    }
    if ! awk -v begin="$KFACEAUTH_LEGACY_BEGIN" -v end="$KFACEAUTH_LEGACY_END" \
        -v rule='auth        sufficient    pam_kfaceauth.so' '
        $0 == begin {
            if (inside || seen) invalid = 1
            inside = 1
            seen = 1
            next
        }
        $0 == end {
            if (!inside || rules != 1) invalid = 1
            inside = 0
            ended = 1
            next
        }
        inside {
            if ($0 != rule) invalid = 1
            rules++
            next
        }
        $1 == "auth" && index($0, "pam_kfaceauth.so") { invalid = 1 }
        END { if (invalid || !seen || !ended || inside || rules != 1) exit 1 }
    ' "$file"; then
        printf 'kfaceauth-pam-setup: legacy PAM markers or rule are not an exact managed block; preserving %s\n' \
            "$file" >&2
        return 1
    fi

    backup=$(mktemp "${file}.kfaceauth-legacy-backup.XXXXXX") || return 1
    if ! cp -p -- "$file" "$backup"; then
        rm -f -- "$backup"
        return 1
    fi
    legacy_pam_files+=("$file")
    legacy_pam_backups+=("$backup")

    temp=$(mktemp "${file}.kfaceauth-migrate.XXXXXX") || return 1
    if ! cp -p -- "$file" "$temp" || ! awk -v begin="$KFACEAUTH_LEGACY_BEGIN" -v end="$KFACEAUTH_LEGACY_END" '
        $0 == begin { inside = 1; next }
        $0 == end { inside = 0; next }
        !inside { print }
    ' "$file" >"$temp" || ! mv -f -- "$temp" "$file"; then
        rm -f -- "$temp"
        return 1
    fi
}

migrate_legacy_global_rules() {
    migrate_legacy_global_rule "${KFACEAUTH_PAM_DIRECTORY}/sddm" || return 1
    migrate_legacy_global_rule "${KFACEAUTH_PAM_DIRECTORY}/kde" || return 1
    return 0
}

prepare_target() {
    local target=$1 uid=$2 service module_path= path
    has_admin_rights || fail 'preparing an authentication service requires administrator authorization'
    [[ $uid =~ ^[1-9][0-9]{0,9}$ ]] || fail 'a canonical non-root numeric UID is required'
    ((10#$uid <= 4294967295)) || fail 'UID is outside the supported range'
    service=$(target_service "$target")
    [[ -f "${KFACEAUTH_SYSTEM_VAULT_ROOT}/${uid}/identity.vault" ]] || fail 'system login profile is not provisioned'
    [[ -f "${KFACEAUTH_SYSTEM_KEY_ROOT}/${uid}.key" ]] || fail 'system login key is not provisioned'
    for path in "${KFACEAUTH_PAM_MODULE_PATHS[@]}"; do
        if [[ -r $path ]]; then
            module_path=$path
            break
        fi
    done
    [[ -n $module_path ]] || fail 'PAM module is not installed'

    installed_modules=()
    created_pam_service=
    socket_state_touched=0
    capture_socket_state
    if ! install_dedicated_pam_service "$target"; then
        [[ -z $created_pam_service ]] || rm -f -- "$created_pam_service"
        fail "could not prepare the dedicated PAM service ${service}; standard PAM services were left unchanged"
    fi
    if ! install_policy_modules "$target"; then
        rollback_prepare 'SELinux policy preparation failed'
    fi
    socket_state_touched=1
    if ! systemctl enable --now kfaceauth.socket; then
        rollback_prepare 'KFaceAuth socket activation failed'
    fi
    if ! migrate_legacy_global_rules; then
        rollback_prepare 'legacy global PAM migration failed'
    fi
    local index
    for ((index = 0; index < ${#legacy_pam_backups[@]}; index++)); do
        rm -f -- "${legacy_pam_backups[index]}" || true
    done
    legacy_pam_files=()
    legacy_pam_backups=()
    printf 'target=%s state=prepared service=%s\n' "$target" "$service"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    if [[ $# -eq 1 && $1 == --version ]]; then
        printf '%s\n' 'kfaceauth-pam-setup 2'
        exit 0
    fi
    [[ $# -eq 4 && $1 == --prepare-target && $3 == --uid ]] \
        || fail "usage: kfaceauth-pam-setup --prepare-target sddm|plasma-lock --uid UID"
    prepare_target "$2" "$4"
fi
