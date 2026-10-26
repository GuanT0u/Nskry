"""Explicit local-only package. No dependency downloads, installs or release bundling."""
import argparse
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("stage", type=Path, help="Built DLL stage with complete engine/ directory")
parser.add_argument("output", type=Path, help="New local development .nskryplugin path")
parser.add_argument("--online-only", action="store_true", help="Omit the optional offline runtime (no Python/models needed for online engines)")
parser.add_argument("--without-image-model", action="store_true", help="Explicit Windows-OCR-only build; omit accurate image recognition assets")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
plugin = Path(__file__).resolve().parents[1]
stage = args.stage.resolve()
if not args.online_only and not (stage / "engine/nskry-translate-worker.exe").is_file():
    raise SystemExit("Prepare/freeze the complete engine first")
files = [(plugin / name, name) for name in ("manifest.json", "README.md", "ONLINE_ENGINES.md", "DEVELOPMENT_ONLY.md")]
files += [(repo / name, name) for name in ("LICENSE", "COPYING", "PLUGIN_EXCEPTION")]
files.append((stage / "nskry-translate.dll", "nskry-translate.dll"))
mode = (stage / "image-model.txt").read_text(encoding="utf-8").strip()
if mode != ("0" if args.without_image_model else "1"):
    raise SystemExit("Image-recognition packaging option does not match the configured DLL build")
files.append((stage / "image-model.txt", "image-model.txt"))
if not args.without_image_model:
    for asset in ("nskry-ocr-worker.exe", "onnxruntime.dll", "models/ch_PP-OCRv4_det_infer.onnx", "models/ch_PP-OCRv4_rec_infer.onnx", "THIRD_PARTY_NOTICES.md"):
        if not (stage / "ocr" / asset).is_file():
            raise SystemExit("Accurate recognition assets missing; build the complete plugin first")
    for file in (stage / "ocr").rglob("*"):
        if file.is_symlink() or file.is_junction():
            raise SystemExit("Links are not allowed in image recognition assets")
        if file.is_file():
            files.append((file, file.relative_to(stage).as_posix()))
for file in (() if args.online_only else (stage / "engine").rglob("*")):
    if file.is_symlink() or file.is_junction():
        raise SystemExit("Links are not allowed in plugin packages")
    if file.is_file():
        files.append((file, file.relative_to(stage).as_posix()))
if len(files) > 4096 or sum(path.stat().st_size for path, _ in files) > 512 * 1024 * 1024:
    raise SystemExit("Package exceeds host limits")
if args.output.suffix != ".nskryplugin" or args.output.parent.resolve() == repo / "installer/bundled":
    raise SystemExit("Use a local build path, not the public installer bundle")
args.output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(args.output, "x", zipfile.ZIP_DEFLATED, allowZip64=False) as archive:
    for path, name in files:
        archive.write(path, name)
if args.output.stat().st_size > 128 * 1024 * 1024:
    raise SystemExit("Compressed package exceeds host limit; do not install")
print("Local development package:", args.output.resolve(), "bytes=", args.output.stat().st_size)
