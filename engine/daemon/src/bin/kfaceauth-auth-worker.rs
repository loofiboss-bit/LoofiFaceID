// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::io;

fn main() {
    if std::env::args_os().len() != 1 || kfaceauth_vision_opencv_sys::disable_core_dumps().is_err()
    {
        std::process::exit(1);
    }
    if kfaceauth_daemon::serve_auth_worker(&mut io::stdin().lock(), &mut io::stdout().lock())
        .is_err()
    {
        std::process::exit(1);
    }
}
