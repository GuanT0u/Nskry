"""Explicit diagnostic scoring, NOT an assertion of universal OCR accuracy."""
import argparse
import json
from collections import defaultdict
from pathlib import Path


def groups(text):
    return " ".join(text.split())


def evaluate(data):
    summary = defaultdict(lambda: {"count": 0, "raw_groups_correct": 0, "final_groups_correct": 0})
    regressions = []
    for row in data["synthetic"]:
        item = summary[str(int(row["gap_px"]))]
        item["count"] += 1
        before = groups(row["trace"]["raw_text"]) == row["expected"]
        after = groups(row["trace"]["text"]) == row["expected"]
        item["raw_groups_correct"] += before
        item["final_groups_correct"] += after
        if before and not after:
            regressions.append(row["id"])
    changed = [row for row in data["counterexamples"] if row["trace"]["raw_text"] != row["trace"]["text"]]
    return {"synthetic": dict(summary), "synthetic_regressions": regressions,
            "counterexample_count": len(data["counterexamples"]),
            "changed_counterexamples": [{k: v for k, v in row.items() if k != "trace"} |
                                       {"raw": row["trace"]["raw_text"], "final": row["trace"]["text"]}
                                       for row in changed],
            "full_pipeline": [{k: row[k] for k in ("raw_text", "text", "local_uncertainty")}
                              for row in data["full_pipeline"]],
            "nonregression_pass": not regressions and not changed,
            "quality_scope": "DEVELOPMENT_AND_ADDITIONAL_FONT_CONTROLS_NOT_UNIVERSAL_ACCURACY"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("result", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = evaluate(json.loads(args.result.read_text(encoding="utf-8")))
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    print("Spacing controls nonregression:", result["nonregression_pass"])
    raise SystemExit(0 if result["nonregression_pass"] else 1)
