// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::io;
use std::path::Path;

const PRODUCTION_MODEL_ROOT: &str = "/usr/share/kfaceauth/models";

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
