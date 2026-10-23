"""Explicit local-only package. No dependency downloads, installs or release bundling."""
import argparse
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("stage", type=Path, help="Built DLL stage with complete engine/ directory")
parser.add_argument("output", type=Path, help="New local development .nskryplugin path")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
plugin = Path(__file__).resolve().parents[1]
stage = args.stage.resolve()
if not (stage / "engine/nskry-translate-worker.exe").is_file():
    raise SystemExit("Prepare/freeze the complete engine first")
files = [(plugin / name, name) for name in ("manifest.json", "README.md", "DEVELOPMENT_ONLY.md")]
files += [(repo / name, name) for name in ("LICENSE", "COPYING", "PLUGIN_EXCEPTION")]
files.append((stage / "nskry-translate.dll", "nskry-translate.dll"))
for file in (stage / "engine").rglob("*"):
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
