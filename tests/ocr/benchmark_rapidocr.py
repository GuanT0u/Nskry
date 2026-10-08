"""Opt-in local engine evaluation, not a plugin dependency or online OCR client.

Uses an already installed rapidocr-onnxruntime and its local PP-OCRv4 models.
Each measured request runs in a fresh process; startup/import/model load and
shutdown are included in end_to_end_ms. No screenshots are uploaded.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time


def memory():
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (key, ctypes.c_size_t) for key in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    current = ctypes.windll.kernel32.GetCurrentProcess
    current.restype = wintypes.HANDLE
    query = ctypes.windll.psapi.GetProcessMemoryInfo
    query.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    if not query(current(), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError()
    return {key: round(getattr(counters, key) / 1048576, 2) for key in (
        "PeakWorkingSetSize", "WorkingSetSize", "PeakPagefileUsage", "PrivateUsage")}


def child(args):
    started = time.perf_counter()
    import cv2
    import rapidocr_onnxruntime
    from rapidocr_onnxruntime import RapidOCR
    from rapidocr_onnxruntime.utils.infer_engine import OrtInferSession
    # Probe-only override: the installed adapter does not expose spinning in
    # its public config. Do not edit the user's installed package.
    original_options = OrtInferSession._init_sess_opts
    def options(config):
        value = original_options(config)
        value.enable_cpu_mem_arena = args.arena
        value.add_session_config_entry("session.intra_op.allow_spinning", "0")
        value.add_session_config_entry("session.inter_op.allow_spinning", "0")
        return value
    OrtInferSession._init_sess_opts = staticmethod(options)
    cv2.setNumThreads(args.threads)
    imported = time.perf_counter()
    engine = RapidOCR(intra_op_num_threads=args.threads, inter_op_num_threads=1,
                      use_cls=False, det_use_cuda=False, rec_use_cuda=False,
                      det_use_dml=False, rec_use_dml=False,
                      det_limit_side_len=args.det_size, det_limit_type="min",
                      rec_img_shape=[3, 48, args.rec_min_width])
    loaded = time.perf_counter()
    results, stages = engine(str(Path(args.image).resolve()), use_cls=False)
    done = time.perf_counter()
    usage = memory()
    models = Path(rapidocr_onnxruntime.__file__).parent / "models"
    print(json.dumps({
        "engine": "rapidocr-onnxruntime " + importlib.metadata.version("rapidocr-onnxruntime"),
        "runtime": importlib.metadata.version("onnxruntime"), "threads": args.threads,
        "det_size": args.det_size, "rec_min_width": args.rec_min_width,
        "spinning": False, "import_ms": (imported-started)*1000,
        "arena": args.arena,
        "load_ms": (loaded-imported)*1000, "inference_ms": (done-loaded)*1000,
        "stages_s": stages, "memory_MiB": usage,
        "cpu_s": os.times().user + os.times().system,
        "models": [{"name": p.name, "bytes": p.stat().st_size,
                    "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                   for p in sorted(models.glob("*.onnx"))],
        "lines": [{"box": box, "text": text, "score": score} for box, text, score in (results or [])]
    }, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--threads", type=int, choices=(1, 2, 4), default=2)
    parser.add_argument("--det-size", type=int, default=736)
    parser.add_argument("--rec-min-width", type=int, default=320)
    parser.add_argument("--arena", action="store_true", help="Reuse tensor buffers within this one-shot worker")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.runs < 1 or args.det_size < 32 or args.rec_min_width < 16:
        parser.error("runs must be positive; det-size >= 32; rec-min-width >= 16")
    if args.child:
        child(args)
        return
    runs = []
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    for index in range(args.runs):
        start = time.perf_counter()
        process = subprocess.run([sys.executable, __file__, args.image, "--child", "--threads",
                                  str(args.threads), "--det-size", str(args.det_size),
                                  "--rec-min-width", str(args.rec_min_width)] + (["--arena"] if args.arena else []),
                                 capture_output=True, text=True, encoding="utf-8", env=env,
                                 timeout=60, check=True)
        elapsed = (time.perf_counter()-start)*1000
        run = json.loads(process.stdout)
        run.update(run=index+1, end_to_end_ms=elapsed, worker_exited=True)
        runs.append(run)
        print(f"Run {index+1}: total={elapsed:.1f}ms inference={run['inference_ms']:.1f}ms "
              f"peak_WS={run['memory_MiB']['PeakWorkingSetSize']:.1f}MiB lines={len(run['lines'])}", flush=True)
    times = sorted(r["end_to_end_ms"] for r in runs)
    report = {"image": str(Path(args.image).resolve()), "cold_process_runs": runs,
              "mean_ms": sum(times)/len(times), "p95_ms": times[math.ceil(len(times)*.95)-1],
              "note": "Fresh worker, potentially warm OS file cache; not a cold boot benchmark."}
    if args.output:
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
