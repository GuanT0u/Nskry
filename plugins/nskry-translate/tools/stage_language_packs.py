"""Explicit developer staging; copy data only, never install into a user profile."""
import argparse
from pathlib import Path
import shutil
import hashlib
import json

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
for direction in ("en-zh", "zh-en"):
    source = args.source / direction
    provenance = json.loads((source / "provenance.json").read_text(encoding="utf-8"))
    # Do not attach a valid provenance document to mutated/stale model bytes.
    for name, digest in provenance["files"].items():
        if hashlib.sha256((source / "source" / name).read_bytes()).hexdigest() != digest:
            raise ValueError("Source model hash mismatch: " + name)
    for name, metadata in provenance["int8_files"].items():
        if hashlib.sha256((source / "int8" / name).read_bytes()).hexdigest() != metadata["sha256"]:
            raise ValueError("Converted model hash mismatch: " + name)
    destination = args.destination / direction
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("source.spm", "target.spm"):
        shutil.copyfile(source / "source" / name, destination / name)
    shutil.copyfile(source / "source" / "README.md", destination / "MODEL_CARD.md")
    shutil.copyfile(source / "provenance.json", destination / "provenance.json")
    shutil.copytree(source / "int8", destination / "int8", dirs_exist_ok=True)
