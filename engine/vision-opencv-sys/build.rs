// SPDX-License-Identifier: GPL-3.0-or-later

use std::env;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const OPENCV_LIBRARIES: &[&str] = &[
    "opencv_calib3d",
    "opencv_objdetect",
    "opencv_dnn",
    "opencv_flann",
    "opencv_imgproc",
    "opencv_core",
];

fn main() {
    println!("cargo:rerun-if-changed=native/yunet_bridge.cpp");
    println!("cargo:rerun-if-changed=native/yunet_bridge.h");
    println!("cargo:rerun-if-changed=native/yunet_output_validation.h");
    println!("cargo:rerun-if-env-changed=CXX");
    println!("cargo:rerun-if-env-changed=CXXFLAGS");
    println!("cargo:rerun-if-env-changed=AR");
    println!("cargo:rerun-if-env-changed=KFACEAUTH_SOURCE_ROOT");
    println!("cargo:rerun-if-env-changed=KFACEAUTH_HAS_OPENVINO");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_SYSROOT_DIR");

    let version = command_output("pkg-config", ["--modversion", "opencv4"]);
    let version = String::from_utf8(version.stdout).expect("OpenCV version must be UTF-8");
    let mut components = version.trim().split('.');
    let major = components
        .next()
        .and_then(|value| value.parse::<u32>().ok());
    let minor = components
        .next()
        .and_then(|value| value.parse::<u32>().ok());
    assert!(
        major == Some(4) && minor.is_some_and(|minor| minor >= 8),
        "KFaceAuth requires OpenCV >= 4.8.0, found {}",
        version.trim()
    );

    let include_output = command_output("pkg-config", ["--cflags-only-I", "opencv4"]);
    let include_flags =
        String::from_utf8(include_output.stdout).expect("OpenCV include flags must be UTF-8");
    let output_directory =
        PathBuf::from(env::var_os("OUT_DIR").expect("Cargo must provide OUT_DIR"));
    let object = output_directory.join("yunet_bridge.o");
    let archive = output_directory.join("libkfaceauth_yunet_bridge.a");
    let source = Path::new("native/yunet_bridge.cpp");

    let compiler = env::var_os("CXX").unwrap_or_else(|| OsString::from("c++"));
    let mut compile = Command::new(compiler);
    compile
        .arg("-std=c++20")
        .arg("-fPIC")
        .arg("-fvisibility=hidden")
        .arg("-Wall")
        .arg("-Wextra")
        .arg("-Wconversion")
        .arg("-Werror")
        .arg("-c")
        .arg(source)
        .arg("-o")
        .arg(&object);
    for flag in include_flags.split_whitespace() {
        if let Some(path) = flag.strip_prefix("-I") {
            compile.arg("-isystem").arg(path);
        } else {
            compile.arg(flag);
        }
    }
    add_source_remapping(&mut compile);
    if let Some(flags) = env::var_os("CXXFLAGS") {
        for flag in flags.to_string_lossy().split_whitespace() {
            compile.arg(flag);
        }
    }
    let has_openvino = env::var("KFACEAUTH_HAS_OPENVINO").is_ok_and(|value| value == "1")
        || command_succeeded("pkg-config", ["--exists", "openvino"]);
    if has_openvino {
        compile.arg("-DKFACEAUTH_COMPILED_OPENVINO=1");
    }
    run(&mut compile, "compile the reviewed YuNet C ABI bridge");

    let archiver = env::var_os("AR").unwrap_or_else(|| OsString::from("ar"));
    let mut archive_command = Command::new(archiver);
    archive_command.arg("crsD").arg(&archive).arg(&object);
    run(
        &mut archive_command,
        "archive the reviewed YuNet C ABI bridge",
    );

    println!(
        "cargo:rustc-link-search=native={}",
        output_directory.display()
    );
    println!("cargo:rustc-link-lib=static=kfaceauth_yunet_bridge");
    let library_output = command_output("pkg-config", ["--libs-only-L", "opencv4"]);
    let library_flags =
        String::from_utf8(library_output.stdout).expect("OpenCV library flags must be UTF-8");
    for flag in library_flags.split_whitespace() {
        if let Some(path) = flag.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={path}");
        }
    }
    for library in OPENCV_LIBRARIES {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
    println!("cargo:rustc-link-lib=dylib=stdc++");
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

fn command_succeeded<I, S>(program: &str, arguments: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    Command::new(program)
        .args(arguments)
        .status()
        .is_ok_and(|status| status.success())
}
