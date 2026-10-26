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
    calls = []
    def __init__(self, root, **kwargs):
        assert kwargs == {"device": "cpu", "compute_type": "int8", "inter_threads": 1, "intra_threads": 2}

    def translate_batch(self, batch, **kwargs):
        assert kwargs == {"beam_size": 2, "max_input_length": 256, "max_decoding_length": 256}
        self.calls.append(batch[0])
        content = batch[0][1:-1] if batch[0][0] == ">>cmn_Hans<<" else batch[0][:-1]
        return [types.SimpleNamespace(hypotheses=[content])]


class PolicyTests(unittest.TestCase):
    def setUp(self):
        prefix = patch.object(worker, "source_prefix", side_effect=lambda pack, direction:
                              [">>cmn_Hans<<"] if direction == "en-zh" else [])
        self.addCleanup(prefix.stop)
        self.real_prefix = worker.source_prefix
        prefix.start()

    def test_required_model_marker_is_validated_not_silently_tokenized_as_text(self):
        with patch.object(Path, "stat", return_value=types.SimpleNamespace(st_size=100)):
            with patch.object(Path, "read_text", return_value='["</s>", ">>cmn_Hans<<"]'):
                self.assertEqual(self.real_prefix(Path("unused"), "en-zh"), [">>cmn_Hans<<"])
            with patch.object(Path, "read_text", return_value='["</s>"]'):
                with self.assertRaisesRegex(worker.TranslationError, "pack_load"):
                    self.real_prefix(Path("unused"), "en-zh")
        self.assertEqual(self.real_prefix(Path("unused"), "zh-en"), [])

    def test_sentence_units_are_lossless_without_removing_source_repetition(self):
        for text in ["One sentence. Another! Last?", "你好！再见。下次见？", "3.14 is a number. End.",
                     "U.S. formats work. A. Smith agrees!", 'He said "Go!" Then left.',
                     "line without punctuation", "Same sentence. Same sentence. Same sentence."]:
            with self.subTest(text=text):
                self.assertEqual("".join(worker.sentence_slices(text)), text)
        self.assertEqual(list(worker.sentence_slices("Same sentence. " * 4)), ["Same sentence. "] * 4)
        self.assertEqual(list(worker.sentence_slices("3.14 is a number. End.")),
                         ["3.14 is a number. ", "End."])
        self.assertEqual(list(worker.sentence_slices("U.S. formats work. A. Smith agrees!")),
                         ["U.S. formats work. ", "A. Smith agrees!"])

    def test_sentence_inference_does_not_merge_repeated_source_events(self):
        modules = {"ctranslate2": types.SimpleNamespace(Translator=Engine),
                   "sentencepiece": types.SimpleNamespace(SentencePieceProcessor=Tokenizer)}
        Engine.calls = []
        with patch.dict(sys.modules, modules), patch.object(Path, "is_file", return_value=True):
            text = "A repeated event. " * 4
            self.assertEqual(worker.translate(Path("unused"), "en-zh", text), text)
            self.assertEqual(len(Engine.calls), 4)
            self.assertTrue(all(call[0] == ">>cmn_Hans<<" and "".join(call[1:-1]) == "A repeated event."
                                for call in Engine.calls))

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

    def test_paragraphs_and_bounded_lossless_splitting(self):
        modules = {"ctranslate2": types.SimpleNamespace(Translator=Engine),
                   "sentencepiece": types.SimpleNamespace(SentencePieceProcessor=Tokenizer)}
        Engine.calls = []
        with patch.dict(sys.modules, modules), patch.object(Path, "is_file", return_value=True):
            self.assertEqual(worker.translate(Path("unused"), "en-zh", "hello\r\n\nworld"), "hello\r\n\nworld")
            source = "  Hello, world! " * 100 + "末尾。\r\n\r\n下一段。\n"
            metrics = {}
            self.assertEqual(worker.translate(Path("unused"), "en-zh", source, metrics), source)
            self.assertGreater(metrics["parts"], 1)
            self.assertTrue(all(len(tokens) <= 256 for tokens in Engine.calls))
            self.assertEqual(worker.translate(Path("unused"), "en-zh", "x" * 16000), "x" * 16000)
        for text in ["x" * 256, "中文。" * 300, "😀e\u0301" * 200, "\t a,b. " * 100]:
            parts = worker.split_text(text, Tokenizer())
            self.assertEqual("".join(parts), text)
            self.assertTrue(all(len(part) <= 255 for part in parts))

    def test_output_cap_retries_smaller_pieces_without_partial_publish(self):
        class ExpandingEngine(Engine):
            def translate_batch(self, batch, **kwargs):
                self.calls.append(batch[0])
                return [types.SimpleNamespace(hypotheses=[batch[0][1:-1] * 2])]
        modules = {"ctranslate2": types.SimpleNamespace(Translator=ExpandingEngine),
                   "sentencepiece": types.SimpleNamespace(SentencePieceProcessor=Tokenizer)}
        Engine.calls = []
        with patch.dict(sys.modules, modules), patch.object(Path, "is_file", return_value=True):
            result = worker.translate(Path("unused"), "en-zh", "x" * 240)
            self.assertEqual(result, "x" * 480)
            self.assertGreater(len(Engine.calls), 1)

        class AlwaysCappedEngine(Engine):
            def translate_batch(self, batch, **kwargs):
                return [types.SimpleNamespace(hypotheses=[["x"] * 256])]
        modules["ctranslate2"] = types.SimpleNamespace(Translator=AlwaysCappedEngine)
        with patch.dict(sys.modules, modules), patch.object(Path, "is_file", return_value=True):
            with self.assertRaisesRegex(worker.TranslationError, "output_limit"):
                worker.translate(Path("unused"), "en-zh", "abc")

    def test_pack_missing_and_global_work_limit(self):
        with patch.object(Path, "is_file", return_value=False):
            with self.assertRaisesRegex(worker.TranslationError, "pack_missing"):
                worker.translate(Path("unused"), "zh-en", "你好")
        modules = {"ctranslate2": types.SimpleNamespace(Translator=Engine),
                   "sentencepiece": types.SimpleNamespace(SentencePieceProcessor=Tokenizer)}
        with patch.dict(sys.modules, modules), patch.object(Path, "is_file", return_value=True):
            with self.assertRaisesRegex(worker.TranslationError, "work_limit"):
                worker.translate(Path("unused"), "en-zh", "x\n" * 129)

    def test_adaptive_token_boundaries_are_rechecked(self):
        class VariableTokenizer(Tokenizer):
            def encode(self, text, out_type):
                return list(text) * (2 if text.endswith(" ") else 1)
        source = "Hello. Next word! " * 50
        parts = worker.split_text(source, VariableTokenizer())
        self.assertEqual("".join(parts), source)
        self.assertTrue(all(len(VariableTokenizer().encode(p, str)) <= 255 for p in parts))


if __name__ == "__main__":
    unittest.main()
