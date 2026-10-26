"""Assess explicit public-image audit logs, never treat nonempty translation as quality.

Only OCR edit distance/content are automatic acceptance facts. Translation wording
requires human review against meaning_units, not string equality or a word list.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location(
    "ocr_evaluation", Path(__file__).resolve().parents[1] / "ocr/evaluate_results.py")
ocr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ocr)


def source_metrics(expected, actual):
    wanted, found = ocr.compact(expected), ocr.compact(actual)
    distance = ocr.edit_distance(wanted, found)
    content = lambda text: "".join(c for c in text if c.isalnum())
    words = re.findall(r"[A-Za-z]+|\d+", expected)
    found_words = re.findall(r"[A-Za-z]+|\d+", actual)
    word_distance = ocr.edit_distance([word.lower() for word in words],
                                     [word.lower() for word in found_words]) if words else None
    return {"expected_nonspace_characters": len(wanted), "edit_distance": distance,
            "cer": distance / len(wanted) if wanted else None,
            "exact_ignoring_whitespace": wanted == found,
            "content_exact_ignoring_punctuation_whitespace": content(wanted) == content(found),
            "latin_digit_word_count": len(words),
            "latin_digit_wer_casefold": word_distance / len(words) if words else None,
            "expected_digit_groups": re.findall(r"\d+", expected),
            "actual_digit_groups": re.findall(r"\d+", actual)}


def evaluate(review_path, reports):
    reference = json.loads(review_path.read_text(encoding="utf-8"))
    review_hash = hashlib.sha256(review_path.read_bytes()).hexdigest()
    fixtures = {fixture["id"]: fixture for fixture in reference["fixtures"]}
    output = []
    for report_path in reports:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if report["review_sha256"] != review_hash:
            raise ValueError("Baseline bytes changed since the audit; do not silently regrade")
        rows = []
        for run in report["runs"]:
            fixture = fixtures[run["fixture"]]
            rows.append({**run, "ocr_comparison": source_metrics(fixture["expected_source"], run["source"]),
                         "reference_translation": fixture["reference_translation"],
                         "manual_meaning_units": fixture["meaning_units"],
                         "translation_quality": "REQUIRES_HUMAN_REVIEW_NOT_NONEMPTY_CHECK"})
        output.append({"report": str(report_path), "dll_sha256": report["dll_sha256"],
                       "engine": report["engine"], "runs": rows})
    return output


def image_ocr_gate(evaluations):
    image_rows = [row for report in evaluations for row in report["runs"]
                  if row["path"] == "image_end_to_end"]
    return bool(image_rows) and all(
        row["ocr_comparison"]["exact_ignoring_whitespace"]
        and row["ocr_comparison"]["expected_digit_groups"] == row["ocr_comparison"]["actual_digit_groups"]
        and row["ocr_comparison"]["latin_digit_wer_casefold"] in (None, 0)
        for row in image_rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("review", type=Path)
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-exact-ocr", action="store_true",
                        help="Fail on actual-image OCR mismatch, never grade manual text controls as OCR")
    args = parser.parse_args()
    evaluations = evaluate(args.review, args.reports)
    text = json.dumps(evaluations, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text)
    if args.require_exact_ocr and not image_ocr_gate(evaluations):
        raise SystemExit("Actual-image OCR quality gate FAILED; completed workflows are not quality passes.")
