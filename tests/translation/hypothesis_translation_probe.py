"""Explicit OFFLINE diagnostic. Never called by build, CTest or product init.

Uses the exact production worker's validation/tokenization/sentence pipeline.
Only the test adapter varies compute precision/model representation and beam.
Each child loads ONE model for a fixed bounded matrix, then exits. These timings
are warm inference comparisons, NOT the plugin's cold end-to-end latency.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("production_translation_worker",
    REPO / "plugins/nskry-translate/src/offline_worker.py")
production = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(production)


def digest(path):
    # Streaming keeps provenance verification from inflating the inference
    # process's high-water memory by the entire model file size.
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def process_peaks():
    class Memory(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in
            ("peak_ws", "ws", "qpp", "qp", "qpnp", "qnp", "page", "peak_page", "private")]
    memory = Memory()
    memory.cb = ctypes.sizeof(memory)
    process = ctypes.windll.kernel32.GetCurrentProcess
    process.restype = wintypes.HANDLE
    query = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
    query.restype = wintypes.BOOL
    if not query(process(), ctypes.byref(memory), memory.cb):
        raise ctypes.WinError()
    return {"working_set_peak_bytes": memory.peak_ws, "private_commit_peak_bytes": memory.peak_page}


def prepare(source_root, output):
    # Explicit conversion from existing pinned local original weights only.
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
    import torch
    from ctranslate2.converters import TransformersConverter
    torch.set_num_threads(2)
    output.mkdir(parents=True, exist_ok=False)
    for direction in ("en-zh", "zh-en"):
        root = source_root / direction
        provenance = json.loads((root / "provenance.json").read_text(encoding="utf-8"))
        for name, expected in provenance["files"].items():
            if digest(root / "source" / name) != expected:
                raise ValueError("Pinned source changed: " + name)
        TransformersConverter(str(root / "source")).convert(str(output / direction), quantization="float32")
        manifest = {"original_model": provenance["model"], "revision": provenance["revision"],
                    "precision": "float32", "files": {p.name: digest(p) for p in (output / direction).iterdir() if p.is_file()}}
        (output / direction / "test-provenance.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print("Converted local original weights:", direction, flush=True)


def worker(args):
    import ctranslate2
    review = json.loads(args.cases.read_text(encoding="utf-8"))
    model = args.packs / args.direction / "int8" if args.precision == "int8" else args.fp32 / args.direction
    begin = time.perf_counter()
    real_engine = ctranslate2.Translator(str(model), device="cpu", compute_type=args.precision,
                                       inter_threads=1, intra_threads=2)
    load_ms = (time.perf_counter() - begin) * 1000

    class Adapter:
        def translate_batch(self, source, **kwargs):
            kwargs["beam_size"] = args.beam
            return real_engine.translate_batch(source, **kwargs)

    rows = []
    with patch.object(ctranslate2, "Translator", return_value=Adapter()):
        for case in review["cases"]:
            if case["direction"] != args.direction:
                continue
            for run in range(1, 4):
                begin = time.perf_counter()
                metrics = {}
                result = production.translate(args.packs, args.direction, case["text"], metrics)
                rows.append({**case, "run": run, "translation": result, "parts": metrics["parts"],
                             "warm_call_ms": (time.perf_counter() - begin) * 1000})
    peaks = process_peaks()
    return {"direction": args.direction, "precision": args.precision, "beam": args.beam,
            "engine_load_ms": load_ms, "cases_sha256": digest(args.cases),
            "worker_source_sha256": digest(REPO / "plugins/nskry-translate/src/offline_worker.py"),
            "model_sha256": digest(model / "model.bin"), "process_peaks": peaks, "rows": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("packs", type=Path)
    parser.add_argument("fp32", type=Path)
    parser.add_argument("cases", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--prepare-from", type=Path)
    parser.add_argument("--worker", action="store_true")
    parser.add_argument("--direction", choices=("en-zh", "zh-en"))
    parser.add_argument("--precision", choices=("int8", "float32"))
    parser.add_argument("--beam", type=int, choices=(1, 2, 4))
    args = parser.parse_args()
    if args.prepare_from:
        prepare(args.prepare_from, args.fp32)
    elif args.worker:
        print(json.dumps(worker(args), ensure_ascii=True))
    else:
        if args.output.exists():
            raise ValueError("Choose a NEW output JSON")
        for direction in ("en-zh", "zh-en"):
            provenance = json.loads((args.packs / direction / "provenance.json").read_text(encoding="utf-8"))
            for name, metadata in provenance["int8_files"].items():
                if digest(args.packs / direction / "int8" / name) != metadata["sha256"]:
                    raise ValueError("INT8 model provenance mismatch")
            for name in ("source.spm", "target.spm"):
                if digest(args.packs / direction / name) != provenance["files"][name]:
                    raise ValueError("Tokenizer provenance mismatch")
        reports = []
        for direction in ("en-zh", "zh-en"):
            for precision in ("int8", "float32"):
                for beam in (1, 2, 4):
                    command = [sys.executable, str(Path(__file__).resolve()), str(args.packs), str(args.fp32),
                               str(args.cases), str(args.output), "--worker", "--direction", direction,
                               "--precision", precision, "--beam", str(beam)]
                    begin = time.perf_counter()
                    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=90)
                    if result.returncode:
                        raise RuntimeError("Owned diagnostic child failed: " + result.stderr[-1000:])
                    report = json.loads(result.stdout)
                    report["child_wall_ms"] = (time.perf_counter() - begin) * 1000
                    report["child_exited"] = True
                    reports.append(report)
                    print(direction, precision, "beam", beam, "completed", flush=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(reports, stream, ensure_ascii=False, indent=2)


if __name__ == "__main__":
    main()
