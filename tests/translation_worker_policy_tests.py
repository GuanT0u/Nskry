"""No model, download or third-party inference dependency; Windows DPAPI only."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("offline_worker", Path(__file__).resolve().parents[1] /
    "plugins/nskry-translate/src/offline_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class Tokenizer:
    def __init__(self, **kwargs):
        pass

    def encode(self, text, out_type):
        return list(text)

    def decode(self, tokens):
        return "".join(tokens)


class Engine:
    def __init__(self, root, **kwargs):
        assert kwargs == {"device": "cpu", "compute_type": "int8", "inter_threads": 1, "intra_threads": 2}

    def translate_batch(self, batch, **kwargs):
        assert kwargs == {"beam_size": 2, "max_input_length": 256, "max_decoding_length": 256}
        return [types.SimpleNamespace(hypotheses=[batch[0]])]


class PolicyTests(unittest.TestCase):
    def test_dpapi_roundtrip_and_corruption(self):
        raw = "private synthetic fixture: 中文".encode("utf-8")
        encrypted = worker.crypt(raw)
        self.assertNotIn(raw, encrypted)
        self.assertEqual(worker.crypt(encrypted, decrypt=True), raw)
        with self.assertRaises(OSError):
            worker.crypt(b"not a DPAPI payload", decrypt=True)

    def test_invalid_input_rejected_before_loading_model(self):
        for direction, text in [("bad", "Hello"), ("en-zh", " "), ("en-zh", "a\0b"),
                                ("en-zh", "x" * 16001), ("en-zh", "\ud800")]:
            with self.subTest(direction=direction, length=len(text)), self.assertRaises(ValueError):
                worker.translate(Path("unused"), direction, text)

    def test_paragraphs_and_explicit_token_limits(self):
        modules = {"ctranslate2": types.SimpleNamespace(Translator=Engine),
                   "sentencepiece": types.SimpleNamespace(SentencePieceProcessor=Tokenizer)}
        with patch.dict(sys.modules, modules):
            self.assertEqual(worker.translate(Path("unused"), "en-zh", "hello\r\n\nworld"), "hello\r\n\nworld")
            with self.assertRaisesRegex(ValueError, "paragraph exceeds"):
                worker.translate(Path("unused"), "zh-en", "x" * 256)
            # The input fits, but output at the cap cannot be silently published.
            with self.assertRaisesRegex(ValueError, "output limit"):
                worker.translate(Path("unused"), "zh-en", "x" * 255)


if __name__ == "__main__":
    unittest.main()
