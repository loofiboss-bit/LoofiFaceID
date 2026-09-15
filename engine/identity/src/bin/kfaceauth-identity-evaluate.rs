// SPDX-License-Identifier: GPL-3.0-or-later

//! Non-installed aggregate evaluator for explicitly supplied, consented data.

#![forbid(unsafe_code)]

use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{self, Command, Stdio};
use std::time::{Duration, Instant};

use kfaceauth_identity::{
    IDENTITY_PROTOCOL_VERSION, MAX_IDENTITY_RESPONSE_BYTES, OP_EXTRACT_ENROLLMENT_SAMPLE,
};
use kfaceauth_vision::identity::{IdentityProvider, NormalizedEmbedding, cosine_similarity};
use kfaceauth_vision::{CancellationToken, ImageView, PixelFormat, ProcessingControl};

const MAXIMUM_DATASET_SAMPLES: usize = 64;
const MAXIMUM_PPM_BYTES: usize = 1920 * 1080 * 3 + 256;
const EVALUATION_THRESHOLDS: [f64; 9] = [0.30, 0.35, 0.40, 0.41, 0.45, 0.49, 0.50, 0.60, 0.70];
const EVALUATION_WORKER_MODE: &str = "--evaluation-worker-once";
const WORKER_RSS_PREFIX: &str = "kfaceauth-evaluation-worker-peak-rss-kib=";

struct Arguments {
    model_root: PathBuf,
    dataset_manifest: PathBuf,
    labelled_evaluation: bool,
}

struct SourceSample {
    group: u32,
    width: u32,
    height: u32,
    bytes: Vec<u8>,
}

struct AcceptedSample {
    group: u32,
    embedding: NormalizedEmbedding,
}

#[derive(Default)]
struct PairAggregate {
    count: u64,
    total: f64,
    minimum: f64,
    maximum: f64,
}

impl PairAggregate {
    fn add(&mut self, value: f64) {
        if self.count == 0 {
            self.minimum = value;
            self.maximum = value;
        } else {
            self.minimum = self.minimum.min(value);
            self.maximum = self.maximum.max(value);
        }
        self.total += value;
        self.count += 1;
    }

    fn json(&self) -> String {
        if self.count == 0 {
            return r#"{"count":0,"mean":null,"minimum":null,"maximum":null}"#.to_owned();
        }
        format!(
            r#"{{"count":{},"mean":{:.8},"minimum":{:.8},"maximum":{:.8}}}"#,
            self.count,
            self.total / f64::from(u32::try_from(self.count).unwrap_or(u32::MAX)),
            self.minimum,
            self.maximum
        )
    }
}

fn main() {
    if env::args().nth(1).as_deref() == Some(EVALUATION_WORKER_MODE) {
        process::exit(run_evaluation_worker_once());
    }
    match run() {
        Ok(output) => println!("{output}"),
        Err(code) => {
            println!(
                r#"{{"schema":1,"status":"unavailable","error":"{}"}}"#,
                json_string(code)
            );
            process::exit(1);
        }
    }
}

fn run() -> Result<String, &'static str> {
    let arguments = parse_arguments()?;
    let samples = load_manifest(&arguments.dataset_manifest)?;

    let initialization_started = Instant::now();
    let provider = IdentityProvider::from_model_root(&arguments.model_root)
        .map_err(|_| "model-initialization-failed")?;
    let initialization = initialization_started.elapsed();

    let cancellation = CancellationToken::default();
    let mut warm_latencies = Vec::with_capacity(samples.len());
    let mut accepted = Vec::with_capacity(samples.len());
    let mut errors: BTreeMap<&'static str, u64> = BTreeMap::new();
    for sample in &samples {
        let started = Instant::now();
        let result = extract(&provider, sample, &cancellation);
        warm_latencies.push(started.elapsed());
        match result {
            Ok(embedding) => accepted.push(AcceptedSample {
                group: sample.group,
                embedding,
            }),
            Err(code) => *errors.entry(code).or_default() += 1,
        }
    }

    let mut cold_latencies = Vec::with_capacity(samples.len());
    for sample in &samples {
        let started = Instant::now();
        let cold_provider = IdentityProvider::from_model_root(&arguments.model_root)
            .map_err(|_| "cold-model-initialization-failed")?;
        let _ = extract(&cold_provider, sample, &cancellation);
        cold_latencies.push(started.elapsed());
    }

    let mut worker_latencies = Vec::with_capacity(samples.len());
    let mut worker_peak_rss_kib: Option<u64> = None;
    for (index, sample) in samples.iter().enumerate() {
        let generation = u64::try_from(index)
            .ok()
            .and_then(|value| value.checked_add(1))
            .ok_or("dataset-too-large")?;
        let (latency, peak_rss) =
            run_evaluation_worker_process(&arguments.model_root, sample, generation)?;
        worker_latencies.push(latency);
        worker_peak_rss_kib = match (worker_peak_rss_kib, peak_rss) {
            (Some(current), Some(value)) => Some(current.max(value)),
            (None, value) => value,
            (current, None) => current,
        };
    }

    let (same, different) = pair_aggregates(&accepted);
    let labelled_qualified =
        arguments.labelled_evaluation && labelled_set_is_minimally_usable(&accepted);
    let sweep = threshold_sweep(&accepted, labelled_qualified);
    let peak_rss_kib = peak_rss_kib();
    let error_json = errors
        .iter()
        .map(|(code, count)| format!(r#""{}":{}"#, json_string(code), count))
        .collect::<Vec<_>>()
        .join(",");

    Ok(format!(
        concat!(
            r#"{{"schema":2,"status":"complete","#,
            r#""environment":{{"os":"{}","architecture":"{}","opencv":"{}"}},"#,
            r#""samples":{{"supplied":{},"accepted":{},"errors":{{{}}}}},"#,
            r#""model_initialization_ms":{:.3},"#,
            r#""cold_pipeline_ms":{},"warm_pipeline_ms":{},"#,
            r#""first_worker_process_ms":{},"warm_worker_process_ms":{},"#,
            r#""peak_parent_resident_memory_kib":{},"worker_peak_resident_memory_kib":{},"#,
            r#""repeated_same_identity":{},"different_identity":{},"#,
            r#""far_frr_qualification":"{}","threshold_sweep":{}}}"#
        ),
        json_string(env::consts::OS),
        json_string(env::consts::ARCH),
        json_string(&kfaceauth_vision::yunet::YuNetProvider::runtime_version()),
        samples.len(),
        accepted.len(),
        error_json,
        milliseconds(initialization),
        latency_json(&cold_latencies),
        latency_json(&warm_latencies),
        latency_json(&worker_latencies[..worker_latencies.len().min(1)]),
        latency_json(worker_latencies.get(1..).unwrap_or_default()),
        peak_rss_kib.map_or_else(|| "null".to_owned(), |value| value.to_string()),
        worker_peak_rss_kib.map_or_else(|| "null".to_owned(), |value| value.to_string()),
        same.json(),
        different.json(),
        if labelled_qualified {
            "dataset-scoped"
        } else {
            "unqualified"
        },
        sweep
    ))
}

fn run_evaluation_worker_once() -> i32 {
    let mut arguments = env::args_os().skip(2);
    let Some(model_root) = arguments.next().map(PathBuf::from) else {
        return 2;
    };
    if arguments.next().is_some() || !model_root.is_absolute() {
        return 2;
    }
    if kfaceauth_vision_opencv_sys::disable_core_dumps().is_err() {
        return 3;
    }
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let result = kfaceauth_identity::serve_once(&mut input, &mut output, &model_root);
    if let Some(value) = peak_rss_kib() {
        eprintln!("{WORKER_RSS_PREFIX}{value}");
    }
    if result.is_ok() { 0 } else { 4 }
}

fn run_evaluation_worker_process(
    model_root: &Path,
    sample: &SourceSample,
    generation: u64,
) -> Result<(Duration, Option<u64>), &'static str> {
    let executable = env::current_exe().map_err(|_| "evaluation-worker-unavailable")?;
    let mut request = evaluation_request(sample, generation)?;
    let started = Instant::now();
    let mut child = Command::new(executable)
        .arg(EVALUATION_WORKER_MODE)
        .arg(model_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|_| "evaluation-worker-unavailable")?;
    let write_result = child
        .stdin
        .take()
        .ok_or("evaluation-worker-unavailable")
        .and_then(|mut input| {
            input
                .write_all(&request)
                .map_err(|_| "evaluation-worker-io-failed")
        });
    request.fill(0);
    write_result?;
    let mut output = child
        .wait_with_output()
        .map_err(|_| "evaluation-worker-io-failed")?;
    let elapsed = started.elapsed();
    if !output.status.success()
        || output.stdout.len() < 16
        || output.stdout.len() > MAX_IDENTITY_RESPONSE_BYTES + 4
    {
        output.stdout.fill(0);
        output.stderr.fill(0);
        return Err("evaluation-worker-failed");
    }
    let peak_rss = parse_worker_peak_rss(&output.stderr);
    output.stdout.fill(0);
    output.stderr.fill(0);
    Ok((elapsed, peak_rss))
}

fn evaluation_request(sample: &SourceSample, generation: u64) -> Result<Vec<u8>, &'static str> {
    let width = u16::try_from(sample.width).map_err(|_| "invalid-frame")?;
    let height = u16::try_from(sample.height).map_err(|_| "invalid-frame")?;
    let stride = u16::try_from(sample.width.checked_mul(3).ok_or("invalid-frame")?)
        .map_err(|_| "invalid-frame")?;
    let mut payload = Vec::with_capacity(25 + sample.bytes.len());
    payload.extend_from_slice(&IDENTITY_PROTOCOL_VERSION.to_be_bytes());
    payload.push(OP_EXTRACT_ENROLLMENT_SAMPLE);
    payload.push(0);
    payload.extend_from_slice(&generation.to_be_bytes());
    payload.extend_from_slice(&10_000_u32.to_be_bytes());
    payload.push(0);
    payload.push(1);
    payload.push(0);
    payload.extend_from_slice(&width.to_be_bytes());
    payload.extend_from_slice(&height.to_be_bytes());
    payload.extend_from_slice(&stride.to_be_bytes());
    payload.extend_from_slice(&sample.bytes);

    let mut framed = Vec::with_capacity(payload.len() + 4);
    framed.extend_from_slice(
        &u32::try_from(payload.len())
            .map_err(|_| "invalid-frame")?
            .to_be_bytes(),
    );
    framed.extend_from_slice(&payload);
    payload.fill(0);
    Ok(framed)
}

fn parse_worker_peak_rss(stderr: &[u8]) -> Option<u64> {
    std::str::from_utf8(stderr)
        .ok()?
        .lines()
        .find_map(|line| line.strip_prefix(WORKER_RSS_PREFIX)?.parse::<u64>().ok())
}

fn parse_arguments() -> Result<Arguments, &'static str> {
    let mut values = env::args_os().skip(1);
    let mut model_root = None;
    let mut dataset_manifest = None;
    let mut labelled_evaluation = false;
    while let Some(argument) = values.next() {
        if argument == "--model-root" {
            model_root = values.next().map(PathBuf::from);
        } else if argument == "--dataset-manifest" {
            dataset_manifest = values.next().map(PathBuf::from);
        } else if argument == "--labelled-evaluation" {
            labelled_evaluation = true;
        } else {
            return Err("invalid-arguments");
        }
    }
    let model_root = model_root.ok_or("missing-model-root")?;
    let dataset_manifest = dataset_manifest.ok_or("missing-dataset-manifest")?;
    if !model_root.is_absolute() || !dataset_manifest.is_absolute() {
        return Err("paths-must-be-absolute");
    }
    Ok(Arguments {
        model_root,
        dataset_manifest,
        labelled_evaluation,
    })
}

fn load_manifest(path: &Path) -> Result<Vec<SourceSample>, &'static str> {
    let manifest = fs::read_to_string(path).map_err(|_| "dataset-manifest-unavailable")?;
    let mut samples = Vec::new();
    for line in manifest.lines().filter(|line| !line.trim().is_empty()) {
        let (group, image_path) = line.split_once('\t').ok_or("invalid-dataset-manifest")?;
        let group = group.parse::<u32>().map_err(|_| "invalid-group-id")?;
        let image_path = Path::new(image_path);
        if group == 0 || !image_path.is_absolute() {
            return Err("invalid-dataset-manifest");
        }
        samples.push(load_ppm(group, image_path)?);
        if samples.len() > MAXIMUM_DATASET_SAMPLES {
            return Err("dataset-too-large");
        }
    }
    if samples.is_empty() {
        return Err("dataset-is-empty");
    }
    Ok(samples)
}

fn load_ppm(group: u32, path: &Path) -> Result<SourceSample, &'static str> {
    let bytes = fs::read(path).map_err(|_| "dataset-image-unavailable")?;
    if bytes.len() > MAXIMUM_PPM_BYTES {
        return Err("dataset-image-too-large");
    }
    let mut offset = 0;
    let magic = ppm_token(&bytes, &mut offset)?;
    let width = ppm_token(&bytes, &mut offset)?
        .parse::<u32>()
        .map_err(|_| "invalid-ppm")?;
    let height = ppm_token(&bytes, &mut offset)?
        .parse::<u32>()
        .map_err(|_| "invalid-ppm")?;
    let maximum = ppm_token(&bytes, &mut offset)?;
    if magic != "P6"
        || maximum != "255"
        || width == 0
        || width > 1920
        || height == 0
        || height > 1080
    {
        return Err("invalid-ppm");
    }
    if offset >= bytes.len() || !bytes[offset].is_ascii_whitespace() {
        return Err("invalid-ppm");
    }
    offset += 1;
    let expected = usize::try_from(width)
        .ok()
        .and_then(|width| width.checked_mul(usize::try_from(height).ok()?))
        .and_then(|pixels| pixels.checked_mul(3))
        .ok_or("invalid-ppm")?;
    if bytes.len().checked_sub(offset) != Some(expected) {
        return Err("invalid-ppm");
    }
    Ok(SourceSample {
        group,
        width,
        height,
        bytes: bytes[offset..].to_vec(),
    })
}

fn ppm_token<'a>(bytes: &'a [u8], offset: &mut usize) -> Result<&'a str, &'static str> {
    while *offset < bytes.len() {
        if bytes[*offset] == b'#' {
            while *offset < bytes.len() && bytes[*offset] != b'\n' {
                *offset += 1;
            }
        } else if bytes[*offset].is_ascii_whitespace() {
            *offset += 1;
        } else {
            break;
        }
    }
    let start = *offset;
    while *offset < bytes.len() && !bytes[*offset].is_ascii_whitespace() {
        *offset += 1;
    }
    if start == *offset {
        return Err("invalid-ppm");
    }
    std::str::from_utf8(&bytes[start..*offset]).map_err(|_| "invalid-ppm")
}

fn extract(
    provider: &IdentityProvider,
    sample: &SourceSample,
    cancellation: &CancellationToken,
) -> Result<NormalizedEmbedding, &'static str> {
    let stride = sample.width.checked_mul(3).ok_or("invalid-frame")?;
    let image = ImageView::new(
        PixelFormat::Rgb8,
        sample.width,
        sample.height,
        stride,
        &sample.bytes,
    )
    .map_err(|_| "invalid-frame")?;
    let control = ProcessingControl::with_timeout(cancellation, Duration::from_secs(10))
        .map_err(|_| "deadline")?;
    provider
        .extract(image, control)
        .map_err(|error| match error {
            kfaceauth_vision::identity::IdentityError::Cancelled => "cancelled",
            kfaceauth_vision::identity::IdentityError::DeadlineExceeded => "deadline",
            kfaceauth_vision::identity::IdentityError::NoFace => "no-face",
            kfaceauth_vision::identity::IdentityError::MultipleFaces => "multiple-faces",
            kfaceauth_vision::identity::IdentityError::PoorQuality => "poor-quality",
            kfaceauth_vision::identity::IdentityError::FaceGeometry => "face-geometry",
            kfaceauth_vision::identity::IdentityError::InvalidEmbedding => "invalid-embedding",
            kfaceauth_vision::identity::IdentityError::Runtime(_) => "runtime",
        })
}

fn pair_aggregates(samples: &[AcceptedSample]) -> (PairAggregate, PairAggregate) {
    let mut same = PairAggregate::default();
    let mut different = PairAggregate::default();
    for (left_index, left) in samples.iter().enumerate() {
        for right in &samples[left_index + 1..] {
            let similarity = cosine_similarity(&left.embedding, &right.embedding);
            if left.group == right.group {
                same.add(similarity);
            } else {
                different.add(similarity);
            }
        }
    }
    (same, different)
}

fn labelled_set_is_minimally_usable(samples: &[AcceptedSample]) -> bool {
    let mut counts = BTreeMap::new();
    for sample in samples {
        *counts.entry(sample.group).or_insert(0_u32) += 1;
    }
    counts.values().filter(|count| **count >= 2).count() >= 2
}

fn threshold_sweep(samples: &[AcceptedSample], qualified: bool) -> String {
    if !qualified {
        return "null".to_owned();
    }
    let mut rows = Vec::new();
    for threshold in EVALUATION_THRESHOLDS {
        let mut true_accept = 0_u64;
        let mut false_reject = 0_u64;
        let mut false_accept = 0_u64;
        let mut true_reject = 0_u64;
        for (left_index, left) in samples.iter().enumerate() {
            for right in &samples[left_index + 1..] {
                let accepted = cosine_similarity(&left.embedding, &right.embedding) >= threshold;
                match (left.group == right.group, accepted) {
                    (true, true) => true_accept += 1,
                    (true, false) => false_reject += 1,
                    (false, true) => false_accept += 1,
                    (false, false) => true_reject += 1,
                }
            }
        }
        rows.push(format!(
            r#"{{"threshold":{threshold:.2},"true_accept":{true_accept},"false_reject":{false_reject},"false_accept":{false_accept},"true_reject":{true_reject}}}"#
        ));
    }
    format!("[{}]", rows.join(","))
}

fn latency_json(values: &[Duration]) -> String {
    if values.is_empty() {
        return r#"{"count":0,"median":null,"p95":null,"worst":null}"#.to_owned();
    }
    let mut sorted = values.to_vec();
    sorted.sort_unstable();
    let median = sorted[sorted.len() / 2];
    let p95_index = sorted
        .len()
        .saturating_mul(95)
        .div_ceil(100)
        .saturating_sub(1);
    format!(
        r#"{{"count":{},"median":{:.3},"p95":{:.3},"worst":{:.3}}}"#,
        sorted.len(),
        milliseconds(median),
        milliseconds(sorted[p95_index]),
        milliseconds(*sorted.last().expect("non-empty latency list"))
    )
}

fn milliseconds(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1_000.0
}

fn peak_rss_kib() -> Option<u64> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    status.lines().find_map(|line| {
        let value = line.strip_prefix("VmHWM:")?.trim();
        value.strip_suffix("kB")?.trim().parse().ok()
    })
}

fn json_string(value: &str) -> String {
    value
        .chars()
        .flat_map(|character| match character {
            '"' => "\\\"".chars().collect::<Vec<_>>(),
            '\\' => "\\\\".chars().collect(),
            '\n' => "\\n".chars().collect(),
            '\r' => "\\r".chars().collect(),
            '\t' => "\\t".chars().collect(),
            value if value.is_control() => "?".chars().collect(),
            value => vec![value],
        })
        .collect()
}
