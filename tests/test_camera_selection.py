# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native discovery metadata without opening a host camera."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class CameraSelectionTests(unittest.TestCase):
    def test_experimental_service_loads_optional_admin_camera_config(self):
        service = (ROOT / "data/systemd/kfaceauth.service").read_text(
            encoding="utf-8"
        )
        troubleshooting = (ROOT / "docs/TROUBLESHOOTING.md").read_text(
            encoding="utf-8"
        )

        self.assertRegex(
            service,
            r"(?m)^EnvironmentFile=-/etc/kfaceauth/kfaceauth\.conf$",
        )
        self.assertIn(
            "KFACEAUTH_CAMERA_DEVICE=/dev/v4l/by-path/REPLACE_WITH_CAMERA_LINK",
            troubleshooting,
        )
        self.assertIn("systemctl restart kfaceauth.service", troubleshooting)
        self.assertIn("grayscale stream does not establish", troubleshooting)

    def test_bounded_native_metadata_policy(self):
        source = r"""
#include "camera_selection.h"
#include <assert.h>
int main(void) {
    uint32_t capture = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    assert(kfaceauth_camera_node_name("video99"));
    assert(!kfaceauth_camera_node_name("video"));
    assert(!kfaceauth_camera_node_name("video99extra"));
    assert(kfaceauth_camera_compatible(capture, V4L2_PIX_FMT_GREY));
    assert(kfaceauth_camera_compatible(capture, V4L2_PIX_FMT_YUYV));
    assert(!kfaceauth_camera_compatible(capture, V4L2_PIX_FMT_RGB24));
    assert(!kfaceauth_camera_compatible(V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING,
                                      V4L2_PIX_FMT_GREY));
    assert(!kfaceauth_camera_compatible(V4L2_CAP_VIDEO_CAPTURE, V4L2_PIX_FMT_GREY));
    assert(!kfaceauth_camera_unique(0));
    assert(kfaceauth_camera_unique(1));
    assert(!kfaceauth_camera_unique(2));
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "fixture.c").write_text(source)
            subprocess.run(["cc", "-std=c17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "engine/crypto-openssl-sys/native"),
                            str(path / "fixture.c"), "-o", str(path / "fixture")], check=True)
            subprocess.run([str(path / "fixture")], check=True)
