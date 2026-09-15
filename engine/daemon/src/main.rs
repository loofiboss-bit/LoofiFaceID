// SPDX-License-Identifier: GPL-3.0-or-later

//! Binary entry point for the standalone system daemon `kfaceauthd`.

#![forbid(unsafe_code)]

use std::fs;
use std::os::unix::net::UnixListener;
use std::path::PathBuf;

use kfaceauth_daemon::{
    DEFAULT_DAEMON_GROUP, DEFAULT_DAEMON_USER, DEFAULT_SOCKET_PATH, DaemonConfig, SOCKET_FILE_MODE,
    run_daemon_loop,
};

fn main() {
    let listener = match kfaceauth_crypto_openssl_sys::systemd_socket_listener() {
        Ok(Some(l)) => l,
        Ok(None) => {
            let socket_path = std::env::var_os("KFACEAUTH_SOCKET_PATH")
                .map_or_else(|| PathBuf::from(DEFAULT_SOCKET_PATH), PathBuf::from);

            if let Some(parent) = socket_path.parent() {
                let _ = fs::create_dir_all(parent);
            }
            if socket_path.exists() {
                let _ = fs::remove_file(&socket_path);
            }

            let l = match UnixListener::bind(&socket_path) {
                Ok(listener) => listener,
                Err(err) => {
                    eprintln!("failed to bind socket at {}: {err}", socket_path.display());
                    std::process::exit(1);
                }
            };

            // Set socket file permissions
            let _ = kfaceauth_crypto_openssl_sys::set_socket_permissions(
                &socket_path,
                SOCKET_FILE_MODE,
                Some(DEFAULT_DAEMON_GROUP),
            );

            l
        }
        Err(err) => {
            eprintln!("failed to query socket activation: {err}");
            std::process::exit(1);
        }
    };

    // Gate 4.1: Daemon drops root privileges immediately upon socket creation;
    // worker executes strictly as kfaceauth:kfaceauth without requiring CAP_DAC_OVERRIDE.
    if let Err(err) =
        kfaceauth_crypto_openssl_sys::drop_privileges(DEFAULT_DAEMON_USER, DEFAULT_DAEMON_GROUP)
    {
        eprintln!(
            "failed to drop root privileges to {DEFAULT_DAEMON_USER}:{DEFAULT_DAEMON_GROUP}: {err}"
        );
        std::process::exit(1);
    }

    let config = DaemonConfig::from_env();
    if let Err(err) = run_daemon_loop(&listener, &config) {
        eprintln!("daemon loop terminated with error: {err}");
        std::process::exit(1);
    }
}
