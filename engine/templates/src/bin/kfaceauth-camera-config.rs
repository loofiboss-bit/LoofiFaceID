// SPDX-License-Identifier: GPL-3.0-or-later
#![forbid(unsafe_code)]

use kfaceauth_crypto_openssl_sys::{
    camera_metadata, effective_uid, lock_child_file_nonblocking, open_child_directory_nofollow,
    open_child_file_nofollow, open_directory_nofollow,
};
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::time::{Duration, Instant};

const KEY: &str = "KFACEAUTH_CAMERA_DEVICE";
const MAX_CONFIG: u64 = 65536;
const CONFIG: &str = "kfaceauth.conf";
const SYSTEMCTL: &str = "/usr/bin/systemctl";

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut text, byte| {
        use std::fmt::Write as _;
        let _ = write!(&mut text, "{byte:02x}");
        text
    })
}

fn cameras() -> Vec<(PathBuf, String)> {
    let mut result = Vec::new();
    let Ok(entries) = fs::read_dir("/dev") else {
        return result;
    };
    let mut nodes: Vec<_> = entries
        .take(4096)
        .filter_map(Result::ok)
        .filter(|entry| {
            entry.file_name().to_str().is_some_and(|name| {
                name.strip_prefix("video").is_some_and(|tail| {
                    !tail.is_empty() && tail.bytes().all(|b| b.is_ascii_digit())
                })
            })
        })
        .map(|entry| entry.path())
        .collect();
    nodes.sort();
    for node in nodes.into_iter().take(128) {
        if let Ok(label) = camera_metadata(&node) {
            let mut stable = Vec::new();
            if let Ok(entries) = fs::read_dir("/dev/v4l/by-path") {
                for entry in entries.take(4096).filter_map(Result::ok) {
                    if fs::canonicalize(entry.path()).ok().as_ref() == Some(&node) {
                        stable.push(entry.path());
                    }
                }
            }
            stable.sort();
            result.push((stable.into_iter().next().unwrap_or(node), label));
        }
    }
    result
}

fn safe_directory(path: &Path, uid: u32) -> Result<File, &'static str> {
    let directory = open_directory_nofollow(path).map_err(|_| "unsafe-directory")?;
    validate_directory(directory, uid)
}

fn validate_directory(directory: File, uid: u32) -> Result<File, &'static str> {
    let metadata = directory.metadata().map_err(|_| "unsafe-directory")?;
    if metadata.uid() != uid || metadata.mode() & 0o022 != 0 {
        return Err("unsafe-directory");
    }
    Ok(directory)
}

fn provision_config_directory(parent: &File, owner: u32) -> Result<File, &'static str> {
    // Writes use the anchored parent; opening its child never traverses /proc symlinks.
    let path = PathBuf::from(format!("/proc/self/fd/{}", parent.as_raw_fd())).join("kfaceauth");
    match fs::create_dir(&path) {
        Ok(()) => parent.sync_all().map_err(|_| "unsafe-directory")?,
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
        Err(_) => return Err("unsafe-directory"),
    }
    let child =
        open_child_directory_nofollow(parent, "kfaceauth").map_err(|_| "unsafe-directory")?;
    validate_directory(child, owner)
}

fn read_config(directory: &File, uid: u32) -> Result<Option<Vec<u8>>, &'static str> {
    let path = PathBuf::from(format!("/proc/self/fd/{}", directory.as_raw_fd())).join(CONFIG);
    if fs::symlink_metadata(&path).is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
    {
        return Ok(None);
    }
    let mut file = open_child_file_nofollow(directory, CONFIG).map_err(|_| "unsafe-config")?;
    let meta = file.metadata().map_err(|_| "unsafe-config")?;
    if !meta.is_file()
        || meta.uid() != uid
        || meta.nlink() != 1
        || meta.mode() & 0o022 != 0
        || meta.len() > MAX_CONFIG
    {
        return Err("unsafe-config");
    }
    let mut bytes = Vec::new();
    std::io::Read::by_ref(&mut file)
        .take(MAX_CONFIG + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "unreadable-config")?;
    if bytes.len() as u64 > MAX_CONFIG || std::str::from_utf8(&bytes).is_err() {
        return Err("invalid-config");
    }
    let after = file.metadata().map_err(|_| "unsafe-config")?;
    let current = open_child_file_nofollow(directory, CONFIG)
        .map_err(|_| "unsafe-config")?
        .metadata()
        .map_err(|_| "unsafe-config")?;
    if meta.dev() != current.dev()
        || meta.ino() != current.ino()
        || meta.len() != after.len()
        || meta.mtime() != after.mtime()
        || meta.mtime_nsec() != after.mtime_nsec()
        || meta.ctime() != after.ctime()
        || meta.ctime_nsec() != after.ctime_nsec()
    {
        return Err("unsafe-config");
    }
    Ok(Some(bytes))
}

// Preserve unsupported EnvironmentFile constructs by rejecting them before mutation.
// systemd permits multiline quoted values and escaped newlines; a physical-line
// editor cannot safely remove an apparent camera assignment inside those values.
fn validate_lines(text: &str) -> Result<(), &'static str> {
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with(['#', ';']) {
            continue;
        }
        let Some((_, value)) = line.split_once('=') else {
            continue;
        };
        let mut quote = None;
        let mut escaped = false;
        for character in value.chars() {
            if escaped {
                escaped = false;
                continue;
            }
            if character == '\\' && quote != Some('\'') {
                escaped = true;
                continue;
            }
            if matches!(character, '\'' | '"') {
                if quote == Some(character) {
                    quote = None;
                } else if quote.is_none() {
                    quote = Some(character);
                }
            }
        }
        if quote.is_some() || escaped {
            return Err("unsupported-config");
        }
    }
    Ok(())
}

fn setting(bytes: &[u8]) -> Result<Option<String>, &'static str> {
    let text = std::str::from_utf8(bytes).map_err(|_| "invalid-config")?;
    validate_lines(text)?;
    let mut value = None;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if let Some((name, entry)) = line.split_once('=') {
            if name.trim() == KEY {
                if value.is_some() {
                    return Err("duplicate-setting");
                }
                value = Some(entry.trim().trim_matches('"').trim_matches('\'').to_owned());
            }
        }
    }
    Ok(value.filter(|item| !item.is_empty()))
}

fn replacement(bytes: &[u8], selected: Option<&str>) -> Result<Vec<u8>, &'static str> {
    setting(bytes)?;
    let text = std::str::from_utf8(bytes).map_err(|_| "invalid-config")?;
    let mut result = String::new();
    for line in text.split_inclusive('\n') {
        let trimmed = line.trim();
        if trimmed
            .split_once('=')
            .is_some_and(|(name, _)| name.trim() == KEY)
        {
            continue;
        }
        result.push_str(line);
    }
    if let Some(selected) = selected {
        if !result.is_empty() && !result.ends_with('\n') {
            result.push('\n');
        }
        result.push_str(KEY);
        result.push('=');
        result.push_str(selected);
        result.push('\n');
    }
    Ok(result.into_bytes())
}

fn atomic_write(directory: &File, bytes: Option<&[u8]>) -> Result<(), &'static str> {
    let base = PathBuf::from(format!("/proc/self/fd/{}", directory.as_raw_fd()));
    if let Some(bytes) = bytes {
        let temporary = base.join(format!(".camera-config-{}.tmp", std::process::id()));
        let result = (|| {
            let previous_metadata = fs::symlink_metadata(base.join(CONFIG)).ok();
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(&temporary)
                .map_err(|_| "staging-failed")?;
            file.write_all(bytes).map_err(|_| "staging-failed")?;
            if let Some(metadata) = previous_metadata {
                std::os::unix::fs::chown(&temporary, None, Some(metadata.gid()))
                    .map_err(|_| "staging-failed")?;
                file.set_permissions(fs::Permissions::from_mode(metadata.mode() & 0o777))
                    .map_err(|_| "staging-failed")?;
            } else {
                file.set_permissions(fs::Permissions::from_mode(0o644))
                    .map_err(|_| "staging-failed")?;
            }
            file.sync_all().map_err(|_| "staging-failed")?;
            fs::rename(&temporary, base.join(CONFIG)).map_err(|_| "install-failed")?;
            directory.sync_all().map_err(|_| "install-failed")
        })();
        if result.is_err() {
            let _ = fs::remove_file(&temporary);
        }
        result
    } else {
        match fs::remove_file(base.join(CONFIG)) {
            Ok(()) => directory.sync_all().map_err(|_| "rollback-failed"),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(_) => Err("rollback-failed"),
        }
    }
}

fn service(action: &str) -> Result<bool, &'static str> {
    let mut child = Command::new(SYSTEMCTL)
        .env_clear()
        .args([action, "kfaceauth.service"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| "service-failed")?;
    let started = Instant::now();
    loop {
        if let Some(status) = child.try_wait().map_err(|_| "service-failed")? {
            return Ok(status.success());
        }
        if started.elapsed() >= Duration::from_secs(15) {
            let _ = child.kill();
            let _ = child.wait();
            return Err("service-timeout");
        }
        std::thread::sleep(Duration::from_millis(25));
    }
}

fn transaction<S, V>(
    directory: &File,
    owner: u32,
    desired: Option<&str>,
    mut service: S,
    validate: V,
) -> Result<(), &'static str>
where
    S: FnMut(&str) -> Result<bool, &'static str>,
    V: Fn() -> bool,
{
    let previous = read_config(directory, owner)?;
    let next = replacement(previous.as_deref().unwrap_or_default(), desired)?;
    let was_active = service("is-active")?;
    let applied = atomic_write(directory, Some(&next))
        .and_then(|()| service("restart"))
        .and_then(|restarted| {
            if !restarted || !service("is-active")? {
                return Err("service-failed");
            }
            if !validate() {
                return Err("unavailable-selection");
            }
            if read_config(directory, owner)?.as_deref() != Some(next.as_slice()) {
                return Err("readback-failed");
            }
            Ok(())
        });
    if let Err(error) = applied {
        let restored = atomic_write(directory, previous.as_deref()).is_ok()
            && service(if was_active { "restart" } else { "stop" }).unwrap_or(false)
            && service("is-active").is_ok_and(|active| active == was_active)
            && read_config(directory, owner).is_ok_and(|bytes| bytes == previous);
        return Err(if restored { error } else { "rollback-failed" });
    }
    Ok(())
}

fn authorized(uid: Option<&str>, euid: u32) -> bool {
    euid == 0
        && uid.is_some_and(|uid| {
            uid.parse::<u32>()
                .is_ok_and(|parsed| parsed > 0 && parsed.to_string() == uid)
        })
}

fn run() -> Result<(), &'static str> {
    let args: Vec<_> = env::args().skip(1).collect();
    if args.len() != 1 {
        return Err("invalid-operation");
    }
    let operation = args[0].as_str();
    if !authorized(env::var("PKEXEC_UID").ok().as_deref(), effective_uid()) {
        return Err("unauthorized");
    }
    if operation == "--list" {
        let devices = cameras();
        let selected = safe_directory(Path::new("/etc/kfaceauth"), 0)
            .and_then(|directory| read_config(&directory, 0))
            .and_then(|bytes| setting(bytes.as_deref().unwrap_or_default()));
        println!("KFCAMERA1");
        match selected {
            Ok(Some(path)) => {
                // Existing by-id aliases remain valid; map them to the enumerated label.
                let canonical = fs::canonicalize(&path).ok();
                let listed = devices.iter().find(|(candidate, _)| {
                    canonical.is_some() && fs::canonicalize(candidate).ok() == canonical
                });
                let display = listed
                    .and_then(|(candidate, _)| candidate.to_str())
                    .unwrap_or(&path);
                println!("selected {}", hex(display.as_bytes()));
            }
            Ok(None) => println!("automatic"),
            Err(_) => println!("unknown"),
        }
        for (path, label) in devices {
            if let Some(path) = path.to_str() {
                println!("device {} {}", hex(path.as_bytes()), hex(label.as_bytes()));
            }
        }
        return Ok(());
    }
    let desired = match operation {
        "--reset" => None,
        "--set" => {
            let mut input = String::new();
            std::io::stdin()
                .take(4097)
                .read_to_string(&mut input)
                .map_err(|_| "invalid-selection")?;
            let path = input.strip_suffix('\n').unwrap_or(&input);
            if path.len() > 4096 || path.contains(['\n', '\r', '\0', '=', '"', '\'', ' ', '\t']) {
                return Err("invalid-selection");
            }
            let devices = cameras();
            if !devices
                .iter()
                .any(|(candidate, _)| candidate.to_str() == Some(path))
            {
                return Err("unavailable-selection");
            }
            Some(path.to_owned())
        }
        _ => return Err("invalid-operation"),
    };
    let parent = safe_directory(Path::new("/etc"), 0)?;
    let directory = provision_config_directory(&parent, 0)?;
    let _lock =
        lock_child_file_nonblocking(&directory, ".camera-config.lock").map_err(|_| "busy")?;
    // Enumerate again after taking the lock to reject unplugged or changed selections.
    if desired.as_ref().is_some_and(|path| {
        !cameras()
            .iter()
            .any(|(node, _)| node.to_str() == Some(path.as_str()))
    }) {
        return Err("unavailable-selection");
    }
    transaction(&directory, 0, desired.as_deref(), service, || {
        desired.as_ref().is_none_or(|path| {
            cameras()
                .iter()
                .any(|(node, _)| node.to_str() == Some(path.as_str()))
        })
    })?;
    // Return the privileged readback directly, including for an existing 0600 config.
    println!("KFCAMERA1");
    if let Some(path) = desired {
        println!("selected {}", hex(path.as_bytes()));
    } else {
        println!("automatic");
    }
    for (path, label) in cameras() {
        if let Some(path) = path.to_str() {
            println!("device {} {}", hex(path.as_bytes()), hex(label.as_bytes()));
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("KFCAMERA1 error {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kfaceauth_crypto_openssl_sys::current_uid;
    use std::sync::atomic::{AtomicUsize, Ordering};
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Fixture(PathBuf, File);
    impl Fixture {
        fn new() -> Self {
            let path = env::temp_dir().join(format!(
                "kfaceauth-camera-test-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
            let directory = safe_directory(&path, current_uid()).unwrap();
            Self(path, directory)
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.0).unwrap();
        }
    }
    #[test]
    fn initial_and_existing_config_directory_use_anchored_child() {
        let f = Fixture::new();
        for _ in 0..2 {
            let directory = provision_config_directory(&f.1, current_uid()).unwrap();
            assert_eq!(
                directory.metadata().unwrap().ino(),
                fs::metadata(f.0.join("kfaceauth")).unwrap().ino()
            );
        }
    }
    #[test]
    fn config_directory_symlink_is_rejected_without_following() {
        let f = Fixture::new();
        std::os::unix::fs::symlink("/tmp", f.0.join("kfaceauth")).unwrap();
        assert!(provision_config_directory(&f.1, current_uid()).is_err());
    }
    #[test]
    fn authorization_is_bound_to_non_root_canonical_uid() {
        assert!(authorized(Some("1000"), 0));
        for uid in [None, Some("0"), Some("01000"), Some("-1"), Some("x")] {
            assert!(!authorized(uid, 0));
        }
        assert!(!authorized(Some("1000"), 1000));
    }
    #[test]
    fn preserves_every_other_config_line() {
        let original =
            b"# comment\nOTHER=\"secret\"\nKFACEAUTH_CAMERA_DEVICE=/dev/video0\nLAST=yes";
        assert_eq!(
            replacement(original, Some("/dev/video1")).unwrap(),
            b"# comment\nOTHER=\"secret\"\nLAST=yes\nKFACEAUTH_CAMERA_DEVICE=/dev/video1\n"
        );
        assert_eq!(
            replacement(original, None).unwrap(),
            b"# comment\nOTHER=\"secret\"\nLAST=yes"
        );
        assert!(
            replacement(
                b"KFACEAUTH_CAMERA_DEVICE=a\nKFACEAUTH_CAMERA_DEVICE=b",
                None
            )
            .is_err()
        );
    }
    #[test]
    fn multiline_values_are_never_edited_as_camera_assignments() {
        for bytes in [
            b"OTHER='first\nKFACEAUTH_CAMERA_DEVICE=/dev/video0\nlast'\n".as_slice(),
            b"OTHER=first\\\nKFACEAUTH_CAMERA_DEVICE=/dev/video0\n".as_slice(),
        ] {
            assert_eq!(replacement(bytes, None), Err("unsupported-config"));
        }
    }
    #[test]
    fn unsafe_files_are_rejected() {
        let f = Fixture::new();
        std::os::unix::fs::symlink("/etc/passwd", f.0.join(CONFIG)).unwrap();
        assert_eq!(
            read_config(&f.1, current_uid()).unwrap_err(),
            "unsafe-config"
        );
    }
    #[test]
    fn writable_and_hardlinked_files_are_rejected() {
        let f = Fixture::new();
        fs::write(f.0.join(CONFIG), b"OTHER=yes\n").unwrap();
        fs::set_permissions(f.0.join(CONFIG), fs::Permissions::from_mode(0o666)).unwrap();
        assert_eq!(
            read_config(&f.1, current_uid()).unwrap_err(),
            "unsafe-config"
        );
        fs::set_permissions(f.0.join(CONFIG), fs::Permissions::from_mode(0o600)).unwrap();
        fs::hard_link(f.0.join(CONFIG), f.0.join("alias")).unwrap();
        assert_eq!(
            read_config(&f.1, current_uid()).unwrap_err(),
            "unsafe-config"
        );
    }
    #[test]
    fn disappeared_selection_rolls_back_after_restart() {
        let f = Fixture::new();
        let original = b"OTHER=preserved\n";
        atomic_write(&f.1, Some(original)).unwrap();
        let result = transaction(
            &f.1,
            current_uid(),
            Some("/dev/video1"),
            |_| Ok(true),
            || false,
        );
        assert_eq!(result, Err("unavailable-selection"));
        assert_eq!(read_config(&f.1, current_uid()).unwrap().unwrap(), original);
    }
    #[test]
    fn successful_transaction_reads_back_and_restarts_only_service() {
        let f = Fixture::new();
        let mut actions = Vec::new();
        transaction(
            &f.1,
            current_uid(),
            Some("/dev/video1"),
            |action| {
                actions.push(action.to_owned());
                Ok(true)
            },
            || true,
        )
        .unwrap();
        assert_eq!(actions, ["is-active", "restart", "is-active"]);
        assert_eq!(
            setting(&read_config(&f.1, current_uid()).unwrap().unwrap())
                .unwrap()
                .as_deref(),
            Some("/dev/video1")
        );
    }
    #[test]
    fn restart_failure_restores_config_and_inactive_service() {
        let f = Fixture::new();
        let original = b"OTHER=value\n";
        atomic_write(&f.1, Some(original)).unwrap();
        let mut actions = Vec::new();
        let result = transaction(
            &f.1,
            current_uid(),
            Some("/dev/video1"),
            |action| {
                actions.push(action.to_owned());
                Ok(action == "stop")
            },
            || true,
        );
        assert_eq!(result, Err("service-failed"));
        assert_eq!(read_config(&f.1, current_uid()).unwrap().unwrap(), original);
        assert_eq!(actions, ["is-active", "restart", "stop", "is-active"]);
    }
    #[test]
    fn rollback_failure_is_explicit() {
        let f = Fixture::new();
        assert_eq!(
            transaction(&f.1, current_uid(), None, |_| Ok(false), || true),
            Err("rollback-failed")
        );
    }
    #[test]
    fn failed_readback_restores_snapshot() {
        let f = Fixture::new();
        let mut calls = 0;
        let result = transaction(
            &f.1,
            current_uid(),
            Some("/dev/video1"),
            |action| {
                calls += 1;
                if calls == 3 {
                    fs::write(f.0.join(CONFIG), b"OTHER=changed\n").unwrap();
                }
                Ok(action != "is-active" || calls != 1 && calls != 5)
            },
            || true,
        );
        assert_eq!(result, Err("readback-failed"));
        assert!(read_config(&f.1, current_uid()).unwrap().is_none());
    }
}
