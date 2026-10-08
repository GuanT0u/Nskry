"""Fetch pinned development inputs for the native CPU OCR worker.

No Python packages are installed. Downloads and extracted sources stay in the
workspace build directory; the distributed worker has no Python dependency.
"""
import argparse
import hashlib
import sys
from pathlib import Path
import tarfile
import urllib.request
import zipfile

ASSETS = (
    ("ort.zip", "https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-win-x64-1.29.0.zip",
     "c9b4b7086b529ad814f428c1bad028e20a25d7dc0699836775faace4ab5b78b2"),
    ("rapidocr.whl", "https://files.pythonhosted.org/packages/ba/12/1e5497183bdbe782dbb91bad1d0d2297dba4d2831b2652657f7517bfc6df/rapidocr_onnxruntime-1.4.4-py3-none-any.whl",
     "971d7d5f223a7a808662229df1ef69893809d8457d834e6373d3854bc1782cbf"),
    ("pyclipper.tar.gz", "https://files.pythonhosted.org/packages/4a/b2/550fe500e49c464d73fabcb8cb04d47e4885d6ca4cfc1f5b0a125a95b19a/pyclipper-1.3.0.post6.tar.gz",
     "42bff0102fa7a7f2abdd795a2594654d62b786d0c6cd67b72d469114fdeb608c"),
    ("opencv.zip", "https://codeload.github.com/opencv/opencv/zip/refs/tags/4.10.0",
     "3810bca2b1d1c572912df0ac3888126341f3762dfd28e91068c805fb656d0e51"),
    ("Boost-LICENSE.txt", "https://raw.githubusercontent.com/boostorg/boost/boost-1.86.0/LICENSE_1_0.txt",
     "c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566"),
    ("RapidOCR-LICENSE.txt", "https://raw.githubusercontent.com/RapidAI/RapidOCR/v1.4.4/LICENSE",
     "3e0af25fdd06aa9586ae97adb00ea927ebe5a3805ac77d2d3a81ce5f55693333"),
)


def safe_target(root, name):
    target = (root / name).resolve()
    if not target.is_relative_to(root.resolve()):
        raise ValueError("Archive entry outside dependency cache")
    return target


def main():
    if sys.version_info < (3, 11):
        raise SystemExit("Python 3.11 or newer is required for dependency preparation.")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=Path("build/ocr-deps"))
    args = parser.parse_args()
    root = args.destination.resolve()
    root.mkdir(parents=True, exist_ok=True)
    for name, url, expected in ASSETS:
        archive = root / name
        if not archive.exists():
            print("Downloading " + name, flush=True)
            temporary = archive.with_suffix(archive.suffix + ".partial")
            with urllib.request.urlopen(url, timeout=90) as response, temporary.open("wb") as output:
                while data := response.read(1024 * 1024):
                    output.write(data)
            temporary.replace(archive)
        with archive.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        print(name + " sha256=" + digest, flush=True)
        if expected and digest != expected:
            raise ValueError("Dependency SHA256 mismatch: " + name)
        marker = root / (name + ".extracted")
        if marker.exists():
            continue
        if name.endswith("tar.gz"):
            with tarfile.open(archive) as package:
                for member in package.getmembers():
                    target = safe_target(root, member.name)
                    if member.isdir():
                        target.mkdir(parents=True, exist_ok=True)
                    elif member.isfile():
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with package.extractfile(member) as source, target.open("wb") as output:
                            output.write(source.read())
                    else:
                        raise ValueError("Unsupported archive link")
        elif name.endswith(("zip", "whl")):
            with zipfile.ZipFile(archive) as package:
                for member in package.infolist():
                    target = safe_target(root, member.filename)
                    if member.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with package.open(member) as source, target.open("wb") as output:
                            output.write(source.read())
        marker.write_text(digest, encoding="ascii")
    print("Ready: " + str(root), flush=True)


if __name__ == "__main__":
    main()
