"""Explicit real-engine test. Receives a built worker and prepared packs; no downloads."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import ctypes
from ctypes import wintypes
import time
import importlib.util
import argparse

spec = importlib.util.spec_from_file_location("offline_worker", Path(__file__).resolve().parents[1] /
    "plugins/nskry-translate/src/offline_worker.py")
protocol = importlib.util.module_from_spec(spec)
spec.loader.exec_module(protocol)

class Memory(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ("peak_ws", "ws", "qpp", "qp", "qpnp", "qnp", "page", "peak_page", "private")]

open_process = ctypes.windll.kernel32.OpenProcess
open_process.restype = wintypes.HANDLE

parser = argparse.ArgumentParser()
parser.add_argument("worker", type=Path)
parser.add_argument("packs", type=Path)
parser.add_argument("artifacts", type=Path)
parser.add_argument("--long", action="store_true", help="Also measure a public synthetic long paragraph")
args = parser.parse_args()
worker, packs, artifacts = args.worker, args.packs, args.artifacts
root = Path(tempfile.mkdtemp(prefix="translate-smoke-", dir=artifacts))
fixtures = [("en-zh", "Save the screenshot to the clipboard."), ("zh-en", "请不要关闭应用程序。")]
if args.long:
    fixtures.append(("en-zh", "Save the screenshot to the clipboard. Keep the application open. " * 35))
for index, (direction, text) in enumerate(fixtures):
    source = root / (str(index) + ".txt")
    output = root / (str(index) + ".json")
    source.write_bytes(protocol.crypt(text.encode("utf-8")))
    started = time.perf_counter()
    process = subprocess.Popen([str(worker.resolve()), str(packs.resolve()), direction, str(source), str(output)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    handle = open_process(0x410, False, process.pid)
    peak_private = peak_working_set = 0
    while process.poll() is None:
        if time.perf_counter() - started > 60:
            process.kill()  # Only the child created by this test.
            process.wait(timeout=5)
            raise TimeoutError("Offline worker exceeded its test deadline")
        memory = Memory(); memory.cb = ctypes.sizeof(memory)
        if ctypes.windll.psapi.GetProcessMemoryInfo(wintypes.HANDLE(handle), ctypes.byref(memory), memory.cb):
            peak_private = max(peak_private, memory.private)
            peak_working_set = max(peak_working_set, memory.peak_ws)
        time.sleep(0.01)
    created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
    cpu_ms = None
    if handle:
        if ctypes.windll.kernel32.GetProcessTimes(wintypes.HANDLE(handle), ctypes.byref(created),
                ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user)):
            cpu_ms = (((kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                      ((user.dwHighDateTime << 32) | user.dwLowDateTime)) / 10000
        ctypes.windll.kernel32.CloseHandle(wintypes.HANDLE(handle))
    stdout, stderr = process.communicate(timeout=60)
    print("exit=", process.returncode, "peak_private=", peak_private, "peak_working_set=", peak_working_set,
          "cpu_ms=", cpu_ms, "stderr=", stderr.decode("utf-8", errors="replace"))
    print("wall_ms=", (time.perf_counter() - started) * 1000)
    if process.returncode:
        raise SystemExit(1)
    reply = json.loads(protocol.crypt(output.read_bytes(), decrypt=True).decode("ascii"))
    assert reply["ok"] and reply["text"].strip()
    print("direction=", direction, "input_units=", len(text.encode("utf-16-le")) // 2,
          "output_units=", len(reply["text"].encode("utf-16-le")) // 2, "parts=", reply.get("parts"))
    if len(text) < 256:
        print(json.dumps(reply, ensure_ascii=True))
    source.unlink()
    output.unlink()
root.rmdir()
print("Artifacts:", root)
