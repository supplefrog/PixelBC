#!/usr/bin/env python3
"""Read the single release version shared by CMake and artifact scripts."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent

def project_version():
    match = re.search(r"project\(pixel_vulkan_wrapper VERSION (\d+\.\d+\.\d+)\b", (ROOT / "CMakeLists.txt").read_text())
    if not match:
        raise ValueError("Missing CMake project version")
    return match.group(1)
