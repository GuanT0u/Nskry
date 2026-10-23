"""Opt-in developer probe. Never run by CMake, plugin init, or application startup.

Prepare in a development venv with torch, transformers, CTranslate2 and
sentencepiece. Runtime probes import only CTranslate2 and sentencepiece.
Public synthetic fixtures only; no screenshot/user text is uploaded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

MODELS = {"en-zh": "Helsinki-NLP/opus-mt-en-zh", "zh-en": "Helsinki-NLP/opus-mt-zh-en"}
REVISIONS = {"en-zh": "408d9bc410a388e1d9aef112a2daba955b945255",
             "zh-en": "cf109095479db38d6df799875e34039d4938aaa6"}
FILES = ["config.json", "generation_config.json", "tokenizer_config.json", "vocab.json",
         "source.spm", "target.spm", "pytorch_model.bin", "README.md"]
FIXTURES = {
    "en-zh": ["Hello, how are you?", "Save the screenshot to the clipboard.",
              "The recording is paused. Click Resume to continue.",
              "Do not close the application until the download is complete."],
    "zh-en": ["你好，今天怎么样？", "将截图保存到剪贴板。", "录制已暂停，点击继续恢复录制。",
              "下载完成之前，请不要关闭应用程序。"],
}


def prepare(root):
    import torch
    from huggingface_hub import hf_hub_download
    from ctranslate2.converters import TransformersConverter
    torch.set_num_threads(2)
    for direction, model in MODELS.items():
        revision = REVISIONS[direction]
        source = root / direction / "source"
        converted = root / direction / "int8"
        manifest = {"model": model, "revision": revision, "files": {}}
        for name in FILES:
            path = Path(hf_hub_download(model, name, revision=revision, local_dir=source, token=False))
            manifest["files"][name] = hashlib.sha256(path.read_bytes()).hexdigest()
        # No remote Python code. torch>=2.6 uses weights-only loading for .bin.
        if converted.exists():
            previous = json.loads((root / direction / "provenance.json").read_text(encoding="utf-8"))
            if previous["revision"] != revision or previous["files"] != manifest["files"]:
                raise ValueError("Existing conversion has different provenance. Choose a new output directory.")
        else:
            TransformersConverter(str(source)).convert(str(converted), quantization="int8")
        manifest["int8_files"] = {
            str(path.relative_to(converted)): {"bytes": path.stat().st_size,
                                              "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in converted.rglob("*") if path.is_file()
        }
        (root / direction / "provenance.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        print(direction, "int8_bytes=", sum(x["bytes"] for x in manifest["int8_files"].values()), flush=True)


def worker(root, direction):
    started = time.perf_counter()
    import ctranslate2
    import sentencepiece as sp
    source = sp.SentencePieceProcessor(model_file=str(root / direction / "source" / "source.spm"))
    target = sp.SentencePieceProcessor(model_file=str(root / direction / "source" / "target.spm"))
    translator = ctranslate2.Translator(str(root / direction / "int8"), device="cpu",
                                      compute_type="int8", inter_threads=1, intra_threads=2)
    loaded = time.perf_counter()
    rows = []
    for text in FIXTURES[direction]:
        begin = time.perf_counter()
        tokens = source.encode(text, out_type=str) + ["</s>"]
        result = translator.translate_batch([tokens], beam_size=2, max_input_length=256,
                                            max_decoding_length=256)[0]
        output = target.decode([x for x in result.hypotheses[0] if x not in ("</s>", "<pad>")])
        rows.append({"source": text, "translation": output, "inference_ms": (time.perf_counter()-begin)*1000})
    # This is a process-lifetime probe; interpreter and model die on exit.
    return {"direction": direction, "load_ms": (loaded-started)*1000,
            "elapsed_ms": (time.perf_counter()-started)*1000, "rows": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--worker", choices=MODELS)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.prepare:
        root.mkdir(parents=True, exist_ok=True)
        prepare(root)
    elif args.worker:
        print(json.dumps(worker(root, args.worker), ensure_ascii=True))
    else:
        reports = []
        for direction in MODELS:
            for run in range(3):
                begin = time.perf_counter()
                child = subprocess.run([sys.executable, str(Path(__file__).resolve()), str(root),
                                        "--worker", direction], capture_output=True, text=True,
                                       encoding="utf-8", timeout=60, check=True)
                report = json.loads(child.stdout)
                report.update(run=run, process_wall_ms=(time.perf_counter()-begin)*1000, exited=True)
                reports.append(report)
        (root / "probe-results.json").write_text(json.dumps(reports, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(reports, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
