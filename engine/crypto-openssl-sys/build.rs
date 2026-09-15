// SPDX-License-Identifier: GPL-3.0-or-later

use std::env;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

fn main() {
    println!("cargo:rerun-if-changed=native/crypto_bridge.c");
    println!("cargo:rerun-if-changed=native/crypto_bridge.h");
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=CFLAGS");
    println!("cargo:rerun-if-env-changed=AR");
    println!("cargo:rerun-if-env-changed=KFACEAUTH_SOURCE_ROOT");

    let version = command_output("pkg-config", ["--modversion", "openssl"]);
    let version = String::from_utf8(version.stdout).expect("OpenSSL version must be UTF-8");
    let major = version
        .trim()
        .split('.')
        .next()
        .and_then(|value| value.parse::<u32>().ok());
    assert!(
        major.is_some_and(|value| value >= 3),
        "KFaceAuth requires Fedora OpenSSL 3.x or newer, found {}",
        version.trim()
    );

    let include_output = command_output("pkg-config", ["--cflags-only-I", "openssl"]);
    let include_flags =
        String::from_utf8(include_output.stdout).expect("OpenSSL include flags must be UTF-8");
    let output_directory =
        PathBuf::from(env::var_os("OUT_DIR").expect("Cargo must provide OUT_DIR"));
    let object = output_directory.join("crypto_bridge.o");
    let archive = output_directory.join("libkfaceauth_crypto_openssl.a");

    let compiler = env::var_os("CC").unwrap_or_else(|| OsString::from("cc"));
    let mut compile = Command::new(compiler);
    compile
        .arg("-std=c17")
        .arg("-fPIC")
        .arg("-fvisibility=hidden")
        .arg("-Wall")
        .arg("-Wextra")
        .arg("-Wconversion")
        .arg("-Werror")
        .arg("-c")
        .arg(Path::new("native/crypto_bridge.c"))
        .arg("-o")
        .arg(&object);
    for flag in include_flags.split_whitespace() {
        if let Some(path) = flag.strip_prefix("-I") {
            compile.arg("-isystem").arg(path);
        } else {
            compile.arg(flag);
        }
    }

    let has_tpm2 = Command::new("pkg-config")
        .args(["--exists", "tss2-esys", "tss2-mu"])
        .status()
        .is_ok_and(|s| s.success());

    if has_tpm2 {
        compile.arg("-DKFACEAUTH_HAS_TPM2=1");
        let tpm_include_output = command_output("pkg-config", ["--cflags-only-I", "tss2-esys"]);
        let tpm_include_flags =
            String::from_utf8(tpm_include_output.stdout).expect("TSS2 include flags must be UTF-8");
        for flag in tpm_include_flags.split_whitespace() {
            if let Some(path) = flag.strip_prefix("-I") {
                compile.arg("-isystem").arg(path);
            } else {
                compile.arg(flag);
            }
        }
    }

    let has_keyutils = Path::new("/usr/include/keyutils.h").exists();
    if has_keyutils {
        compile.arg("-DKFACEAUTH_HAS_KEYUTILS=1");
    }

    add_source_remapping(&mut compile);
    if let Some(flags) = env::var_os("CFLAGS") {
        for flag in flags.to_string_lossy().split_whitespace() {
            compile.arg(flag);
        }
    }
    run(&mut compile, "compile the reviewed OpenSSL C ABI bridge");

    let archiver = env::var_os("AR").unwrap_or_else(|| OsString::from("ar"));
    let mut archive_command = Command::new(archiver);
    archive_command.arg("crsD").arg(&archive).arg(&object);
    run(
        &mut archive_command,
        "archive the reviewed OpenSSL C ABI bridge",
    );

    println!(
        "cargo:rustc-link-search=native={}",
        output_directory.display()
    );
    println!("cargo:rustc-link-lib=static=kfaceauth_crypto_openssl");
    println!("cargo:rustc-link-lib=dylib=crypto");
    if has_tpm2 {
        println!("cargo:rustc-link-lib=dylib=tss2-esys");
        println!("cargo:rustc-link-lib=dylib=tss2-mu");
    }
    if has_keyutils {
        println!("cargo:rustc-link-lib=dylib=keyutils");
    }
}

fn add_source_remapping(command: &mut Command) {
    if let Some(root) = env::var_os("KFACEAUTH_SOURCE_ROOT") {
        command
            .arg(format!(
                "-ffile-prefix-map={}=",
                PathBuf::from(&root).display()
            ))
            .arg(format!(
                "-fdebug-prefix-map={}=",
                PathBuf::from(root).display()
            ));
    }
}

fn command_output<I, S>(program: &str, arguments: I) -> Output
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let output = Command::new(program)
        .args(arguments)
        .output()
        .unwrap_or_else(|error| panic!("failed to execute {program}: {error}"));
    assert!(
        output.status.success(),
        "{program} failed: {}",
        String::from_utf8_lossy(&output.stderr).trim()
    );
    output
}

fn run(command: &mut Command, purpose: &str) {
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("failed to {purpose}: {error}"));
    assert!(status.success(), "failed to {purpose}");
}
