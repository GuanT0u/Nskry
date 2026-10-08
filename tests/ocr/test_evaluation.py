import unittest
from evaluate_results import edit_distance, evaluate, native_requests


def line(text, x=0, y=0):
    return {"text": text, "box": [[x, y], [x+40, y], [x+40, y+20], [x, y+20]]}


class EvaluationTests(unittest.TestCase):
    def test_missing_wrong_and_split_character(self):
        self.assertEqual(edit_distance("需要", "要"), 1)
        self.assertEqual(edit_distance("需要", "霄要"), 1)
        self.assertEqual(edit_distance("文件", "文亻牛"), 2)

    def test_whitespace_only_normalization(self):
        self.assertTrue(evaluate([line("需 要")], "需要")["exact"])
        self.assertFalse(evaluate([line("“需要”")], '"需要"')["exact"])
        self.assertTrue(evaluate([line("“需要”")], '"需要"')["han_exact"])
        self.assertFalse(evaluate([line("要")], "需要")["han_exact"])

    def test_same_text_elsewhere_cannot_fill_missing_region(self):
        value = evaluate([line("此电脑", 200, 200)], regions=[
            {"name": "path", "text": "此电脑", "rect": [0, 0, 60, 25]}])
        self.assertFalse(value["regions"][0]["contains"])

    def test_icon_false_positive_is_not_exact_success(self):
        value = evaluate([line("三查看")], regions=[
            {"name": "view", "text": "查看", "rect": [0, 0, 60, 25]}])
        self.assertTrue(value["regions"][0]["contains"])
        self.assertFalse(value["regions"][0]["exact"])
        self.assertEqual(value["regions"][0]["edit_distance"], 1)

    def test_empty_result(self):
        self.assertEqual(evaluate([], "文字")["cer"], 1)

    def test_native_trace_ignores_arbitration_notes(self):
        trace = "\n".join([
            "========== OCR REQUEST 1 ==========", "========== OCR PASS A ==========",
            "Line [0,0,40,20] 文字", "  Word [0,0,20,20] 文",
            "========== OCR ARBITRATION ==========", "  A: 文字", "Final: 文字",
            "PassCount: 1 TotalMs: 10.5"])
        run = native_requests(trace)[0]
        self.assertEqual(run["lines"], [{"text": "文字"}])
        self.assertEqual(len(run["passes"]["A"]), 1)
        self.assertEqual(run["total_ms"], 10.5)
        with self.assertRaises(ValueError):
            native_requests(trace.rsplit("\n", 1)[0])


if __name__ == "__main__":
    unittest.main()
