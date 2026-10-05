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
    IDENTITY_PROTOCOL_VERSION, MAX_IDENTITY_REQUEST_BYTES, MAX_IDENTITY_RESPONSE_BYTES,
    OP_EXTRACT_ENROLLMENT_SAMPLE,
};
use kfaceauth_protocol::write_frame;
use kfaceauth_templates::{Profile, VerificationResult};
use kfaceauth_vision::identity::{IdentityProvider, NormalizedEmbedding};
use kfaceauth_vision::{CancellationToken, ImageView, PixelFormat, ProcessingControl};
use zeroize::Zeroize;

const MAXIMUM_DATASET_SAMPLES: usize = 100_000;
const MAXIMUM_PPM_BYTES: usize = 1920 * 1080 * 3 + 256;
const EVALUATION_WORKER_MODE: &str = "--evaluation-worker-once";
const WORKER_RSS_PREFIX: &str = "kfaceauth-evaluation-worker-peak-rss-kib=";

struct Arguments {
    model_root: PathBuf,
    dataset_manifest: PathBuf,
    labelled_evaluation: bool,
}

struct SourceSample {
    width: u32,
    height: u32,
    bytes: Vec<u8>,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Role {
    Enrollment,
    Probe,
}

struct ManifestEntry {
    group: u32,
    role: Role,
    path: PathBuf,
}

#[derive(Default)]
struct Decisions {
    attempted: u64,
    match_count: u64,
    no_match: u64,
    ambiguous: u64,
    extraction_failures: u64,
}
impl Decisions {
    fn record(&mut self, result: VerificationResult) {
        self.attempted += 1;
        match result {
            VerificationResult::Match => self.match_count += 1,
            VerificationResult::NoMatch => self.no_match += 1,
            VerificationResult::Ambiguous => self.ambiguous += 1,
        }
    }
    fn json(&self) -> String {
        format!(
            r#"{{"attempted":{},"Match":{},"NoMatch":{},"Ambiguous":{},"extraction_failures":{}}}"#,
            self.attempted + self.extraction_failures,
            self.match_count,
            self.no_match,
            self.ambiguous,
            self.extraction_failures
        )
    }
}

impl Drop for SourceSample {
    fn drop(&mut self) {
        self.bytes.zeroize();
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
    let mut cold_latencies = Vec::with_capacity(samples.len());
    let mut worker_latencies = Vec::with_capacity(samples.len());
    let mut worker_peak_rss_kib: Option<u64> = None;
    let mut errors: BTreeMap<&'static str, u64> = BTreeMap::new();
    let mut accepted = 0_u64;
    for (index, entry) in samples.iter().enumerate() {
        let sample = match load_ppm(entry.group, &entry.path) {
            Ok(sample) => sample,
            Err(code) => {
                *errors.entry(code).or_default() += 1;
                continue;
            }
        };
        let started = Instant::now();
        match extract(&provider, &sample, &cancellation) {
            Ok(_) => accepted += 1,
            Err(code) => *errors.entry(code).or_default() += 1,
        }
        warm_latencies.push(started.elapsed());
        let started = Instant::now();
        let cold_provider = IdentityProvider::from_model_root(&arguments.model_root)
            .map_err(|_| "cold-model-initialization-failed")?;
        let _ = extract(&cold_provider, &sample, &cancellation);
        cold_latencies.push(started.elapsed());
        let generation = u64::try_from(index + 1).map_err(|_| "dataset-too-large")?;
        let (latency, peak_rss) =
            run_evaluation_worker_process(&arguments.model_root, &sample, generation)?;
        worker_latencies.push(latency);
        if let Some(value) = peak_rss {
            worker_peak_rss_kib = Some(worker_peak_rss_kib.unwrap_or(0).max(value));
        }
    }
    // Only one bounded profile and one probe embedding are resident at a time.
    let (profiles, enrollment_failures, genuine, impostor) = evaluate_profiles(&samples, |entry| {
        let sample = load_ppm(entry.group, &entry.path)?;
        extract(&provider, &sample, &cancellation)
    });
    let error_json = errors
        .iter()
        .map(|(code, count)| format!(r#""{}":{}"#, json_string(code), count))
        .collect::<Vec<_>>()
        .join(",");
    Ok(format!(
        concat!(
            r#"{{"schema":3,"status":"complete","environment":{{"os":"{}","architecture":"{}","opencv":"{}"}},"#,
            r#""samples":{{"supplied":{},"accepted":{},"errors":{{{}}}}},"#,
            r#""model_initialization_ms":{:.3},"cold_pipeline_ms":{},"warm_pipeline_ms":{},"#,
            r#""first_worker_process_ms":{},"warm_worker_process_ms":{},"#,
            r#""peak_parent_resident_memory_kib":{},"worker_peak_resident_memory_kib":{},"#,
            r#""profiles":{{"accepted":{},"enrollment_failures":{}}},"genuine":{},"impostor":{},"far_frr_qualification":"unqualified","decision_evaluation":"{}"}}"#
        ),
        json_string(env::consts::OS),
        json_string(env::consts::ARCH),
        json_string(&kfaceauth_vision::yunet::YuNetProvider::runtime_version()),
        samples.len(),
        accepted,
        error_json,
        milliseconds(initialization),
        latency_json(&cold_latencies),
        latency_json(&warm_latencies),
        latency_json(&worker_latencies[..worker_latencies.len().min(1)]),
        latency_json(worker_latencies.get(1..).unwrap_or_default()),
        peak_rss_kib().map_or_else(|| "null".to_owned(), |v| v.to_string()),
        worker_peak_rss_kib.map_or_else(|| "null".to_owned(), |v| v.to_string()),
        profiles,
        enrollment_failures,
        genuine.json(),
        impostor.json(),
        if arguments.labelled_evaluation
            && enrollment_failures == 0
            && genuine.extraction_failures == 0
            && impostor.extraction_failures == 0
            && profiles >= 2
            && genuine.attempted > 0
            && impostor.attempted > 0
        {
            "dataset-scoped"
        } else {
            "unqualified"
        }
    ))
}

fn evaluate_profiles<F>(
    entries: &[ManifestEntry],
    mut extract_sample: F,
) -> (u64, u64, Decisions, Decisions)
where
    F: FnMut(&ManifestEntry) -> Result<NormalizedEmbedding, &'static str>,
{
    let mut groups = BTreeMap::new();
    for entry in entries
        .iter()
        .filter(|entry| entry.role == Role::Enrollment)
    {
        *groups.entry(entry.group).or_insert(0_usize) += 1;
    }
    let mut profiles = 0;
    let mut failures = 0;
    let mut genuine = Decisions::default();
    let mut impostor = Decisions::default();
    for (group, count) in groups {
        if !(3..=8).contains(&count) {
            failures += 1;
            continue;
        }
        let embeddings: Result<Vec<_>, _> = entries
            .iter()
            .filter(|entry| entry.role == Role::Enrollment && entry.group == group)
            .map(&mut extract_sample)
            .collect();
        let Ok(profile) =
            embeddings.and_then(|values| Profile::new(values).map_err(|_| "profile-bounds"))
        else {
            failures += 1;
            continue;
        };
        profiles += 1;
        for probe in entries.iter().filter(|entry| entry.role == Role::Probe) {
            let aggregate = if probe.group == group {
                &mut genuine
            } else {
                &mut impostor
            };
            match extract_sample(probe) {
                Ok(candidate) => aggregate.record(profile.verify(&candidate)),
                Err(_) => aggregate.extraction_failures += 1,
            }
        }
    }
    (profiles, failures, genuine, impostor)
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
    request.zeroize();
    write_result?;
    let mut output = child
        .wait_with_output()
        .map_err(|_| "evaluation-worker-io-failed")?;
    let elapsed = started.elapsed();
    if !output.status.success()
        || output.stdout.len() < 16
        || output.stdout.len() > MAX_IDENTITY_RESPONSE_BYTES + 4
    {
        output.stdout.zeroize();
        output.stderr.zeroize();
        return Err("evaluation-worker-failed");
    }
    let peak_rss = parse_worker_peak_rss(&output.stderr);
    output.stdout.zeroize();
    output.stderr.zeroize();
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
    let result = write_frame(&mut framed, &payload, MAX_IDENTITY_REQUEST_BYTES);
    payload.zeroize();
    result.map_err(|_| "invalid-frame")?;
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

fn load_manifest(path: &Path) -> Result<Vec<ManifestEntry>, &'static str> {
    use std::io::{BufRead, Read};
    let file = fs::File::open(path).map_err(|_| "dataset-manifest-unavailable")?;
    // Bound metadata as well as frames. More than 64 samples is supported;
    // raw frames and extracted probe embeddings are never accumulated.
    let mut lines = io::BufReader::new(file.take(32 * 1024 * 1024 + 1)).lines();
    if lines
        .next()
        .transpose()
        .map_err(|_| "invalid-dataset-manifest")?
        .as_deref()
        != Some("kfaceauth-evaluation-v2")
    {
        return Err("invalid-manifest-schema");
    }
    let mut entries = Vec::new();
    let mut paths = std::collections::BTreeSet::new();
    let mut total_bytes = 0;
    for line in lines {
        let line = line.map_err(|_| "invalid-dataset-manifest")?;
        total_bytes += line.len() + 1;
        if total_bytes > 32 * 1024 * 1024 {
            return Err("dataset-too-large");
        }
        if line.is_empty() {
            continue;
        }
        let fields: Vec<_> = line.split('\t').collect();
        if fields.len() != 3 {
            return Err("invalid-dataset-manifest");
        }
        let group = fields[0].parse::<u32>().map_err(|_| "invalid-group-id")?;
        let role = match fields[1] {
            "enrollment" => Role::Enrollment,
            "probe" => Role::Probe,
            _ => return Err("invalid-sample-role"),
        };
        let image_path = Path::new(fields[2]);
        if group == 0 || !image_path.is_absolute() {
            return Err("invalid-dataset-manifest");
        }
        let canonical = fs::canonicalize(image_path).map_err(|_| "dataset-image-unavailable")?;
        if !paths.insert(canonical.clone()) {
            return Err("duplicate-dataset-image");
        }
        entries.push(ManifestEntry {
            group,
            role,
            path: canonical,
        });
        if entries.len() > MAXIMUM_DATASET_SAMPLES {
            return Err("dataset-too-large");
        }
    }
    if entries.is_empty() || !entries.iter().any(|entry| entry.role == Role::Probe) {
        return Err("dataset-has-no-probes");
    }
    Ok(entries)
}

fn load_ppm(_group: u32, path: &Path) -> Result<SourceSample, &'static str> {
    let mut bytes = zeroize::Zeroizing::new(Vec::new());
    std::io::Read::read_to_end(
        &mut std::io::Read::take(
            fs::File::open(path).map_err(|_| "dataset-image-unavailable")?,
            MAXIMUM_PPM_BYTES as u64 + 1,
        ),
        &mut bytes,
    )
    .map_err(|_| "dataset-image-unavailable")?;
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
        .extract(
            image,
            control,
            kfaceauth_vision::identity::ExtractionPurpose::LocalProfile,
        )
        .map_err(|error| match error {
            kfaceauth_vision::identity::IdentityError::Cancelled => "cancelled",
            kfaceauth_vision::identity::IdentityError::DeadlineExceeded => "deadline",
            kfaceauth_vision::identity::IdentityError::NoFace => "no-face",
            kfaceauth_vision::identity::IdentityError::MultipleFaces => "multiple-faces",
            kfaceauth_vision::identity::IdentityError::PoorQuality => "poor-quality",
            kfaceauth_vision::identity::IdentityError::FaceGeometry => "face-geometry",
            kfaceauth_vision::identity::IdentityError::LivenessUnavailable => {
                "liveness-unavailable"
            }
            kfaceauth_vision::identity::IdentityError::InvalidEmbedding => "invalid-embedding",
            kfaceauth_vision::identity::IdentityError::SpoofDetected(_) => "spoof-detected",
            kfaceauth_vision::identity::IdentityError::Runtime(_) => "runtime",
        })
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

#[cfg(test)]
mod tests {
    use super::*;
    fn embedding(score: f32) -> NormalizedEmbedding {
        let mut values = [0.0; 128];
        values[0] = score;
        values[1] = (1.0 - score * score).sqrt();
        NormalizedEmbedding::from_raw(values).unwrap()
    }
    fn entry(group: u32, role: Role, score: &str) -> ManifestEntry {
        ManifestEntry {
            group,
            role,
            path: PathBuf::from(score),
        }
    }
    #[test]
    fn uses_product_median_and_separate_probe_denominators() {
        let mut entries = Vec::new();
        for group in [1, 2] {
            for _ in 0..3 {
                entries.push(entry(group, Role::Enrollment, "1"));
            }
        }
        for score in ["1", "0.43", "0", "fail"] {
            entries.push(entry(1, Role::Probe, score));
        }
        let (profiles, failures, genuine, impostor) = evaluate_profiles(&entries, |e| {
            e.path
                .to_str()
                .unwrap()
                .parse::<f32>()
                .map(embedding)
                .map_err(|_| "extraction-failed")
        });
        assert_eq!((profiles, failures), (2, 0));
        for aggregate in [genuine, impostor] {
            assert_eq!(
                (
                    aggregate.match_count,
                    aggregate.ambiguous,
                    aggregate.no_match,
                    aggregate.extraction_failures
                ),
                (1, 1, 1, 1)
            );
            assert_eq!(aggregate.attempted, 3);
        }
    }
    #[test]
    fn median_rejects_a_single_matching_enrollment_outlier() {
        let entries = vec![
            entry(1, Role::Enrollment, "1"),
            entry(1, Role::Enrollment, "0"),
            entry(1, Role::Enrollment, "0"),
            entry(1, Role::Probe, "1"),
        ];
        let (_, _, genuine, _) = evaluate_profiles(&entries, |e| {
            Ok(embedding(e.path.to_str().unwrap().parse().unwrap()))
        });
        assert_eq!((genuine.match_count, genuine.no_match), (0, 1));
    }

    #[test]
    fn manifest_accepts_more_than_64_entries_and_rejects_role_reuse() {
        use std::fmt::Write as _;
        let root = env::temp_dir().join(format!("kfaceauth-evaluator-test-{}", process::id()));
        fs::create_dir_all(&root).unwrap();
        let manifest_path = root.join("manifest.tsv");
        let mut manifest = String::from("kfaceauth-evaluation-v2\n");
        for index in 0..65 {
            let image = root.join(format!("{index}.ppm"));
            fs::write(&image, b"P6\n1 1\n255\nabc").unwrap();
            writeln!(manifest, "1\tprobe\t{}", image.display()).unwrap();
        }
        fs::write(&manifest_path, &manifest).unwrap();
        assert_eq!(load_manifest(&manifest_path).unwrap().len(), 65);
        writeln!(manifest, "1\tenrollment\t{}", root.join("0.ppm").display()).unwrap();
        fs::write(&manifest_path, manifest).unwrap();
        assert!(matches!(
            load_manifest(&manifest_path),
            Err("duplicate-dataset-image")
        ));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn failed_or_oversized_enrollment_is_not_a_partial_profile() {
        let mut entries = Vec::new();
        for _ in 0..9 {
            entries.push(entry(1, Role::Enrollment, "1"));
        }
        for score in ["1", "1", "fail"] {
            entries.push(entry(2, Role::Enrollment, score));
        }
        entries.push(entry(1, Role::Probe, "1"));
        let (profiles, failures, genuine, impostor) = evaluate_profiles(&entries, |e| {
            e.path
                .to_str()
                .unwrap()
                .parse::<f32>()
                .map(embedding)
                .map_err(|_| "extraction-failed")
        });
        assert_eq!(
            (profiles, failures, genuine.attempted, impostor.attempted),
            (0, 2, 0, 0)
        );
    }
}
