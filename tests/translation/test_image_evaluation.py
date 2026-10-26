import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("image_evaluation", Path(__file__).with_name("evaluate_images.py"))
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)


class EvaluationTests(unittest.TestCase):
    def test_only_whitespace_is_ignored_for_strict_cer(self):
        result = evaluation.source_metrics("Test.\n你好 15", "Test.你好15")
        self.assertEqual(result["cer"], 0)
        self.assertTrue(result["exact_ignoring_whitespace"])
        result = evaluation.source_metrics("Test.\n你好 15", "test你好15")
        self.assertEqual(result["edit_distance"], 2)
        self.assertFalse(result["content_exact_ignoring_punctuation_whitespace"])

    def test_omissions_and_added_junk_cannot_pass(self):
        result = evaluation.source_metrics("打浦路15号", "打浦路1号广告")
        self.assertGreater(result["cer"], 0)
        self.assertFalse(result["content_exact_ignoring_punctuation_whitespace"])
        self.assertEqual(result["expected_digit_groups"], ["15"])
        self.assertEqual(result["actual_digit_groups"], ["1"])

    def test_empty_ocr_is_not_accurate(self):
        result = evaluation.source_metrics("123", "")
        self.assertEqual(result["cer"], 1)
        self.assertFalse(result["exact_ignoring_whitespace"])

    def test_word_and_number_fragmentation_is_not_hidden_by_cer(self):
        result = evaluation.source_metrics("works brown 12", "wo rks b rown 1 2")
        self.assertEqual(result["cer"], 0)
        self.assertGreater(result["latin_digit_wer_casefold"], 0)
        self.assertNotEqual(result["expected_digit_groups"], result["actual_digit_groups"])

    def test_manual_controls_cannot_make_a_failed_image_pass(self):
        good = {"path": "manual_gold_text_control", "ocr_comparison": evaluation.source_metrics("Hello", "Hello")}
        bad = {"path": "image_end_to_end", "ocr_comparison": evaluation.source_metrics("Hello", "Wrong")}
        self.assertFalse(evaluation.image_ocr_gate([{"runs": [good, bad]}]))
        self.assertFalse(evaluation.image_ocr_gate([{"runs": [good]}]))


if __name__ == "__main__":
    unittest.main()
