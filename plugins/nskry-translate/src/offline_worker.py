"""One bounded offline translation task; packaged separately from model conversion tools."""
import json
import sys
from pathlib import Path
import ctypes
from ctypes import wintypes
import re

MAX_TOKENS = 256
MAX_PARTS = 128
MAX_REPLY_BYTES = 2 * 1024 * 1024


class TranslationError(ValueError):
    """Only stable codes cross the process boundary, never exception/path text."""
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def split_text(text, tokenizer, budget=MAX_TOKENS - 1):
    """Lossless character slices; re-tokenize each slice, never truncate tokens.

    Sentence/word boundaries are preferred. Hard character boundaries are only
    used for a very long word or unpunctuated text. No assumption that token
    counts are strictly monotonic (SentencePiece can merge a longer prefix).
    """
    parts = []
    remaining = text
    while remaining:
        if len(tokenizer.encode(remaining, out_type=str)) <= budget:
            parts.append(remaining)
            break
        low, high = 1, len(remaining) - 1
        end = 0
        while low <= high:
            middle = (low + high) // 2
            if len(tokenizer.encode(remaining[:middle], out_type=str)) <= budget:
                end = middle
                low = middle + 1
            else:
                high = middle - 1
        if not end:
            raise TranslationError("input_tokens")
        # Avoid tiny fragments: prefer a boundary in the latter half of the
        # verified prefix, then fall back to that prefix. Check chosen boundaries
        # again because punctuation/whitespace can alter tokenization.
        prefix = remaining[:end]
        sentences = [m.end() for m in re.finditer(r"[。！？!?;；]|[.]\s", prefix)]
        words = [m.end() for m in re.finditer(r"\s+", prefix)]
        for boundary in reversed(sentences):
            if boundary >= end // 2 and len(tokenizer.encode(prefix[:boundary], out_type=str)) <= budget:
                end = boundary
                break
        else:
            for boundary in reversed(words):
                if boundary >= end // 2 and len(tokenizer.encode(prefix[:boundary], out_type=str)) <= budget:
                    end = boundary
                    break
        parts.append(remaining[:end])
        remaining = remaining[end:]
        if len(parts) >= MAX_PARTS and remaining:
            raise TranslationError("work_limit")
    return parts


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


def translate(root, direction, text, metrics=None):
    if direction not in ("en-zh", "zh-en"):
        raise TranslationError("direction")
    try:
        valid = text.strip() and len(text.encode("utf-16-le")) <= 32000 and "\x00" not in text
    except UnicodeError:
        valid = False
    if not valid:
        raise TranslationError("input")
    pack = root / direction
    if not all((pack / name).is_file() for name in ("source.spm", "target.spm", "int8/model.bin", "int8/config.json")):
        raise TranslationError("pack_missing")
    try:
        import ctranslate2
        import sentencepiece
    except ImportError:
        raise TranslationError("runtime_missing") from None
    try:
        source = sentencepiece.SentencePieceProcessor(model_file=str(pack / "source.spm"))
        target = sentencepiece.SentencePieceProcessor(model_file=str(pack / "target.spm"))
        engine = ctranslate2.Translator(str(pack / "int8"), device="cpu", compute_type="int8",
                                        inter_threads=1, intra_threads=2)
    except Exception:
        raise TranslationError("pack_load") from None
    calls = 0

    def infer(content):
        nonlocal calls
        if calls >= MAX_PARTS:
            raise TranslationError("work_limit")
        tokens = source.encode(content, out_type=str) + ["</s>"]
        if len(tokens) > MAX_TOKENS:
            raise TranslationError("input_tokens")
        calls += 1
        hypothesis = engine.translate_batch([tokens], beam_size=2, max_input_length=MAX_TOKENS,
                                          max_decoding_length=MAX_TOKENS)[0].hypotheses[0]
        if len(hypothesis) >= MAX_TOKENS:
            # The target can be longer than the source. Retry two smaller source
            # slices rather than publish a silently truncated first translation.
            if len(content) < 2:
                raise TranslationError("output_limit")
            smaller = split_text(content, source, max(1, (len(tokens) - 1) // 2))
            if len(smaller) < 2:
                raise TranslationError("output_limit")
            return join_parts(smaller)
        result = target.decode([t for t in hypothesis if t not in ("</s>", "<pad>")])
        if not result.strip():
            raise TranslationError("empty_output")
        return result

    def join_parts(parts):
        translated = []
        for part in parts:
            leading = part[:len(part) - len(part.lstrip())]
            trailing = part[len(part.rstrip()):]
            if not part.strip():
                translated.append(part)
                continue
            if translated and not translated[-1][-1:].isspace() and not leading and direction == "zh-en":
                translated.append(" ")
            translated.append(leading + infer(part.strip()) + trailing)
        return "".join(translated)

    output = []
    # Only actual line breaks separate paragraphs. splitlines() would also
    # consume other Unicode separators not handled by rstrip("\r\n").
    for line in re.findall(r"[^\r\n]*(?:\r\n|\r|\n|$)", text):
        content = line.rstrip("\r\n")
        ending = line[len(content):]
        if not content.strip():
            output.append(line)
            continue
        output.append(join_parts(split_text(content, source)) + ending)
    if metrics is not None:
        metrics["parts"] = calls
    return "".join(output)


def main():
    if len(sys.argv) != 5:
        return 2
    root, direction, source, destination = sys.argv[1:]
    try:
        source = Path(source)
        if source.stat().st_size > 128000:
            raise TranslationError("input")
        metrics = {}
        output = translate(Path(root), direction, crypt(source.read_bytes(), decrypt=True).decode("utf-8"), metrics)
        reply = {"protocol": 1, "ok": True, "text": output, **metrics}
    except TranslationError as error:
        reply = {"protocol": 1, "ok": False, "error_code": error.code}
    except MemoryError:
        reply = {"protocol": 1, "ok": False, "error_code": "memory"}
    except Exception:
        # Do not log screenshot text, model internals or full local paths.
        reply = {"protocol": 1, "ok": False, "error_code": "engine"}
    encoded = json.dumps(reply, ensure_ascii=True).encode("ascii")
    if len(encoded) > MAX_REPLY_BYTES - 4096:  # Leave room for the encrypted file envelope.
        reply = {"protocol": 1, "ok": False, "error_code": "reply_limit"}
        encoded = json.dumps(reply).encode("ascii")
    with open(destination, "xb") as stream:
        stream.write(crypt(encoded))
    return 0 if reply["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
