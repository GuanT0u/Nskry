"""Compare offline benchmark output, without correcting OCR text.

Whitespace is ignored for text accuracy; punctuation, case, insertions,
substitutions and deletions are not. Region matches prevent text elsewhere in
the screenshot from masking a local omission. A contains hit is NOT exact OCR.
"""
import argparse
import json
import re
from pathlib import Path


def compact(text):
    return "".join(text.split())


def edit_distance(expected, actual):
    previous = list(range(len(actual) + 1))
    for i, left in enumerate(expected, 1):
        current = [i]
        for j, right in enumerate(actual, 1):
            current.append(min(previous[j] + 1, current[j - 1] + 1,
                               previous[j - 1] + (left != right)))
        previous = current
    return previous[-1]


def region_text(lines, rect):
    selected = []
    for line in lines:
        xs, ys = zip(*line["box"])
        x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
        area = max(0, x1 - x0) * max(0, y1 - y0)
        overlap = max(0, min(x1, rect[2]) - max(x0, rect[0])) * max(0, min(y1, rect[3]) - max(y0, rect[1]))
        if area and overlap >= area * .5:
            selected.append((y0, x0, line["text"]))
    return " ".join(item[2] for item in sorted(selected))


def evaluate(lines, expected=None, regions=None):
    result = {}
    if expected is not None:
        wanted = compact(expected)
        actual = compact("\n".join(line["text"] for line in lines))
        distance = edit_distance(wanted, actual)
        han = lambda value: "".join(char for char in value if "\u3400" <= char <= "\u9fff")
        result.update(edit_distance=distance, expected_characters=len(wanted),
                      cer=distance / len(wanted) if wanted else None, exact=wanted == actual,
                      han_exact=han(wanted) == han(actual))
    if regions:
        result["regions"] = []
        for region in regions:
            wanted = compact(region["text"])
            actual = compact(region_text(lines, region["rect"]))
            result["regions"].append(dict(name=region["name"], expected=wanted, actual=actual,
                                           contains=wanted in actual, exact=wanted == actual,
                                           edit_distance=edit_distance(wanted, actual)))
    return result


def native_requests(text):
    """Read opt-in production trace without treating arbitration notes as text."""
    requests = []
    current = None
    active_pass = None
    for raw in text.splitlines():
        if raw.startswith("========== OCR REQUEST "):
            current = {"run": len(requests) + 1, "lines": [], "passes": {}}
            requests.append(current)
            active_pass = None
        elif current is not None:
            if raw.startswith("========== OCR PASS "):
                active_pass = raw.split()[3]
                current["passes"][active_pass] = []
            elif raw.startswith("========== OCR ARBITRATION"):
                active_pass = None
            elif active_pass and (match := re.match(r"Line \[(\d+),(\d+),(\d+),(\d+)\] (.*)", raw)):
                x0, y0, x1, y1 = map(int, match.groups()[:4])
                current["passes"][active_pass].append({
                    "text": match[5], "box": [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]})
            elif raw.startswith("Final: "):
                current["lines"].append({"text": raw[7:]})
            elif raw.startswith("PassCount: "):
                tokens = raw.split()
                current.update(pass_count=int(tokens[1]), total_ms=float(tokens[3]))
    if not requests or any("pass_count" not in request for request in requests):
        raise ValueError("Missing or incomplete native OCR trace")
    return requests


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--expected", type=Path)
    parser.add_argument("--regions", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--native-trace", action="store_true")
    args = parser.parse_args()
    raw = args.report.read_text(encoding="utf-8")
    runs = native_requests(raw) if args.native_trace else json.loads(raw)["cold_process_runs"]
    expected = args.expected.read_text(encoding="utf-8") if args.expected else None
    regions = json.loads(args.regions.read_text(encoding="utf-8")) if args.regions else None
    output = []
    for run in runs:
        # Old native traces have no final geometry; report per-pass spatial
        # metrics rather than inventing final boxes.
        item = dict(run=run["run"], **evaluate(run["lines"], expected, None if args.native_trace else regions))
        if args.native_trace:
            item.update(pass_count=run["pass_count"], total_ms=run["total_ms"],
                        passes={key: evaluate(lines, expected, regions) for key, lines in run["passes"].items()})
        output.append(item)
    text = json.dumps(output, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text)
