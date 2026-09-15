// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::io;
use std::path::{Path, PathBuf};

use kfaceauth_vision_opencv_sys::{configure_worker_sandbox, install_seccomp};

fn initialize_provider(
    model_root: &Path,
) -> Result<kfaceauth_vision::yunet::YuNetProvider, kfaceauth_vision::worker::WorkerErrorCode> {
    configure_worker_sandbox(model_root, None)
        .map_err(|_| kfaceauth_vision::worker::WorkerErrorCode::InternalError)?;
    let provider = kfaceauth_vision::yunet::YuNetProvider::from_model_root(model_root)
        .map_err(|_| kfaceauth_vision::worker::WorkerErrorCode::ModelUnavailable)?;
    if std::env::var_os("KFACEAUTH_ENABLE_SECCOMP_SANDBOX").is_some() {
        install_seccomp(provider.uses_vulkan())
            .map_err(|_| kfaceauth_vision::worker::WorkerErrorCode::InternalError)?;
    }
    Ok(provider)
}

fn main() {
    let arguments: Vec<_> = std::env::args_os().collect();
    let (model_root, session_mode) = match arguments.as_slice() {
        [_, flag, root] if flag == "--model-root" => (PathBuf::from(root), false),
        [_, flag, root, opt] if flag == "--model-root" && opt == "--session" => {
            (PathBuf::from(root), true)
        }
        [_, opt, flag, root] if flag == "--model-root" && opt == "--session" => {
            (PathBuf::from(root), true)
        }
        _ => {
            eprintln!("usage: kfaceauth-vision-worker --model-root ABSOLUTE_ROOT [--session]");
            std::process::exit(2);
        }
    };
    if !model_root.is_absolute() {
        eprintln!("vision worker initialization failed");
        std::process::exit(2);
    }
    let hardening = kfaceauth_vision_opencv_sys::disable_core_dumps();

    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let result = if session_mode {
        kfaceauth_vision::worker::serve_session_with_provider_factory(
            &mut input,
            &mut output,
            move || {
                hardening.map_err(|_| kfaceauth_vision::worker::WorkerErrorCode::InternalError)?;
                initialize_provider(&model_root)
            },
        )
    } else {
        kfaceauth_vision::worker::serve_once_with_provider_factory(
            &mut input,
            &mut output,
            move || {
                hardening.map_err(|_| kfaceauth_vision::worker::WorkerErrorCode::InternalError)?;
                initialize_provider(&model_root)
            },
        )
    };
    if result.is_err() {
        eprintln!("vision worker terminated after invalid local protocol I/O");
        std::process::exit(1);
    }
}
