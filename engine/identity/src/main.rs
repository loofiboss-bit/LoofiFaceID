// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::io;
use std::path::{Path, PathBuf};

use kfaceauth_vision_opencv_sys::configure_worker_sandbox;

const PRODUCTION_MODEL_ROOT: &str = "/usr/share/kfaceauth/models";

fn production_data_root() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("XDG_DATA_HOME") {
        let path = PathBuf::from(path);
        return path.is_absolute().then_some(path);
    }
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .map(|path| path.join(".local/share"))
}

fn main() {
    let arguments: Vec<_> = std::env::args_os().collect();
    let session_mode = match arguments.as_slice() {
        [_] => false,
        [_, opt] if opt == "--session" => true,
        _ => {
            eprintln!("identity worker accepts no command-line parameters or optional --session");
            std::process::exit(2);
        }
    };
    if kfaceauth_vision_opencv_sys::disable_core_dumps().is_err() {
        eprintln!("identity worker hardening failed");
        std::process::exit(1);
    }
    if let Some(data_root) = production_data_root()
        && configure_worker_sandbox(Path::new(PRODUCTION_MODEL_ROOT), Some(&data_root)).is_err()
    {
        eprintln!("identity worker sandbox initialization failed");
        std::process::exit(1);
    }
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let result = if session_mode {
        kfaceauth_identity::serve_session(&mut input, &mut output, Path::new(PRODUCTION_MODEL_ROOT))
    } else {
        kfaceauth_identity::serve_once(&mut input, &mut output, Path::new(PRODUCTION_MODEL_ROOT))
    };
    if result.is_err() {
        eprintln!("identity worker terminated after invalid local protocol I/O");
        std::process::exit(1);
    }
}
