#!/usr/bin/env python3
"""Create a deterministic NFS-compatible flat driver ZIP; validate ABI and dependencies."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import zipfile
from version import project_version

ROOT = pathlib.Path(__file__).resolve().parent.parent
STAMP = (2026, 10, 7, 0, 0, 0)
VERSION = project_version()
PACKAGE_VERSION = VERSION + "-candidate"

def sha(data):
    return hashlib.sha256(data).hexdigest()

def write_zip(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / f"outputs/v{VERSION}/pixelbc-{VERSION}-candidate.zip")
    parser.add_argument("--readelf", required=True)
    args = parser.parse_args()
    for path in (args.output, args.output.with_suffix(".manifest.json"), args.output.with_suffix(".elf.txt")):
        if path.exists():
            raise FileExistsError(f"Refusing to overwrite preserved artifact: {path}")
    cache = (args.build_dir / "CMakeCache.txt").read_text()
    assert re.search(r"^CMAKE_PROJECT_VERSION:STATIC=" + re.escape(VERSION) + r"$", cache, re.MULTILINE), "Build/source version mismatch"
    library = args.build_dir / "libvulkan_pixel_bc.so"
    probe = args.build_dir / "pixel_bc_probe"
    dynamic = subprocess.check_output([args.readelf, "-d", str(library)], text=True)
    header = subprocess.check_output([args.readelf, "-h", str(library)], text=True)
    symbols = subprocess.check_output([args.readelf, "--dyn-syms", "--wide", str(library)], text=True)
    segments = subprocess.check_output([args.readelf, "-l", str(library)], text=True)
    assert "AArch64" in header and "ELF64" in header
    needed = re.findall(r"Shared library: \[(.*?)\]", dynamic)
    assert set(needed) <= {"libc.so", "libm.so", "libdl.so", "liblog.so"}, needed
    assert "libvulkan" not in " ".join(needed) and not re.search(r"\b(?:RPATH|RUNPATH)\b", dynamic)
    assert re.search(r"\b248\s+OBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+HMI\b", symbols), "Missing exact LP64 HMI object"
    assert re.search(r"FLAGS_1.*NODELETE", dynamic), "Android wrapper must retain process-lifetime callback code"
    for name in ("vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "pvb_get_stats", "pvb_get_version", "pvb_get_transfer_stats"):
        assert re.search(r"\bFUNC\s+GLOBAL\s+DEFAULT\s+\d+\s+" + name + r"\b", symbols), name
    load_alignments = re.findall(r"^\s*LOAD\s+.*?\s+(0x[0-9a-f]+)\s*$", segments, re.MULTILINE)
    assert load_alignments and all(int(value, 16) >= 16384 for value in load_alignments), "Needs 16KiB load-segment alignment"
    meta = {
        "schemaVersion": 1, "name": f"PixelBC-{PACKAGE_VERSION}", "author": "Pixel Vulkan Wrapper contributors",
        "packageVersion": PACKAGE_VERSION, "vendor": "ARM", "driverVersion": f"Stock Mali + BC1-5/7 GPU decode {VERSION}",
        "minApi": 29, "libraryName": library.name,
        "description": "Pixel 7 stock-Mali Android HAL wrapper candidate. RGBA BC1-5/7 GPU decoding and retained original blocks for supported copies/readback; BC6/core BC1-7 feature stays unsupported. This candidate's phone behavior and performance UNVERIFIED. NFS-compatible metadata; ARM is outside the older generic ADPKG vendor enum."
    }
    notices = (ROOT / "docs/THIRD_PARTY.md").read_bytes() + b"\n\n" + (ROOT / "licenses/bcdec-MIT.txt").read_bytes() + b"\n\n" + (ROOT / "licenses/NDK-R30-runtime-NOTICE.txt").read_bytes() + b"\n\n" + (ROOT / "licenses/Apache-2.0.txt").read_bytes()
    entries = {"meta.json": (json.dumps(meta, indent=2) + "\n").encode(), library.name: library.read_bytes(), "LICENSE.txt": (ROOT / "LICENSE").read_bytes(), "NOTICE.txt": notices}
    write_zip(args.output, entries)
    manifest = {"version": VERSION, "package": args.output.name, "wrapper_nodelete": True, "stock_backend_process_lifetime": True, "sha256": sha(args.output.read_bytes()), "entries": {n: sha(d) for n,d in sorted(entries.items())}, "needed": needed, "elf_machine": "AArch64", "hal_object_bytes": 248, "minimum_load_alignment": 16384, "phone_runtime_verified": False, "performance_verified": False, "probe_sha256": sha(probe.read_bytes())}
    args.output.with_suffix(".manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    args.output.with_suffix(".elf.txt").write_text(header + dynamic + symbols + segments)
    with zipfile.ZipFile(args.output) as archive:
        assert len(archive.namelist()) == len(set(archive.namelist())) == 4
        assert all(pathlib.PurePosixPath(n).name == n for n in archive.namelist())
        assert json.loads(archive.read("meta.json"))["libraryName"] in archive.namelist()
        assert not archive.testzip()
    print(json.dumps(manifest, indent=2))

if __name__ == "__main__":
    main()
