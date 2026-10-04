#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

readonly KDE_PAM_FILE=/etc/pam.d/kde
readonly SDDM_PAM_FILE=/etc/pam.d/sddm
readonly PAM_MODULE=pam_kfaceauth.so
readonly BEGIN_MARKER='# BEGIN kfaceauth experimental authentication'
readonly END_MARKER='# END kfaceauth experimental authentication'
readonly SELINUX_POLICY_DIR=/usr/share/kfaceauth/selinux
readonly SOCKET_PATH=/run/kfaceauth/kfaceauthd.sock

declare -a installed_modules=()
declare -a removed_modules=()
socket_was_enabled=0
socket_was_active=0
service_was_active=0
socket_state_touched=0

fail() {
    printf 'kfaceauth-pam-setup: %s\n' "$1" >&2
    exit 1
}

target_file() {
    case "$1" in
        sddm) printf '%s\n' "$SDDM_PAM_FILE" ;;
        plasma-lock) printf '%s\n' "$KDE_PAM_FILE" ;;
        *) fail 'target must be sddm or plasma-lock' ;;
    esac
}

other_target_file() {
    case "$1" in
        sddm) printf '%s\n' "$KDE_PAM_FILE" ;;
        plasma-lock) printf '%s\n' "$SDDM_PAM_FILE" ;;
        *) fail 'target must be sddm or plasma-lock' ;;
    esac
}

is_managed() {
    local file=$1
    [[ -f "$file" ]] && grep -Fqx "$BEGIN_MARKER" "$file"
}

has_managed_marker() {
    local file=$1
    grep -Fqx "$BEGIN_MARKER" "$file" || grep -Fqx "$END_MARKER" "$file"
}

has_unmanaged_module_line() {
    local file=$1
    awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" -v module="$PAM_MODULE" '
        $0 == begin { managed = 1; next }
        $0 == end { managed = 0; next }
        !managed && $1 == "auth" && index($0, module) { found = 1 }
        END { exit !found }
    ' "$file"
}

managed_rule_is_valid() {
    local file=$1
    awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" \
        -v rule='auth        sufficient    pam_kfaceauth.so' '
        $0 == begin {
            if (inside || seen) invalid = 1
            inside = 1
            seen = 1
            next
        }
        $0 == end {
            if (!inside) invalid = 1
            inside = 0
            closed++
            end_line = NR
            next
        }
        inside {
            if ($0 != rule) invalid = 1
            else rules++
        }
        !inside && NR > end_line && $1 == "auth" && ($2 == "substack" || $2 == "include") && $3 == "password-auth" {
            fallback = 1
        }
        END { exit !(seen && closed == 1 && !inside && !invalid && rules == 1 && fallback) }
    ' "$file" && ! has_unmanaged_module_line "$file"
}

unmanaged_rule_is_adoptable() {
    local file=$1
    awk -v rule='auth        sufficient    pam_kfaceauth.so' -v module="$PAM_MODULE" '
        BEGIN { exact_rule = 1 }
        $1 == "auth" && index($0, module) {
            module_rules++
            if ($0 != rule) exact_rule = 0
            module_line = NR
        }
        $1 == "auth" && ($2 == "substack" || $2 == "include") && $3 == "password-auth" && !fallback_line {
            fallback_line = NR
        }
        END { exit !(module_rules == 1 && exact_rule && fallback_line > module_line) }
    ' "$file"
}

has_password_auth_fallback() {
    local file=$1
    awk '$1 == "auth" && ($2 == "substack" || $2 == "include") && $3 == "password-auth" { found = 1 }
        END { exit !found }' "$file"
}

edit_pam_file() {
    local file=$1
    local action=$2
    local temp
    temp=$(mktemp "${file}.kfaceauth.XXXXXX") || return 1
    cp -p -- "$file" "$temp"

    if [[ $action == enable ]]; then
        if has_managed_marker "$file"; then
            rm -f -- "$temp"
            if is_managed "$file" && managed_rule_is_valid "$file" && ! has_unmanaged_module_line "$file"; then
                return 0
            fi
            printf 'kfaceauth-pam-setup: managed PAM markers are malformed in %s; PAM was left unchanged\n' \
                "$file" >&2
            return 1
        fi
        if has_unmanaged_module_line "$file"; then
            if ! unmanaged_rule_is_adoptable "$file"; then
                rm -f -- "$temp"
                printf 'kfaceauth-pam-setup: unmanaged PAM rule is not the exact safe rule or has no later password fallback in %s; PAM was left unchanged\n' \
                    "$file" >&2
                return 1
            fi
            if ! awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" \
                -v rule='auth        sufficient    pam_kfaceauth.so' '
                $0 == rule && !adopted {
                    print begin
                    print $0
                    print end
                    adopted = 1
                    next
                }
                { print }
                END { if (!adopted) exit 3 }
            ' "$file" >"$temp"; then
                rm -f -- "$temp"
                printf 'kfaceauth-pam-setup: could not adopt the exact PAM rule in %s; PAM was left unchanged\n' \
                    "$file" >&2
                return 1
            fi
        else
            if ! has_password_auth_fallback "$file"; then
                rm -f -- "$temp"
                printf 'kfaceauth-pam-setup: no password-auth fallback was found in %s; PAM was left unchanged\n' \
                    "$file" >&2
                return 1
            fi
            if ! awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" '
                BEGIN { inserted = 0 }
                !inserted && $1 == "auth" && ($2 == "substack" || $2 == "include") && $3 == "password-auth" {
                    print begin
                    print "auth        sufficient    pam_kfaceauth.so"
                    print end
                    inserted = 1
                }
                { print }
                END { if (!inserted) exit 3 }
            ' "$file" >"$temp"; then
                rm -f -- "$temp"
                printf 'kfaceauth-pam-setup: could not locate the password-auth fallback in %s; PAM was left unchanged\n' \
                    "$file" >&2
                return 1
            fi
        fi
        if ! managed_rule_is_valid "$temp"; then
            rm -f -- "$temp"
            printf 'kfaceauth-pam-setup: generated PAM block would not preserve a valid password fallback in %s; PAM was left unchanged\n' \
                "$file" >&2
            return 1
        fi
    else
        if ! is_managed "$file"; then
            rm -f -- "$temp"
            return 0
        fi
        if ! managed_rule_is_valid "$file"; then
            rm -f -- "$temp"
            printf 'kfaceauth-pam-setup: managed PAM block is invalid in %s; PAM was left unchanged\n' \
                "$file" >&2
            return 1
        fi
        if ! awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" '
            $0 == begin {
                if (inside || seen) exit 4
                inside = 1
                seen = 1
                next
            }
            $0 == end {
                if (!inside) exit 4
                inside = 0
                next
            }
            !inside { print }
            END { if (inside) exit 4 }
        ' "$file" >"$temp"; then
            rm -f -- "$temp"
            printf 'kfaceauth-pam-setup: managed PAM markers are malformed in %s; PAM was left unchanged\n' \
                "$file" >&2
            return 1
        fi
    fi

    if ! mv -f -- "$temp" "$file"; then
        rm -f -- "$temp"
        return 1
    fi
}

module_installed() {
    local module=$1
    semodule -l | awk -v module="$module" '$1 == module { found = 1 } END { exit !found }'
}

selinux_enabled() {
    [[ "$(getenforce)" != Disabled ]]
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
    if [[ $target == sddm ]] || module_installed kfaceauth_sddm; then
        # Replace the pre-v5.2 broad runtime-directory rule during activation.
        modules+=(kfaceauth_sddm)
    fi
    for module in "${modules[@]}"; do
        package="${SELINUX_POLICY_DIR}/${module}.pp"
        if [[ ! -r $package ]]; then
            printf 'kfaceauth-pam-setup: packaged SELinux module is missing: %s\n' "$module" >&2
            return 1
        fi
        if ! module_installed "$module"; then
            installed_modules+=("$module")
        fi
        if ! semodule -i "$package"; then
            printf 'kfaceauth-pam-setup: could not install SELinux module %s\n' "$module" >&2
            return 1
        fi
    done

    if [[ -e /run/kfaceauth ]]; then
        if ! restorecon -R -v /run/kfaceauth; then
            printf 'kfaceauth-pam-setup: could not restore runtime-directory labels\n' >&2
            return 1
        fi
    fi
}

restore_installed_modules() {
    local module index failed=0
    for ((index = ${#installed_modules[@]} - 1; index >= 0; index--)); do
        module=${installed_modules[index]}
        if module_installed "$module"; then
            semodule -r "$module" || failed=1
        fi
    done
    return "$failed"
}

remove_policy_module() {
    local module=$1
    if module_installed "$module"; then
        removed_modules+=("$module")
        semodule -r "$module"
    fi
}

restore_removed_modules() {
    local module index failed=0
    for ((index = ${#removed_modules[@]} - 1; index >= 0; index--)); do
        module=${removed_modules[index]}
        semodule -i "${SELINUX_POLICY_DIR}/${module}.pp" || failed=1
    done
    return "$failed"
}

has_other_target() {
    local file
    file=$(other_target_file "$1")
    [[ -f $file ]] || return 1
    is_managed "$file" || has_unmanaged_module_line "$file"
}

restore_pam_backup() {
    local file=$1
    local backup=$2
    if [[ -e $backup ]]; then
        cp -p -- "$backup" "$file"
    fi
}

capture_socket_state() {
    socket_was_enabled=0
    socket_was_active=0
    service_was_active=0
    socket_state_touched=0
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

rollback_enable() {
    local file=$1
    local backup=$2
    local failed=0
    restore_pam_backup "$file" "$backup" || failed=1
    if [[ $socket_state_touched -eq 1 ]]; then
        restore_socket_state || failed=1
    fi
    restore_installed_modules || failed=1
    rm -f -- "$backup"
    if [[ $failed -eq 0 ]]; then
        fail 'activation failed; the previous PAM and service states were restored'
    fi
    fail 'activation failed and rollback was incomplete; inspect PAM, systemd, and SELinux state'
}

enable_target() {
    local target=$1
    local uid=$2
    local file backup
    file=$(target_file "$target")
    [[ $EUID -eq 0 ]] || fail 'enabling PAM requires administrator authorization'
    [[ $uid =~ ^[0-9]+$ ]] || fail 'a numeric target UID is required'
    [[ -f $file ]] || fail "PAM service file is missing: ${file}"
    [[ -f "/var/lib/kfaceauth/${uid}/identity.vault" ]] || fail 'system login profile is not provisioned'
    [[ -f "/etc/kfaceauth/keys/${uid}.key" ]] || fail 'system login key is not provisioned'
    [[ -r /usr/lib64/security/pam_kfaceauth.so || -r /usr/lib/security/pam_kfaceauth.so ]] || fail 'PAM module is not installed'
    capture_socket_state

    backup=$(mktemp "${file}.kfaceauth-backup.XXXXXX") || fail 'could not create PAM rollback copy'
    cp -p -- "$file" "$backup" || fail 'could not preserve the current PAM stack'

    if ! install_policy_modules "$target"; then
        if ! restore_installed_modules; then
            rm -f -- "$backup"
            fail 'SELinux policy activation failed and policy rollback was incomplete; inspect SELinux state'
        fi
        rm -f -- "$backup"
        fail 'SELinux policy activation failed; PAM was left unchanged'
    fi
    if ! edit_pam_file "$file" enable; then
        rollback_enable "$file" "$backup"
    fi
    if ! systemctl daemon-reload; then
        rollback_enable "$file" "$backup"
    fi
    socket_state_touched=1
    if ! systemctl enable --now kfaceauth.socket; then
        rollback_enable "$file" "$backup"
    fi
    if selinux_enabled && [[ -e $SOCKET_PATH ]]; then
        if ! restorecon -v "$SOCKET_PATH"; then
            rollback_enable "$file" "$backup"
        fi
    fi
    rm -f -- "$backup"
    printf 'target=%s state=enabled\n' "$target"
}

disable_target() {
    local target=$1
    local file backup
    file=$(target_file "$target")
    [[ $EUID -eq 0 ]] || fail 'disabling PAM requires administrator authorization'
    [[ -f $file ]] || fail "PAM service file is missing: ${file}"

    if ! is_managed "$file"; then
        if has_unmanaged_module_line "$file"; then
            fail "refusing to disable an unmanaged PAM rule in ${file}"
        fi
        printf 'target=%s state=disabled\n' "$target"
        return 0
    fi

    capture_socket_state
    backup=$(mktemp "${file}.kfaceauth-backup.XXXXXX") || fail 'could not create PAM rollback copy'
    cp -p -- "$file" "$backup" || fail 'could not preserve the current PAM stack'
    if ! edit_pam_file "$file" disable; then
        rm -f -- "$backup"
        return 1
    fi

    if ! has_other_target "$target"; then
        socket_state_touched=1
        if ! systemctl disable --now kfaceauth.socket; then
            local rollback_failed=0
            restore_pam_backup "$file" "$backup" || rollback_failed=1
            restore_socket_state || rollback_failed=1
            rm -f -- "$backup"
            [[ $rollback_failed -eq 0 ]] || fail 'service deactivation failed and rollback was incomplete; inspect PAM and systemd state'
            fail 'service deactivation failed; the previous PAM file was restored'
        fi
        if command -v semodule >/dev/null 2>&1 && command -v getenforce >/dev/null 2>&1 \
            && [[ "$(getenforce)" != Disabled ]]; then
            removed_modules=()
            if ! remove_policy_module kfaceauth_sddm || ! remove_policy_module kfaceauth; then
                local rollback_failed=0
                restore_removed_modules || rollback_failed=1
                restore_pam_backup "$file" "$backup" || rollback_failed=1
                restore_socket_state || rollback_failed=1
                rm -f -- "$backup"
                [[ $rollback_failed -eq 0 ]] || fail 'policy removal failed and rollback was incomplete; inspect system state'
                fail 'SELinux policy removal failed; prior PAM and service states were restored'
            fi
            if [[ -e /run/kfaceauth ]] && ! restorecon -R -v /run/kfaceauth; then
                local rollback_failed=0
                restore_removed_modules || rollback_failed=1
                restore_pam_backup "$file" "$backup" || rollback_failed=1
                restore_socket_state || rollback_failed=1
                rm -f -- "$backup"
                [[ $rollback_failed -eq 0 ]] || fail 'runtime relabeling failed and rollback was incomplete; inspect system state'
                fail 'runtime relabeling failed; prior PAM and service states were restored'
            fi
        fi
    elif [[ $target == sddm ]] \
        && command -v semodule >/dev/null 2>&1 \
        && command -v getenforce >/dev/null 2>&1 \
        && [[ "$(getenforce)" != Disabled ]]; then
        removed_modules=()
        if ! remove_policy_module kfaceauth_sddm; then
            local rollback_failed=0
            restore_removed_modules || rollback_failed=1
            restore_pam_backup "$file" "$backup" || rollback_failed=1
            rm -f -- "$backup"
            [[ $rollback_failed -eq 0 ]] || fail 'SDDM policy removal failed and rollback was incomplete; inspect system state'
            fail 'SDDM SELinux policy removal failed; the previous PAM file was restored'
        fi
    fi
    rm -f -- "$backup"
    printf 'target=%s state=disabled\n' "$target"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    [[ $# -ge 2 ]] || fail 'usage: kfaceauth-pam-setup --enable-target sddm|plasma-lock --uid UID | --disable-target sddm|plasma-lock'
    case "$1" in
        --enable-target)
            [[ $# -eq 4 && $3 == --uid ]] || fail 'usage: --enable-target sddm|plasma-lock --uid UID'
            enable_target "$2" "$4"
            ;;
        --disable-target)
            [[ $# -eq 2 ]] || fail 'usage: --disable-target sddm|plasma-lock'
            disable_target "$2"
            ;;
        *) fail 'usage: --enable-target sddm|plasma-lock --uid UID | --disable-target sddm|plasma-lock' ;;
    esac
fi
