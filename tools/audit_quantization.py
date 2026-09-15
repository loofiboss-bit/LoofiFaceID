#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Audit FP32/INT8 embedding parity and benchmark FAR/FRR drift.

The utility is deliberately offline and consumes permissioned, precomputed
embedding vectors. It does not open images, contact a network, or emit pair
scores. This keeps benchmark data outside the repository while making the
qualification calculation reproducible.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
from typing import Any, Iterable

EMBEDDING_DIMENSION = 128
MAX_MODEL_BYTES = 64 * 1024 * 1024
MAX_INPUT_LINE_BYTES = 256 * 1024
DEFAULT_THRESHOLD = 0.45
DEFAULT_MAX_EMBEDDING_DIVERGENCE = 1.0e-4
REQUIRED_DATASETS = frozenset({"lfw", "ijbc"})


class AuditError(ValueError):
    """A stable, user-safe audit input error."""


def _read_json_lines(path: Path) -> Iterable[dict[str, Any]]:
    try:
        with path.open("rb") as stream:
            for line_number, raw_line in enumerate(stream, start=1):
                if len(raw_line) > MAX_INPUT_LINE_BYTES:
                    raise AuditError(f"line-too-large:{path.name}:{line_number}")
                line = raw_line.strip()
                if not line:
                    continue
                try:
                    value = json.loads(line)
                except json.JSONDecodeError as error:
                    raise AuditError(f"invalid-json:{path.name}:{line_number}") from error
                if not isinstance(value, dict):
                    raise AuditError(f"object-required:{path.name}:{line_number}")
                yield value
    except OSError as error:
        raise AuditError(f"read-failed:{path}") from error


def _model_metadata(path: Path) -> dict[str, Any]:
    try:
        stat = path.stat()
    except OSError as error:
        raise AuditError(f"model-stat-failed:{path}") from error
    if not path.is_file() or stat.st_size <= 0 or stat.st_size > MAX_MODEL_BYTES:
        raise AuditError(f"model-size:{path}")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise AuditError(f"model-read-failed:{path}") from error
    return {"bytes": stat.st_size, "sha256": digest.hexdigest()}


def _finite_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise AuditError(f"invalid-number:{field}")
    converted = float(value)
    if not math.isfinite(converted):
        raise AuditError(f"non-finite-number:{field}")
    return converted


def _load_embeddings(path: Path) -> dict[str, tuple[float, ...]]:
    embeddings: dict[str, tuple[float, ...]] = {}
    for row in _read_json_lines(path):
        identifier = row.get("id")
        values = row.get("embedding")
        if not isinstance(identifier, str) or not identifier or len(identifier) > 256:
            raise AuditError(f"invalid-embedding-id:{path.name}")
        if identifier in embeddings:
            raise AuditError(f"duplicate-embedding-id:{path.name}")
        if not isinstance(values, list) or len(values) != EMBEDDING_DIMENSION:
            raise AuditError(f"embedding-dimension:{path.name}:{identifier}")
        vector = tuple(
            _finite_number(value, f"embedding:{path.name}:{identifier}") for value in values
        )
        norm = math.sqrt(sum(value * value for value in vector))
        if not math.isfinite(norm) or norm <= 1.0e-12:
            raise AuditError(f"embedding-zero-norm:{path.name}:{identifier}")
        embeddings[identifier] = vector
    if not embeddings:
        raise AuditError(f"embedding-empty:{path.name}")
    return embeddings


def _normalise_dataset(value: Any) -> str:
    if not isinstance(value, str):
        raise AuditError("invalid-dataset")
    normalised = value.lower().replace("-", "")
    if normalised == "lfw":
        return "lfw"
    if normalised == "ijbc":
        return "ijbc"
    raise AuditError(f"unsupported-dataset:{value}")


def _load_pairs(path: Path) -> list[dict[str, Any]]:
    pairs: list[dict[str, Any]] = []
    for row in _read_json_lines(path):
        dataset = _normalise_dataset(row.get("dataset"))
        left = row.get("left")
        right = row.get("right")
        same = row.get("same")
        if (
            not isinstance(left, str)
            or not left
            or len(left) > 256
            or not isinstance(right, str)
            or not right
            or len(right) > 256
            or not isinstance(same, bool)
        ):
            raise AuditError("invalid-pair")
        pairs.append({"dataset": dataset, "left": left, "right": right, "same": same})
    if not pairs:
        raise AuditError(f"pairs-empty:{path.name}")
    return pairs


def _cosine(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    left_norm = math.sqrt(sum(value * value for value in left))
    right_norm = math.sqrt(sum(value * value for value in right))
    score = sum(a * b for a, b in zip(left, right, strict=True)) / (left_norm * right_norm)
    if not math.isfinite(score):
        raise AuditError("non-finite-cosine")
    return max(-1.0, min(1.0, score))


def _classification(pairs: list[dict[str, Any]], embeddings: dict[str, tuple[float, ...]], threshold: float) -> dict[str, Any]:
    total = genuine = impostor = false_reject = false_accept = 0
    maximum_pair_drift = 0.0
    for pair in pairs:
        try:
            left = embeddings[pair["left"]]
            right = embeddings[pair["right"]]
        except KeyError as error:
            raise AuditError(f"pair-embedding-missing:{error.args[0]}") from error
        score = _cosine(left, right)
        total += 1
        if pair["same"]:
            genuine += 1
            false_reject += score < threshold
        else:
            impostor += 1
            false_accept += score >= threshold
        maximum_pair_drift = max(maximum_pair_drift, abs(score))
    if total == 0:
        raise AuditError("no-pairs")
    return {
        "pairs": total,
        "genuine_pairs": genuine,
        "impostor_pairs": impostor,
        "false_reject": false_reject,
        "false_accept": false_accept,
        "frr": false_reject / genuine if genuine else None,
        "far": false_accept / impostor if impostor else None,
        "maximum_observed_cosine": maximum_pair_drift,
    }


def _audit(
    fp32_model: Path,
    int8_model: Path,
    pairs_path: Path | None,
    fp32_embeddings_path: Path | None,
    int8_embeddings_path: Path | None,
    threshold: float,
    maximum_embedding_divergence: float,
) -> dict[str, Any]:
    if not math.isfinite(threshold) or not -1.0 < threshold < 1.0:
        raise AuditError("invalid-threshold")
    if not math.isfinite(maximum_embedding_divergence) or maximum_embedding_divergence < 0.0:
        raise AuditError("invalid-divergence-limit")
    models = {"fp32": _model_metadata(fp32_model), "int8": _model_metadata(int8_model)}
    inputs = (pairs_path, fp32_embeddings_path, int8_embeddings_path)
    if all(value is None for value in inputs):
        return {
            "schema": "kfaceauth-quantization-audit-v1",
            "status": "not-run",
            "reason": "benchmark vectors were not supplied",
            "threshold": threshold,
            "models": models,
        }
    if any(value is None for value in inputs):
        raise AuditError("all-benchmark-inputs-required")

    assert pairs_path is not None
    assert fp32_embeddings_path is not None
    assert int8_embeddings_path is not None
    pairs = _load_pairs(pairs_path)
    fp32 = _load_embeddings(fp32_embeddings_path)
    int8 = _load_embeddings(int8_embeddings_path)
    datasets = {_normalise_dataset(pair["dataset"]) for pair in pairs}
    if fp32.keys() != int8.keys():
        raise AuditError("embedding-id-set-mismatch")

    maximum_divergence = 0.0
    sum_divergence = 0.0
    for identifier in fp32:
        divergence = 1.0 - _cosine(fp32[identifier], int8[identifier])
        maximum_divergence = max(maximum_divergence, divergence)
        sum_divergence += divergence

    per_dataset: dict[str, Any] = {}
    for dataset in sorted(datasets):
        dataset_pairs = [pair for pair in pairs if pair["dataset"] == dataset]
        fp32_result = _classification(dataset_pairs, fp32, threshold)
        int8_result = _classification(dataset_pairs, int8, threshold)
        per_dataset[dataset] = {
            "fp32": fp32_result,
            "int8": int8_result,
            "far_drift": _difference(int8_result["far"], fp32_result["far"]),
            "frr_drift": _difference(int8_result["frr"], fp32_result["frr"]),
        }
    complete = REQUIRED_DATASETS.issubset(datasets)
    return {
        "schema": "kfaceauth-quantization-audit-v1",
        "status": "complete" if complete else "incomplete-dataset",
        "datasets": sorted(datasets),
        "threshold": threshold,
        "models": models,
        "embedding_count": len(fp32),
        "embedding_cosine_divergence": {
            "maximum": maximum_divergence,
            "mean": sum_divergence / len(fp32),
            "limit": maximum_embedding_divergence,
            "passes": maximum_divergence < maximum_embedding_divergence,
        },
        "per_dataset": per_dataset,
    }


def _difference(left: float | None, right: float | None) -> float | None:
    if left is None or right is None:
        return None
    return left - right


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fp32-model", type=Path, required=True)
    parser.add_argument("--int8-model", type=Path, required=True)
    parser.add_argument("--pairs", type=Path, help="JSONL LFW/IJB-C pair labels")
    parser.add_argument("--fp32-embeddings", type=Path, help="JSONL FP32 embeddings")
    parser.add_argument("--int8-embeddings", type=Path, help="JSONL INT8 embeddings")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    parser.add_argument(
        "--max-embedding-divergence",
        type=float,
        default=DEFAULT_MAX_EMBEDDING_DIVERGENCE,
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        result = _audit(
            arguments.fp32_model,
            arguments.int8_model,
            arguments.pairs,
            arguments.fp32_embeddings,
            arguments.int8_embeddings,
            arguments.threshold,
            arguments.max_embedding_divergence,
        )
    except AuditError as error:
        print(json.dumps({"schema": "kfaceauth-quantization-audit-v1", "status": "error", "error": str(error)}))
        return 2
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
