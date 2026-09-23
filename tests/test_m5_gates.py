from __future__ import annotations

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UnqualifiedLivenessBoundaryTests(unittest.TestCase):
    def test_texture_analysis_errors_fail_closed_in_code(self) -> None:
        liveness = (ROOT / "engine/vision/src/liveness.rs").read_text(
            encoding="utf-8"
        )
        identity = (ROOT / "engine/vision/src/identity.rs").read_text(
            encoding="utf-8"
        )

        self.assertIn("Err(_) => return LivenessDecision::AnalysisFailed", liveness)
        self.assertIn("LivenessDecision::AnalysisFailed", identity)
        self.assertIn("IdentityError::LivenessUnavailable", identity)

    def test_attack_and_user_thresholds_are_requirements_not_results(self) -> None:
        status = (ROOT / "docs/RELEASE-QUALIFICATION-V5.1.md").read_text(
            encoding="utf-8"
        )
        old_report = (ROOT / "docs/QUALIFICATION-V5.md").read_text(encoding="utf-8")
        changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")

        self.assertIn("not release-qualified", status)
        self.assertIn("At least 300 consent-based photo presentations", status)
        self.assertIn("At least 300 consent-based screen presentations", status)
        self.assertIn("BPCER <= 5%", status)
        self.assertIn("At least 3,000 comparisons", status)
        self.assertIn("**Not run**", status)
        self.assertIn("are withdrawn", old_report.lower())
        self.assertIn("anti-spoofing system", old_report.lower())
        self.assertIn("historical claims correction", changelog.lower())
        self.assertNotIn("0.0% APCER", changelog)
        self.assertNotIn("certifying 0.0% APCER", old_report)

    def test_historical_test_matrix_does_not_claim_iso_or_pad_qualification(self) -> None:
        matrix = (ROOT / "docs/TEST-MATRIX.md").read_text(encoding="utf-8")
        status = (ROOT / "docs/RELEASE-QUALIFICATION-V5.1.md").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "no physical presentation studies, no qualified anti-spoofing system",
            matrix,
        )
        self.assertIn("**Open**", status)
        self.assertIn("Internal tests do not establish ISO/IEC 30107-3 certification", status)
        self.assertNotIn("Gate 5.1: PASS", matrix)
        self.assertNotIn("0/50 print attacks", matrix)


if __name__ == "__main__":
    unittest.main()
