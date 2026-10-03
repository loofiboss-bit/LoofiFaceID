#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helper script to safely configure KScreenLocker (KDE Lock Screen) and SDDM
# for Windows Hello-style face authentication using pam_kfaceauth.so.

set -euo pipefail

KDE_PAM_FILE="/etc/pam.d/kde"
KDE_BACKUP_FILE="/etc/pam.d/kde.bak.kfaceauth"
SDDM_PAM_FILE="/etc/pam.d/sddm"
SDDM_BACKUP_FILE="/etc/pam.d/sddm.bak.kfaceauth"

PAM_MODULE="pam_kfaceauth.so"
PAM_LINE="auth        sufficient    pam_kfaceauth.so"
SELINUX_POLICY_DIR="/usr/share/kfaceauth/selinux"

install_selinux_policy() {
    local mode
    local build_dir

    if ! command -v getenforce >/dev/null 2>&1; then
        echo "Error: SELinux tools are unavailable; refusing to enable SDDM PAM." >&2
        return 1
    fi

    mode="$(getenforce)"
    if [[ "$mode" == "Disabled" ]]; then
        echo "SELinux is disabled; no KFaceAuth SDDM policy is needed."
        return 0
    fi

    local tool
    for tool in checkmodule semodule_package semodule restorecon; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "Error: $tool is required to install the KFaceAuth SDDM SELinux policy." >&2
            return 1
        fi
    done

    local policy_source="${SELINUX_POLICY_DIR}/kfaceauth_sddm.te"
    local file_contexts="${SELINUX_POLICY_DIR}/kfaceauth_sddm.fc"
    if [[ ! -r "$policy_source" || ! -r "$file_contexts" ]]; then
        echo "Error: KFaceAuth SDDM SELinux policy files are missing from ${SELINUX_POLICY_DIR}." >&2
        return 1
    fi

    build_dir="$(mktemp -d "${TMPDIR:-/tmp}/kfaceauth-selinux.XXXXXXXX")"
    if ! checkmodule -M -m -o "${build_dir}/kfaceauth_sddm.mod" "$policy_source"; then
        rmdir -- "$build_dir"
        echo "Error: Failed to compile the KFaceAuth SDDM SELinux policy." >&2
        return 1
    fi
    if ! semodule_package -o "${build_dir}/kfaceauth_sddm.pp" \
        -m "${build_dir}/kfaceauth_sddm.mod" -f "$file_contexts"; then
        rm -f -- "${build_dir}/kfaceauth_sddm.mod"
        rmdir -- "$build_dir"
        echo "Error: Failed to package the KFaceAuth SDDM SELinux policy." >&2
        return 1
    fi
    if ! semodule -i "${build_dir}/kfaceauth_sddm.pp"; then
        rm -f -- "${build_dir}/kfaceauth_sddm.mod" "${build_dir}/kfaceauth_sddm.pp"
        rmdir -- "$build_dir"
        echo "Error: Failed to install the KFaceAuth SDDM SELinux policy." >&2
        return 1
    fi

    if [[ -e /run/kfaceauth ]] && ! restorecon -R -v /run/kfaceauth; then
        rm -f -- "${build_dir}/kfaceauth_sddm.mod" "${build_dir}/kfaceauth_sddm.pp"
        rmdir -- "$build_dir"
        echo "Error: Policy was installed, but /run/kfaceauth could not be relabeled." >&2
        return 1
    fi

    rm -f -- "${build_dir}/kfaceauth_sddm.mod" "${build_dir}/kfaceauth_sddm.pp"
    rmdir -- "$build_dir"
    echo "Installed the path-scoped KFaceAuth SDDM SELinux policy."
}

remove_selinux_policy() {
    if ! command -v getenforce >/dev/null 2>&1 || [[ "$(getenforce)" == "Disabled" ]]; then
        return 0
    fi
    if ! command -v semodule >/dev/null 2>&1 || ! command -v restorecon >/dev/null 2>&1; then
        echo "Warning: SELinux tools are unavailable; the KFaceAuth policy was left unchanged." >&2
        return 0
    fi
    if semodule -l | awk '$1 == "kfaceauth_sddm" { found = 1 } END { exit !found }'; then
        semodule -r kfaceauth_sddm
        if [[ -e /run/kfaceauth ]]; then
            restorecon -R -v /run/kfaceauth
        fi
    fi
}

status_check() {
    local target_uid="${1:-${UID:-$(id -u)}}"
    local kde_enabled=0
    local sddm_enabled=0
    local enabled=0
    local socket_active=0
    local vault_synced=0

    if [[ -f "$KDE_PAM_FILE" ]] && grep -q "$PAM_MODULE" "$KDE_PAM_FILE" 2>/dev/null; then
        kde_enabled=1
    fi

    if [[ -f "$SDDM_PAM_FILE" ]] && grep -q "$PAM_MODULE" "$SDDM_PAM_FILE" 2>/dev/null; then
        sddm_enabled=1
    fi

    if [[ $kde_enabled -eq 1 ]]; then
        enabled=1
    fi

    if systemctl is-active --quiet kfaceauth.socket 2>/dev/null; then
        socket_active=1
    fi

    if [[ -f "/var/lib/kfaceauth/${target_uid}/identity.vault" || -f "/var/lib/kfaceauth/${target_uid}/vault.bin" ]]; then
        vault_synced=1
    fi

    echo "pam_enabled=$enabled"
    echo "kde_enabled=$kde_enabled"
    echo "sddm_enabled=$sddm_enabled"
    echo "socket_active=$socket_active"
    echo "vault_synced=$vault_synced"
}

configure_target() {
    local pam_file="$1"
    local backup_file="$2"
    local label="$3"

    if [[ ! -f "$pam_file" ]]; then
        echo "Note: $pam_file not present on this system, skipping $label."
        return 0
    fi

    if grep -q "$PAM_MODULE" "$pam_file"; then
        echo "$label PAM already configured for $PAM_MODULE."
    else
        if [[ ! -f "$backup_file" ]]; then
            cp -p "$pam_file" "$backup_file"
        fi

        # Prefer inserting before password-auth substack for clean fail-closed ordering
        if grep -E -q '^[[:space:]]*auth.*substack.*password-auth' "$pam_file"; then
            sed -i "/^[[:space:]]*auth.*substack.*password-auth/i $PAM_LINE" "$pam_file"
        elif grep -E -q '^[[:space:]]*auth' "$pam_file"; then
            sed -i "0,/^[[:space:]]*auth/s//$PAM_LINE\n&/" "$pam_file"
        fi

        # Verify whether the module line exists after sed; if not, prepend to file
        if ! grep -q "$PAM_MODULE" "$pam_file"; then
            local temp_file
            temp_file="$(mktemp)"
            echo "$PAM_LINE" > "$temp_file"
            cat "$pam_file" >> "$temp_file"
            cp "$temp_file" "$pam_file"
            rm -f "$temp_file"
        fi

        if ! grep -q "$PAM_MODULE" "$pam_file"; then
            echo "Error: Failed to insert $PAM_MODULE into $pam_file" >&2
            return 1
        fi
        echo "Configured $pam_file with $PAM_LINE"
    fi
}

remove_target() {
    local pam_file="$1"
    local label="$2"

    if [[ -f "$pam_file" ]] && grep -q "$PAM_MODULE" "$pam_file"; then
        sed -i "\|$PAM_MODULE|d" "$pam_file"
        echo "Removed $PAM_MODULE from $pam_file ($label)."
    fi
}

enable_pam() {
    if [[ $EUID -ne 0 ]]; then
        echo "Error: Enabling PAM requires root privileges (run with sudo or pkexec)." >&2
        exit 1
    fi

    install_selinux_policy

    configure_target "$KDE_PAM_FILE" "$KDE_BACKUP_FILE" "KScreenLocker"
    configure_target "$SDDM_PAM_FILE" "$SDDM_BACKUP_FILE" "SDDM"

    # Enable systemd socket
    systemctl daemon-reload 2>/dev/null || true
    systemctl enable --now kfaceauth.socket 2>/dev/null || true
    echo "kfaceauth.socket enabled and started."
}

disable_pam() {
    if [[ $EUID -ne 0 ]]; then
        echo "Error: Disabling PAM requires root privileges (run with sudo or pkexec)." >&2
        exit 1
    fi

    remove_target "$KDE_PAM_FILE" "KScreenLocker"
    remove_target "$SDDM_PAM_FILE" "SDDM"

    systemctl stop kfaceauth.socket kfaceauth.service 2>/dev/null || true
    systemctl disable kfaceauth.socket 2>/dev/null || true
    remove_selinux_policy
    echo "kfaceauth service and socket disabled."
}

case "${1:-status}" in
    --enable|enable)
        enable_pam
        ;;
    --disable|disable)
        disable_pam
        ;;
    --status|status)
        status_check "${2:-}"
        ;;
    *)
        echo "Usage: $0 [--enable | --disable | --status]" >&2
        exit 1
        ;;
esac
