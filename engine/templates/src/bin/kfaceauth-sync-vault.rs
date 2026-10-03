// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::PathBuf;
use std::process::ExitCode;

use kfaceauth_crypto_openssl_sys::{
    KEY_BYTES, current_uid, seal_master_key, set_socket_permissions,
};
use kfaceauth_templates::{MasterKey, Vault, migrate_legacy_vault};
use zeroize::Zeroize;

fn parse_hex_key(hex: &str) -> Option<[u8; KEY_BYTES]> {
    let hex = hex.trim();
    if hex.len() != KEY_BYTES * 2 {
        return None;
    }
    let mut bytes = [0_u8; KEY_BYTES];
    for (i, byte) in bytes.iter_mut().enumerate() {
        let chunk = &hex[i * 2..i * 2 + 2];
        *byte = u8::from_str_radix(chunk, 16).ok()?;
    }
    Some(bytes)
}

#[allow(clippy::too_many_lines)]
fn main() -> ExitCode {
    let mut target_uid = current_uid();
    let mut custom_legacy_root: Option<PathBuf> = None;
    let mut custom_system_root: Option<PathBuf> = None;
    let mut custom_keys_dir: Option<PathBuf> = None;
    let mut hex_key_arg: Option<String> = None;
    let mut delete_mode = false;
    let mut enable_pam = false;
    let mut disable_pam = false;

    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--uid" => {
                let Some(val) = args.next() else {
                    eprintln!("Missing value for --uid");
                    return ExitCode::FAILURE;
                };
                let Ok(uid) = val.parse::<u32>() else {
                    eprintln!("Invalid UID: {val}");
                    return ExitCode::FAILURE;
                };
                target_uid = uid;
            }
            "--legacy-root" => {
                let Some(val) = args.next() else {
                    eprintln!("Missing value for --legacy-root");
                    return ExitCode::FAILURE;
                };
                custom_legacy_root = Some(PathBuf::from(val));
            }
            "--system-root" => {
                let Some(val) = args.next() else {
                    eprintln!("Missing value for --system-root");
                    return ExitCode::FAILURE;
                };
                custom_system_root = Some(PathBuf::from(val));
            }
            "--keys-dir" => {
                let Some(val) = args.next() else {
                    eprintln!("Missing value for --keys-dir");
                    return ExitCode::FAILURE;
                };
                custom_keys_dir = Some(PathBuf::from(val));
            }
            "--hex-key" => {
                let Some(val) = args.next() else {
                    eprintln!("Missing value for --hex-key");
                    return ExitCode::FAILURE;
                };
                hex_key_arg = Some(val);
            }
            "--delete" => {
                delete_mode = true;
            }
            "--enable-pam" => {
                enable_pam = true;
            }
            "--disable-pam" => {
                disable_pam = true;
            }
            "--help" | "-h" => {
                println!(
                    "Usage: kfaceauth-sync-vault [--uid <UID>] [--legacy-root <DIR>] [--system-root <DIR>] [--keys-dir <DIR>] [--hex-key <KEY>] [--delete] [--enable-pam] [--disable-pam]"
                );
                return ExitCode::SUCCESS;
            }
            unknown => {
                eprintln!("Unknown argument: {unknown}");
                return ExitCode::FAILURE;
            }
        }
    }

    let system_vault_base =
        custom_system_root.unwrap_or_else(|| PathBuf::from("/var/lib/kfaceauth"));
    let system_vault_dir = system_vault_base.join(target_uid.to_string());
    let keys_dir = custom_keys_dir.unwrap_or_else(|| PathBuf::from("/etc/kfaceauth/keys"));
    let key_file = keys_dir.join(format!("{target_uid}.key"));

    let run_pam_setup = |arg: &str| -> bool {
        let candidates = [
            "kfaceauth-pam-setup",
            "/usr/bin/kfaceauth-pam-setup",
            "/usr/local/bin/kfaceauth-pam-setup",
        ];
        for bin in candidates {
            if let Ok(mut child) = std::process::Command::new(bin).arg(arg).spawn() {
                if let Ok(status) = child.wait() {
                    return status.success();
                }
            }
        }
        eprintln!("Warning: kfaceauth-pam-setup executable not found in PATH or /usr/bin");
        false
    };

    if delete_mode {
        if system_vault_dir.exists() {
            let _ = fs::remove_dir_all(&system_vault_dir);
        }
        if key_file.exists() {
            let _ = fs::remove_file(&key_file);
        }
        if disable_pam {
            let _ = run_pam_setup("--disable");
        }
        println!("result=deleted uid={target_uid}");
        return ExitCode::SUCCESS;
    }

    let mut key_input = hex_key_arg.or_else(|| env::var("KFACEAUTH_MASTER_KEY").ok());

    if key_input.is_none() {
        let mut stdin_buf = String::new();
        if let Ok(n) = io::stdin().read_to_string(&mut stdin_buf) {
            if n > 0 {
                key_input = Some(stdin_buf.trim().to_string());
            }
        }
    }

    let Some(mut key_bytes) = key_input.and_then(|val| parse_hex_key(&val)) else {
        eprintln!(
            "Error: Master key must be supplied via --hex-key, KFACEAUTH_MASTER_KEY, or stdin"
        );
        return ExitCode::FAILURE;
    };

    let master_key = MasterKey::from_bytes(key_bytes);

    let legacy_vault = if let Some(ref root) = custom_legacy_root {
        Vault::user_session_with_root(root, target_uid)
    } else {
        let mut found_vault = None;
        if let Ok(passwd) = fs::read_to_string("/etc/passwd") {
            for line in passwd.lines() {
                let parts: Vec<&str> = line.split(':').collect();
                if parts.len() >= 6 && parts[2].parse::<u32>().ok() == Some(target_uid) {
                    let home_dir = PathBuf::from(parts[5]);
                    let user_vault_dir = home_dir
                        .join(".local/share")
                        .join(kfaceauth_templates::PRODUCT_DIRECTORY);
                    if user_vault_dir.exists() {
                        found_vault =
                            Some(Vault::user_session_with_root(&user_vault_dir, target_uid));
                        break;
                    }
                }
            }
        }
        if let Some(v) = found_vault.or_else(|| Vault::production().ok()) {
            v
        } else {
            eprintln!("Failed to locate user session vault for UID {target_uid}");
            key_bytes.zeroize();
            return ExitCode::FAILURE;
        }
    };

    // Ensure /etc/kfaceauth/keys exists and seal master key
    if let Err(e) = seal_master_key(target_uid, &key_bytes, Some(&keys_dir)) {
        eprintln!("Failed to seal master key for UID {target_uid}: {e:?}");
        key_bytes.zeroize();
        return ExitCode::FAILURE;
    }
    key_bytes.zeroize();

    let system_vault = Vault::system_with_root(&system_vault_base, target_uid);

    let summary = match migrate_legacy_vault(&legacy_vault, &system_vault, &master_key) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("Failed to commit system profile: {e}");
            return ExitCode::FAILURE;
        }
    };

    // Ensure system permissions: group kfaceauth can read vault and key
    if let Some(parent) = keys_dir.parent() {
        let _ = set_socket_permissions(parent, 0o755, Some("kfaceauth"));
    }
    let _ = set_socket_permissions(&keys_dir, 0o750, Some("kfaceauth"));
    let _ = set_socket_permissions(&key_file, 0o640, Some("kfaceauth"));

    let _ = set_socket_permissions(&system_vault_base, 0o755, Some("kfaceauth"));
    let _ = std::os::unix::fs::chown(&system_vault_dir, Some(target_uid), None);
    let _ = set_socket_permissions(&system_vault_dir, 0o750, Some("kfaceauth"));
    let vault_file = system_vault_dir.join("identity.vault");
    if vault_file.exists() {
        let _ = std::os::unix::fs::chown(&vault_file, Some(target_uid), None);
        let _ = set_socket_permissions(&vault_file, 0o640, Some("kfaceauth"));
        let bin_file = system_vault_dir.join("vault.bin");
        let _ = fs::copy(&vault_file, &bin_file);
        let _ = std::os::unix::fs::chown(&bin_file, Some(target_uid), None);
        let _ = set_socket_permissions(&bin_file, 0o640, Some("kfaceauth"));
    }

    if enable_pam && !run_pam_setup("--enable") {
        eprintln!("Warning: Failed to configure PAM or systemd socket via kfaceauth-pam-setup");
    }

    println!(
        "result=ok uid={target_uid} samples={}",
        summary.sample_count
    );
    ExitCode::SUCCESS
}
