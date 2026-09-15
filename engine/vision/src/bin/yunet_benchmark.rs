// SPDX-License-Identifier: GPL-3.0-or-later

use std::env;
use std::fmt::Write as _;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use kfaceauth_vision::yunet::{YUNET_MODEL_SHA256, YuNetProvider};
use kfaceauth_vision::{
    CancellationToken, ImageView, PixelFormat, ProcessingControl, VisionProvider,
};
use kfaceauth_vision_opencv_sys::{set_thread_count, thread_count};

const MAX_WARM_UP: usize = 1_000;
const MAX_ITERATIONS: usize = 10_000;
const BENCHMARK_TIMEOUT: Duration = Duration::from_secs(30);

struct Arguments {
    model_root: PathBuf,
    width: u32,
    height: u32,
    format: PixelFormat,
    format_name: &'static str,
    warm_up: usize,
    iterations: usize,
    threads: Option<u32>,
    thread_sweep: bool,
}

fn main() {
    let arguments = parse_arguments().unwrap_or_else(|message| {
        eprintln!("{message}");
        std::process::exit(2);
    });
    let bytes_per_pixel = arguments.format.bytes_per_pixel();
    let stride = arguments
        .width
        .checked_mul(bytes_per_pixel)
        .expect("validated benchmark geometry");
    let frame_size = stride
        .checked_mul(arguments.height)
        .and_then(|size| usize::try_from(size).ok())
        .expect("validated benchmark frame size");
    let frame = synthetic_frame(frame_size, arguments.format);
    let image = ImageView::new(
        arguments.format,
        arguments.width,
        arguments.height,
        stride,
        &frame,
    )
    .expect("validated benchmark image");

    if let Some(threads) = arguments.threads {
        set_thread_count(threads).unwrap_or_else(|_| {
            eprintln!("benchmark failed: requested OpenCV thread count is unavailable");
            std::process::exit(2);
        });
    }

    let initialization_started = Instant::now();
    let provider = YuNetProvider::from_model_root(&arguments.model_root).unwrap_or_else(|_| {
        eprintln!("benchmark failed: verified YuNet initialization unavailable");
        std::process::exit(1);
    });
    let initialization = initialization_started.elapsed();
    let first = measure(&provider, image).unwrap_or_else(|()| {
        eprintln!("benchmark failed: cold YuNet inference unavailable");
        std::process::exit(1);
    });
    let cold_start = initialization.saturating_add(first);

    for _ in 0..arguments.warm_up {
        if measure(&provider, image).is_err() {
            eprintln!("benchmark failed: warm-up YuNet inference unavailable");
            std::process::exit(1);
        }
    }

    let mut measured = Vec::with_capacity(arguments.iterations);
    for _ in 0..arguments.iterations {
        measured.push(measure(&provider, image).unwrap_or_else(|()| {
            eprintln!("benchmark failed: measured YuNet inference unavailable");
            std::process::exit(1);
        }));
    }
    measured.sort_unstable();
    let median = percentile(&measured, 1, 2);
    let p95 = percentile(&measured, 95, 100);
    let worst = *measured.last().expect("iterations are nonzero");
    let peak_memory_kib = peak_memory_kib();
    let thread_scaling = if arguments.thread_sweep {
        measure_thread_sweep(&provider, image)
    } else {
        "null".to_owned()
    };

    println!(
        "{{\"schema\":\"kfaceauth-yunet-benchmark-v2\",\
         \"environment\":\"build-environment\",\
         \"architecture\":\"{}\",\
         \"opencv_version\":\"{}\",\
         \"backend\":\"{}\",\"opencv_threads\":{},\
         \"model_sha256\":\"{}\",\
         \"width\":{},\"height\":{},\"pixel_format\":\"{}\",\
         \"warm_up_iterations\":{},\"measured_iterations\":{},\
         \"provider_initialization_ms\":{:.3},\"first_inference_ms\":{:.3},\
         \"cold_start_ms\":{:.3},\"median_inference_ms\":{:.3},\
         \"p95_inference_ms\":{:.3},\"worst_inference_ms\":{:.3},\
         \"peak_memory_kib\":{},\"thread_scaling\":{}}}",
        escape_json(env::consts::ARCH),
        escape_json(&YuNetProvider::runtime_version()),
        provider.backend().name(),
        thread_count(),
        YUNET_MODEL_SHA256,
        arguments.width,
        arguments.height,
        arguments.format_name,
        arguments.warm_up,
        arguments.iterations,
        milliseconds(initialization),
        milliseconds(first),
        milliseconds(cold_start),
        milliseconds(median),
        milliseconds(p95),
        milliseconds(worst),
        peak_memory_kib.map_or_else(|| "null".to_owned(), |value| value.to_string()),
        thread_scaling,
    );
}

fn parse_arguments() -> Result<Arguments, String> {
    let mut result = Arguments {
        model_root: PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../models"),
        width: 320,
        height: 320,
        format: PixelFormat::Rgb8,
        format_name: "RGB8",
        warm_up: 3,
        iterations: 20,
        threads: None,
        thread_sweep: false,
    };
    let mut arguments = env::args_os();
    let _program = arguments.next();
    while let Some(flag) = arguments.next() {
        match flag.to_str() {
            Some("--thread-sweep") => result.thread_sweep = true,
            Some(flag_name) => {
                let value = arguments
                    .next()
                    .ok_or_else(|| format!("missing value for {flag_name}"))?;
                match flag_name {
                    "--model-root" => result.model_root = PathBuf::from(value),
                    "--width" => result.width = parse_number(&value, "--width")?,
                    "--height" => result.height = parse_number(&value, "--height")?,
                    "--warm-up" => result.warm_up = parse_number(&value, "--warm-up")?,
                    "--iterations" => result.iterations = parse_number(&value, "--iterations")?,
                    "--threads" => result.threads = Some(parse_number(&value, "--threads")?),
                    "--format" => match value.to_str() {
                        Some("rgb8") => {
                            result.format = PixelFormat::Rgb8;
                            result.format_name = "RGB8";
                        }
                        Some("rgba8") => {
                            result.format = PixelFormat::Rgba8;
                            result.format_name = "RGBA8";
                        }
                        Some("gray8") => {
                            result.format = PixelFormat::Gray8;
                            result.format_name = "Gray8";
                        }
                        _ => return Err("--format must be rgb8, rgba8, or gray8".to_owned()),
                    },
                    _ => return Err(format!("unknown argument {flag_name}")),
                }
            }
            None => return Err("benchmark argument must be valid UTF-8".to_owned()),
        }
    }
    if result.width == 0
        || result.width > kfaceauth_vision::MAX_WIDTH
        || result.height == 0
        || result.height > kfaceauth_vision::MAX_HEIGHT
    {
        return Err("benchmark dimensions must be within 1..1920 by 1..1080".to_owned());
    }
    if result.warm_up > MAX_WARM_UP {
        return Err(format!("--warm-up must be at most {MAX_WARM_UP}"));
    }
    if result.iterations == 0 || result.iterations > MAX_ITERATIONS {
        return Err(format!("--iterations must be within 1..={MAX_ITERATIONS}"));
    }
    if result
        .threads
        .is_some_and(|threads| !(1..=16).contains(&threads))
    {
        return Err("--threads must be within 1..=16".to_owned());
    }
    if !result.model_root.is_absolute() {
        return Err("--model-root must be absolute".to_owned());
    }
    Ok(result)
}

fn parse_number<T>(value: &std::ffi::OsStr, flag: &str) -> Result<T, String>
where
    T: std::str::FromStr,
{
    value
        .to_str()
        .and_then(|text| text.parse().ok())
        .ok_or_else(|| format!("{flag} requires an unsigned integer"))
}

fn synthetic_frame(size: usize, format: PixelFormat) -> Vec<u8> {
    let mut frame = vec![0; size];
    let pixel_size = usize::try_from(format.bytes_per_pixel()).expect("pixel size fits usize");
    for (index, byte) in frame.iter_mut().enumerate() {
        let channel = index % pixel_size;
        *byte = if format == PixelFormat::Rgba8 && channel == 3 {
            u8::MAX
        } else {
            u8::try_from((index / pixel_size) % 32).expect("synthetic value fits u8")
        };
    }
    frame
}

fn measure(provider: &YuNetProvider, image: ImageView<'_>) -> Result<Duration, ()> {
    let cancellation = CancellationToken::default();
    let control =
        ProcessingControl::with_timeout(&cancellation, BENCHMARK_TIMEOUT).map_err(|_| ())?;
    let started = Instant::now();
    let _neutral_result = provider.analyze(image, control).map_err(|_| ())?;
    Ok(started.elapsed())
}

fn measure_thread_sweep(provider: &YuNetProvider, image: ImageView<'_>) -> String {
    let original = thread_count();
    let mut entries = String::from("[");
    for (index, threads) in [1_u32, 2, 4, 8, 16].into_iter().enumerate() {
        if set_thread_count(threads).is_err() {
            let _ = set_thread_count(original);
            return "null".to_owned();
        }
        let _ = measure(provider, image);
        let mut samples = [
            measure(provider, image),
            measure(provider, image),
            measure(provider, image),
        ]
        .into_iter()
        .collect::<Result<Vec<_>, _>>()
        .unwrap_or_default();
        if samples.is_empty() {
            let _ = set_thread_count(original);
            return "null".to_owned();
        }
        samples.sort_unstable();
        let median = percentile(&samples, 1, 2);
        let p95 = percentile(&samples, 95, 100);
        if index > 0 {
            entries.push(',');
        }
        write!(
            &mut entries,
            "{{\"threads\":{threads},\"median_ms\":{:.3},\"p95_ms\":{:.3}}}",
            milliseconds(median),
            milliseconds(p95)
        )
        .expect("writing benchmark JSON cannot fail");
    }
    let _ = set_thread_count(original);
    entries.push(']');
    entries
}

fn percentile(values: &[Duration], numerator: usize, denominator: usize) -> Duration {
    let rank = values
        .len()
        .checked_mul(numerator)
        .expect("bounded iteration count")
        .div_ceil(denominator);
    values[rank.saturating_sub(1).min(values.len() - 1)]
}

fn milliseconds(value: Duration) -> f64 {
    value.as_secs_f64() * 1_000.0
}

fn peak_memory_kib() -> Option<u64> {
    let status = std::fs::read_to_string("/proc/self/status").ok()?;
    let line = status.lines().find(|line| line.starts_with("VmHWM:"))?;
    line.split_whitespace().nth(1)?.parse().ok()
}

fn escape_json(value: &str) -> String {
    let mut result = String::with_capacity(value.len());
    for character in value.chars() {
        match character {
            '"' => result.push_str("\\\""),
            '\\' => result.push_str("\\\\"),
            '\u{08}' => result.push_str("\\b"),
            '\u{0c}' => result.push_str("\\f"),
            '\n' => result.push_str("\\n"),
            '\r' => result.push_str("\\r"),
            '\t' => result.push_str("\\t"),
            value if value.is_control() => {
                write!(&mut result, "\\u{:04x}", u32::from(value))
                    .expect("writing to a String cannot fail");
            }
            value => result.push(value),
        }
    }
    result
}
