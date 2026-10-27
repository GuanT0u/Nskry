"""Explicit local diagnostic, public fixed text only; no downloads or prewarming.

Profiles the prepared development interpreter, not PyInstaller boot time. Each
round is a fresh process. Nothing here is imported by the shipping plugin.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import time

TEXT = {"en-zh": "Save the screenshot to the clipboard.",
        "zh-en": "将截图保存到剪贴板。"}


def memory():
    class Counters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in ("peak_ws", "ws", "peak_paged", "paged",
                "peak_nonpaged", "nonpaged", "commit", "peak_commit", "private")]
    value = Counters()
    value.cb = ctypes.sizeof(value)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(value), value.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return {key: getattr(value, key) for key in ("private", "ws", "peak_ws", "peak_commit")}


def child(root, direction):
    started = time.perf_counter()
    stages = {"entry": memory()}
    import ctranslate2
    stages["ctranslate2"] = memory()
    import sentencepiece
    stages["imports"] = memory()
    timings = {"imports_ms": (time.perf_counter()-started)*1000, "model_ms": 0,
               "tokenizers_ms": 0, "inference_ms": 0, "prefix_ms": 0}
    constructor = ctranslate2.Translator
    tokenizer = sentencepiece.SentencePieceProcessor

    class Engine:
        def __init__(self, *args, **kwargs):
            start = time.perf_counter()
            self.value = constructor(*args, **kwargs)
            timings["model_ms"] += (time.perf_counter()-start)*1000
            stages["model"] = memory()

        def translate_batch(self, *args, **kwargs):
            start = time.perf_counter()
            result = self.value.translate_batch(*args, **kwargs)
            timings["inference_ms"] += (time.perf_counter()-start)*1000
            return result

    def make_tokenizer(*args, **kwargs):
        start = time.perf_counter()
        result = tokenizer(*args, **kwargs)
        timings["tokenizers_ms"] += (time.perf_counter()-start)*1000
        return result

    ctranslate2.Translator = Engine
    sentencepiece.SentencePieceProcessor = make_tokenizer
    source = Path(__file__).resolve().parents[1]/"src/offline_worker.py"
    spec = importlib.util.spec_from_file_location("profiled_worker", source)
    worker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(worker)
    original_prefix = worker.source_prefix

    def prefix(*args):
        start = time.perf_counter()
        result = original_prefix(*args)
        timings["prefix_ms"] += (time.perf_counter()-start)*1000
        return result

    worker.source_prefix = prefix
    metrics = {}
    text = worker.translate(root, direction, TEXT[direction], metrics)
    stages["finished"] = memory()
    print(json.dumps({"direction": direction, "versions": {"python": sys.version.split()[0],
        "ctranslate2": ctranslate2.__version__, "sentencepiece": sentencepiece.__version__},
        **timings, "body_ms": (time.perf_counter()-started)*1000,
        "process_cpu_ms": time.process_time()*1000, "memory": stages, "parts": metrics["parts"],
        "text_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest()}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-python", type=Path)
    parser.add_argument("--language-root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--child", choices=list(TEXT), help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        child(args.language_root.resolve(), args.child)
        return
    if not args.runtime_python or not args.output or not 1 <= args.rounds <= 20:
        parser.error("Supply runtime-python, a new output file and rounds within 1..20")
    records = []
    for round_ in range(args.rounds):
        for direction in TEXT:
            start = time.perf_counter()
            result = subprocess.run([str(args.runtime_python.resolve()), str(Path(__file__).resolve()),
                "--language-root", str(args.language_root.resolve()), "--child", direction],
                check=True, capture_output=True, text=True, timeout=60)
            row = json.loads(result.stdout)
            row.update(round=round_, process_wall_ms=(time.perf_counter()-start)*1000)
            records.append(row)
    with args.output.open("x", encoding="utf-8") as file:
        json.dump({"scope": "development interpreter; fixed public text; fresh process each round",
                   "records": records}, file, indent=2)
    print(args.output.resolve())


if __name__ == "__main__":
    main()
