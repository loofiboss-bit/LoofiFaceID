// SPDX-License-Identifier: GPL-3.0-or-later

use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

const RESPONSE_ANALYSIS: u8 = 0x81;
const RESPONSE_ERROR: u8 = 0xff;
const DEADLINE_EXCEEDED: u8 = 10;

fn model_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../models")
        .canonicalize()
        .expect("source model root exists")
}

fn request(width: u16, height: u16, timeout_ms: u32, generation: u64) -> Vec<u8> {
    let stride = u32::from(width) * 3;
    let frame_size = usize::try_from(stride * u32::from(height)).unwrap();
    let mut payload = Vec::with_capacity(24 + frame_size);
    payload.extend_from_slice(&2_u16.to_be_bytes());
    payload.push(1);
    payload.push(1);
    payload.extend_from_slice(&generation.to_be_bytes());
    payload.extend_from_slice(&timeout_ms.to_be_bytes());
    payload.extend_from_slice(&width.to_be_bytes());
    payload.extend_from_slice(&height.to_be_bytes());
    payload.extend_from_slice(&stride.to_be_bytes());
    payload.resize(24 + frame_size, 0);

    let mut framed = Vec::with_capacity(payload.len() + 4);
    framed.extend_from_slice(&u32::try_from(payload.len()).unwrap().to_be_bytes());
    framed.extend_from_slice(&payload);
    framed
}

fn run_worker(input: &[u8]) -> (Duration, Vec<u8>) {
    let started = Instant::now();
    let mut child = Command::new(env!("CARGO_BIN_EXE_kfaceauth-vision-worker"))
        .arg("--model-root")
        .arg(model_root())
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("production worker starts");
    child
        .stdin
        .take()
        .expect("worker stdin")
        .write_all(input)
        .expect("worker request");
    let output = child.wait_with_output().expect("production worker exits");
    assert!(
        output.status.success(),
        "worker failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    (started.elapsed(), output.stdout)
}

fn response_payload(framed: &[u8]) -> &[u8] {
    assert!(framed.len() >= 4);
    let length = usize::try_from(u32::from_be_bytes(framed[0..4].try_into().unwrap())).unwrap();
    assert_eq!(framed.len(), length + 4);
    &framed[4..]
}

#[test]
fn cold_production_worker_runs_real_zero_face_inference() {
    let generation = 41;
    let (elapsed, response) = run_worker(&request(32, 32, 5_000, generation));
    assert!(elapsed < Duration::from_secs(10));
    let payload = response_payload(&response);
    assert_eq!(payload[2], RESPONSE_ANALYSIS);
    assert_eq!(payload[3], 0);
    assert_eq!(
        u64::from_be_bytes(payload[4..12].try_into().unwrap()),
        generation
    );
}

#[test]
fn real_inference_obeys_the_request_deadline() {
    let generation = 42;
    let (_, response) = run_worker(&request(640, 480, 1, generation));
    let payload = response_payload(&response);
    assert_eq!(payload[2], RESPONSE_ERROR);
    assert_eq!(payload[3], DEADLINE_EXCEEDED);
    assert_eq!(
        u64::from_be_bytes(payload[4..12].try_into().unwrap()),
        generation
    );
}
