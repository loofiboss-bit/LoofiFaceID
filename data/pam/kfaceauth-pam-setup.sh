#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helper script to safely configure KScreenLocker (KDE Lock Screen) PAM
# for Windows Hello-style face authentication using pam_kfaceauth.so.

set -euo pipefail

PAM_FILE="/etc/pam.d/kde"
BACKUP_FILE="/etc/pam.d/kde.bak.kfaceauth"
PAM_MODULE="pam_kfaceauth.so"
PAM_LINE="auth        sufficient    pam_kfaceauth.so"

status_check() {
    local target_uid="${1:-${UID:-$(id -u)}}"
    local enabled=0
    local socket_active=0
    local vault_synced=0

    if grep -q "$PAM_MODULE" "$PAM_FILE" 2>/dev/null; then
        enabled=1
    fi

    if systemctl is-active --quiet kfaceauth.socket 2>/dev/null; then
        socket_active=1
    fi

    if [[ -f "/var/lib/kfaceauth/${target_uid}/vault.bin" ]]; then
        vault_synced=1
    fi

    echo "pam_enabled=$enabled"
    echo "socket_active=$socket_active"
    echo "vault_synced=$vault_synced"
}

enable_pam() {
    if [[ $EUID -ne 0 ]]; then
        echo "Error: Enabling PAM requires root privileges (run with sudo or pkexec)." >&2
        exit 1
    fi

    if [[ ! -f "$PAM_FILE" ]]; then
        echo "Error: $PAM_FILE does not exist on this system." >&2
        exit 1
    fi

    if grep -q "$PAM_MODULE" "$PAM_FILE"; then
        echo "KScreenLocker PAM already configured for $PAM_MODULE."
    else
        if [[ ! -f "$BACKUP_FILE" ]]; then
            cp -p "$PAM_FILE" "$BACKUP_FILE"
        fi

        # Insert as the first auth rule in /etc/pam.d/kde
        sed -i "/^auth/i $PAM_LINE" "$PAM_FILE" 2>/dev/null || {
            # Fallback if no line starts with auth
            echo -e "$PAM_LINE\n$(cat "$PAM_FILE")" > "$PAM_FILE"
        }
        echo "Configured $PAM_FILE with $PAM_LINE"
    fi

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

    if [[ -f "$PAM_FILE" ]] && grep -q "$PAM_MODULE" "$PAM_FILE"; then
        sed -i "\|$PAM_MODULE|d" "$PAM_FILE"
        echo "Removed $PAM_MODULE from $PAM_FILE."
    fi

    systemctl stop kfaceauth.socket kfaceauth.service 2>/dev/null || true
    systemctl disable kfaceauth.socket 2>/dev/null || true
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
