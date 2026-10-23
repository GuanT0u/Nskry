"""One bounded offline translation task; packaged separately from model conversion tools."""
import json
import sys
from pathlib import Path
import ctypes
from ctypes import wintypes


def crypt(data, decrypt=False):
    class Blob(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]
    buffer = ctypes.create_string_buffer(data)
    source = Blob(len(data), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_ubyte)))
    result = Blob()
    function = getattr(ctypes.WinDLL("crypt32", use_last_error=True),
                       "CryptUnprotectData" if decrypt else "CryptProtectData")
    if not function(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(result)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return ctypes.string_at(result.data, result.size)
    finally:
        release = ctypes.WinDLL("kernel32").LocalFree
        release.argtypes = [ctypes.c_void_p]
        release.restype = ctypes.c_void_p
        release(result.data)


def translate(root, direction, text):
    if direction not in ("en-zh", "zh-en"):
        raise ValueError("Only English/Chinese directions are supported by this language pack.")
    if not text.strip() or len(text.encode("utf-16-le")) > 32000 or "\x00" in text:
        raise ValueError("Select nonempty text within 16000 UTF-16 units.")
    import ctranslate2
    import sentencepiece
    source = sentencepiece.SentencePieceProcessor(model_file=str(root / direction / "source.spm"))
    target = sentencepiece.SentencePieceProcessor(model_file=str(root / direction / "target.spm"))
    engine = ctranslate2.Translator(str(root / direction / "int8"), device="cpu", compute_type="int8",
                                    inter_threads=1, intra_threads=2)
    output = []
    for line in text.splitlines(keepends=True):
        content = line.rstrip("\r\n")
        ending = line[len(content):]
        if not content.strip():
            output.append(line)
            continue
        tokens = source.encode(content, out_type=str) + ["</s>"]
        if len(tokens) > 256:
            raise ValueError("A paragraph exceeds the 256-token MVP limit. Select a smaller region or add line breaks.")
        result = engine.translate_batch([tokens], beam_size=2, max_input_length=256, max_decoding_length=256)[0]
        if len(result.hypotheses[0]) >= 256:
            raise ValueError("Translation reached its output limit; no partial result was published.")
        output.append(target.decode([t for t in result.hypotheses[0] if t not in ("</s>", "<pad>")]) + ending)
    return "".join(output)


def main():
    if len(sys.argv) != 5:
        return 2
    root, direction, source, destination = sys.argv[1:]
    source = Path(source)
    if source.stat().st_size > 128000:
        return 3
    try:
        output = translate(Path(root), direction, crypt(source.read_bytes(), decrypt=True).decode("utf-8"))
        reply = {"ok": True, "text": output}
    except Exception:
        # Do not log screenshot text, model internals or full local paths.
        reply = {"ok": False, "error": "Offline translation failed. Check the language pack and paragraph limits."}
    encoded = json.dumps(reply, ensure_ascii=True).encode("ascii")
    if len(encoded) > 2 * 1024 * 1024:
        return 4
    with open(destination, "xb") as stream:
        stream.write(crypt(encoded))
    return 0 if reply["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
