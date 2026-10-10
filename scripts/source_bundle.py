#!/usr/bin/env python3
"""Capture project source and a reviewable patch without tracked delivery outputs."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import zipfile
from version import ROOT, project_version

STAMP = (2026, 10, 7, 0, 0, 0)
EXCLUDED = ("work/", "outputs/", ".git/")

def digest(data):
    return hashlib.sha256(data).hexdigest()

def source_paths(args):
    return sorted({name for name in subprocess.check_output(args, cwd=ROOT).decode().split("\0")
                   if name and not name.startswith(EXCLUDED)})

def main():
    version = project_version()
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT / f"outputs/v{version}")
    parser.add_argument("--base", default="HEAD", help="Commit from which to capture the review patch")
    args = parser.parse_args()
    base = subprocess.check_output(["git", "rev-parse", "--verify", args.base + "^{commit}"], cwd=ROOT, text=True).strip()
    source_zip = args.output_dir / f"pixelbc-{version}-source.zip"
    patch_path = args.output_dir / f"pixelbc-{version}.patch"
    receipt = args.output_dir / "source-files.sha256.json"
    for path in (source_zip, patch_path, receipt):
        if path.exists():
            raise FileExistsError(f"Refusing to overwrite preserved artifact: {path}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    paths = source_paths(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"])
    source = {}
    with zipfile.ZipFile(source_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in paths:
            path = ROOT / name
            if not path.is_file():
                continue
            data = path.read_bytes()
            source[name] = digest(data)
            entry = zipfile.ZipInfo(name, STAMP)
            entry.create_system = 3
            entry.external_attr = (0o100755 if path.stat().st_mode & 0o111 else 0o100644) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    receipt.write_text(json.dumps({"version": version, "base_commit": base, "files": source}, indent=2) + "\n")
    patch = subprocess.check_output(["git", "diff", "--binary", base, "--", ".", ":(exclude)outputs", ":(exclude)work"], cwd=ROOT)
    untracked = source_paths(["git", "ls-files", "--others", "--exclude-standard", "-z"])
    for name in untracked:
        if not (ROOT / name).is_file():
            continue
        result = subprocess.run(["git", "diff", "--no-index", "--binary", "/dev/null", name], cwd=ROOT, stdout=subprocess.PIPE, check=False)
        assert result.returncode == 1, (name, result.returncode)
        patch += result.stdout
    patch_path.write_bytes(patch)
    print(json.dumps({"version": version, "source_files": len(source), "source_sha256": digest(source_zip.read_bytes()), "patch_sha256": digest(patch), "base_commit": base}, indent=2))

if __name__ == "__main__":
    main()
