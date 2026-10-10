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
    parser.add_argument("--variant", choices=("", "weighted", "fast", "bc1-specialized", "shape-rgba"), default="")
    args = parser.parse_args()
    for path in (args.output, args.output.with_suffix(".manifest.json"), args.output.with_suffix(".elf.txt")):
        if path.exists():
            raise FileExistsError(f"Refusing to overwrite preserved artifact: {path}")
    cache = (args.build_dir / "CMakeCache.txt").read_text()
    assert re.search(r"^CMAKE_PROJECT_VERSION:STATIC=" + re.escape(VERSION) + r"$", cache, re.MULTILINE), "Build/source version mismatch"
    options = {}
    for name in ("PVB_ETC2", "PVB_UPLOAD_ARENAS", "PVB_SHARED_DECODE", "PVB_COMPACT_HISTORY", "PVB_BC1_SHAPE_RGBA"):
        match = re.search(r"^" + name + r":BOOL=(ON|OFF)$", cache, re.MULTILINE)
        assert match, name
        options[name] = match.group(1)
    lossy = options["PVB_ETC2"] == "ON"
    if args.variant == "shape-rgba":
        assert options == {"PVB_ETC2": "ON", "PVB_UPLOAD_ARENAS": "ON", "PVB_SHARED_DECODE": "OFF", "PVB_COMPACT_HISTORY": "ON", "PVB_BC1_SHAPE_RGBA": "ON"}, "Wrong shape candidate profile"
    assert not args.variant or lossy, "Encoder variants require the experimental ETC2 profile"
    package_version = VERSION + ("-etc2" + ("-" + args.variant if args.variant else "") + "-experimental" if lossy else "-lossless-recovery")
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
    for name in ("vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "pvb_get_stats", "pvb_get_version", "pvb_get_transfer_stats", "pvb_get_compression_stats"):
        assert re.search(r"\bFUNC\s+GLOBAL\s+DEFAULT\s+\d+\s+" + name + r"\b", symbols), name
    load_alignments = re.findall(r"^\s*LOAD\s+.*?\s+(0x[0-9a-f]+)\s*$", segments, re.MULTILINE)
    assert load_alignments and all(int(value, 16) >= 16384 for value in load_alignments), "Needs 16KiB load-segment alignment"
    meta = {
        "schemaVersion": 1, "name": f"PixelBC-{package_version}", "author": "Pixel Vulkan Wrapper contributors",
        "packageVersion": package_version, "vendor": "ARM", "driverVersion": f"Stock Mali + PixelBC {VERSION}",
        "minApi": 29, "libraryName": library.name,
        "description": ("EXPERIMENTAL: BC1_RGBA UNORM/SRGB whole images exactly 512x1024 or 256x2048 use lossless RGBA; " if lossy and options["PVB_BC1_SHAPE_RGBA"] == "ON" else "") + ("other eligible BC1/BC3 use LOSSY native ETC2, hardware checked; BC2/4/5/7 and unsupported native cases use lossless RGBA. " if lossy else "LOSSLESS RGBA BC1-5/7 recovery build. ") + "Original BC bytes retained for supported copies/readback; BC6/global BC feature unsupported. Pixel behavior, visual acceptance and performance UNVERIFIED. NFS-compatible metadata; ARM is outside the older generic ADPKG vendor enum."
    }
    notice_paths = ("docs/THIRD_PARTY.md", "licenses/bcdec-MIT.txt", "licenses/etc2_encode-MIT.txt", "licenses/detex-ISC.txt", "licenses/NDK-R30-runtime-NOTICE.txt", "licenses/Apache-2.0.txt")
    notices = b"\n\n".join((ROOT / path).read_bytes() for path in notice_paths)
    entries = {"meta.json": (json.dumps(meta, indent=2) + "\n").encode(), library.name: library.read_bytes(), "LICENSE.txt": (ROOT / "LICENSE").read_bytes(), "NOTICE.txt": notices}
    write_zip(args.output, entries)
    manifest = {"version": VERSION, "encoder_variant": args.variant or "frozen", "package": args.output.name, "options": options, "lossy_sampling": lossy, "lossy_eligible": ["BC1_RGB", "BC1_RGBA", "BC3"] if lossy else [], "bc2_lossless": True, "original_bc_readback_exact": True, "wrapper_nodelete": True, "stock_backend_process_lifetime": True, "sha256": sha(args.output.read_bytes()), "entries": {n: sha(d) for n,d in sorted(entries.items())}, "needed": needed, "elf_machine": "AArch64", "hal_object_bytes": 248, "minimum_load_alignment": 16384, "phone_runtime_verified": False, "visual_acceptance_verified": False, "performance_verified": False, "probe_sha256": sha(probe.read_bytes())}
    manifest["whole_image_lossless_policy"] = {"enabled": options["PVB_BC1_SHAPE_RGBA"] == "ON", "formats": [133, 134], "top_level_shapes": [[512, 1024], [256, 2048]], "all_mips_layers": True, "immutable_at_creation": True}
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
