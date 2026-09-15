// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::env;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use kfaceauth_crypto_openssl_sys::{KEY_BYTES, current_uid};
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

fn main() -> ExitCode {
    let mut target_uid = current_uid();
    let mut custom_legacy_root: Option<PathBuf> = None;
    let mut custom_system_root: Option<PathBuf> = None;
    let mut hex_key_arg: Option<String> = None;

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
            "--hex-key" => {
                let Some(val) = args.next() else {
                    eprintln!("Missing value for --hex-key");
                    return ExitCode::FAILURE;
                };
                hex_key_arg = Some(val);
            }
            "--help" | "-h" => {
                println!(
                    "Usage: kfaceauth-migrate-vault [--uid <UID>] [--legacy-root <DIR>] [--system-root <DIR>] [--hex-key <KEY>]"
                );
                return ExitCode::SUCCESS;
            }
            unknown => {
                eprintln!("Unknown argument: {unknown}");
                return ExitCode::FAILURE;
            }
        }
    }

    let Some(mut key_bytes) = hex_key_arg
        .or_else(|| env::var("KFACEAUTH_MASTER_KEY").ok())
        .and_then(|val| parse_hex_key(&val))
    else {
        eprintln!("No master key provided via --hex-key or KFACEAUTH_MASTER_KEY");
        return ExitCode::FAILURE;
    };

    let master_key = MasterKey::from_bytes(key_bytes);
    key_bytes.zeroize();

    let legacy_vault = match custom_legacy_root {
        Some(ref root) => {
            Vault::system_with_root(root.parent().unwrap_or_else(|| Path::new(".")), target_uid)
        }
        None => match Vault::production() {
            Ok(vault) => vault,
            Err(err) => {
                eprintln!("Failed to locate legacy production vault: {err}");
                return ExitCode::FAILURE;
            }
        },
    };

    let system_vault = match custom_system_root {
        Some(ref root) => Vault::system_with_root(root, target_uid),
        None => match Vault::system(target_uid) {
            Ok(vault) => vault,
            Err(err) => {
                eprintln!("Failed to determine system vault destination: {err}");
                return ExitCode::FAILURE;
            }
        },
    };

    match migrate_legacy_vault(&legacy_vault, &system_vault, &master_key) {
        Ok(summary) => {
            println!(
                "Successfully migrated vault to {} (sample_count: {})",
                system_vault.root().display(),
                summary.sample_count
            );
            ExitCode::SUCCESS
        }
        Err(err) => {
            eprintln!("Migration failed: {err}");
            ExitCode::FAILURE
        }
    }
}
