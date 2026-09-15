# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "audit_quantization.py"
SPEC = importlib.util.spec_from_file_location("audit_quantization", TOOL)
assert SPEC is not None and SPEC.loader is not None
audit_quantization = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = audit_quantization
SPEC.loader.exec_module(audit_quantization)


def vector(index: int, delta: float = 0.0) -> list[float]:
    values = [0.0] * 128
    values[index] = 1.0
    if delta:
        values[(index + 1) % 128] = delta
    return values


class QuantizationAuditTests(unittest.TestCase):
    def write_jsonl(self, path: Path, rows: list[dict[str, object]]) -> None:
        path.write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8"
        )

    def test_metadata_only_is_explicitly_not_run(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fp32 = root / "fp32.onnx"
            int8 = root / "int8.onnx"
            fp32.write_bytes(b"fp32")
            int8.write_bytes(b"int8")
            result = subprocess.run(
                [sys.executable, str(TOOL), "--fp32-model", str(fp32), "--int8-model", str(int8)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stdout)["status"], "not-run")

    def test_lfw_and_ijbc_audit_reports_aggregate_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fp32_model = root / "fp32.onnx"
            int8_model = root / "int8.onnx"
            fp32_model.write_bytes(b"fp32")
            int8_model.write_bytes(b"int8")
            fp32_rows = [
                {"id": "a", "embedding": vector(0)},
                {"id": "b", "embedding": vector(0)},
                {"id": "c", "embedding": vector(1)},
                {"id": "d", "embedding": vector(1)},
            ]
            int8_rows = [
                {"id": "a", "embedding": vector(0, 1.0e-5)},
                {"id": "b", "embedding": vector(0, 1.0e-5)},
                {"id": "c", "embedding": vector(1, 1.0e-5)},
                {"id": "d", "embedding": vector(1, 1.0e-5)},
            ]
            fp32_embeddings = root / "fp32.jsonl"
            int8_embeddings = root / "int8.jsonl"
            pairs = root / "pairs.jsonl"
            self.write_jsonl(fp32_embeddings, fp32_rows)
            self.write_jsonl(int8_embeddings, int8_rows)
            self.write_jsonl(
                pairs,
                [
                    {"dataset": "LFW", "left": "a", "right": "b", "same": True},
                    {"dataset": "LFW", "left": "a", "right": "c", "same": False},
                    {"dataset": "IJB-C", "left": "c", "right": "d", "same": True},
                    {"dataset": "IJB-C", "left": "b", "right": "d", "same": False},
                ],
            )
            result = subprocess.run(
                [
                    sys.executable,
                    str(TOOL),
                    "--fp32-model",
                    str(fp32_model),
                    "--int8-model",
                    str(int8_model),
                    "--pairs",
                    str(pairs),
                    "--fp32-embeddings",
                    str(fp32_embeddings),
                    "--int8-embeddings",
                    str(int8_embeddings),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["status"], "complete")
            self.assertEqual(report["datasets"], ["ijbc", "lfw"])
            self.assertTrue(report["embedding_cosine_divergence"]["passes"])
            self.assertEqual(report["per_dataset"]["lfw"]["fp32"]["false_accept"], 0)


if __name__ == "__main__":
    unittest.main()
