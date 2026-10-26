# Explicit OCR / translation hypothesis experiments

These are diagnostic controls, not production fixes or quality-pass gates.
No install, application shutdown, model prewarm or external service is involved.
All models/inputs must already be present locally. Do not use private screenshots
with the trace probe: it intentionally records crops and recognition candidates.

## OCR controls

Explicitly build `NskryOcrHypothesisProbe` (BUILD_TESTING and the native OCR model
build must be enabled). It is EXCLUDE_FROM_ALL, not an ordinary CTest, and is
never packaged. Only this target defines `NSKRY_OCR_HYPOTHESIS_PROBE` to observe
the existing inference function's exact crops/probabilities. Normal workers
compile neither the observer nor any crop/tensor logging.

```text
NskryOcrHypothesisProbe.exe <existing-model-directory> <public-image> <NEW-output-directory>
```

Verify the public input hash against `public_image_review_v1.json` first. The
diagnostic saves the production perspective-corrected crops, six recognizer
preprocessing variants (original, 2x cubic, gray, linear contrast, inverse,
CLAHE), each repeated three times at the same tensor width. A second control
uses each original image's detected axis-aligned bounds, with fixed 0/8px margin.
It does not substitute glyphs or select a production answer from the reference.
All inputs still normalize to the model's 48px height. Doubling a crop before
normalization does not create new source detail or test a 96px-height model.

The tool also renders 64 synthetic numeric cases: two fonts, two sizes, two
color schemes, four extra gaps (0/4/12/24px) and two digit sequences. Expected
characters/groups come from the renderer, not OCR output. Evaluate digit groups
separately from digit-character equality; ignore repeated/trailing whitespace
when judging whether a group separator exists. Zero-gap cases are continuous
numbers, not an expectation that OCR invents invisible group boundaries.

`results.json` retains per-frame blank/space probabilities, top-three classes,
decoded text/confidence and midpoint glyph bounds. CTC blank is NOT a word space.
Do not turn all blank runs into separators, and do not infer a splitting rule
from this one numeric sequence. `NskryOcrSpacingHypothesisTests` is a fast ordinary
CTest characterizing these current mechanisms, not proof that spacing is fixed.

## Translation controls

`hypothesis_texts_v1.json` is a human-authored, pre-run manifest of 12 public
text cases and semantic criteria. It bypasses OCR. Use the prepared developer
converter environment ONLY for the explicit local FP32 conversion:

```text
<converter-python> tests/translation/hypothesis_translation_probe.py <staged-packs> <NEW-fp32-directory> tests/translation/hypothesis_texts_v1.json <new-result.json> --prepare-from <existing-original-model-root>
<runtime-python> tests/translation/hypothesis_translation_probe.py <staged-packs> <fp32-directory> tests/translation/hypothesis_texts_v1.json <NEW-result.json>
```

Run from the repository root, or use absolute paths. Conversion verifies original
provenance, sets offline/telemetry-off flags and converts existing local weights;
it never downloads a model. Source and staged INT8/tokenizer hashes are checked.
Converted FP32 weights stay under the explicitly chosen build directory, not a
user language pack or install folder. Their extra disk space is test data.

The matrix has 2 directions × 2 precisions × beams 1/2/4; each config runs six
texts three times = 216 results. Each configuration is a separate bounded child
process (90-second parent deadline). It uses the CURRENT production worker's
validation, source-language marker, token budget and sentence splitting; a
test-only adapter changes the actual model/compute precision and search width.

One model stays loaded across the 18 cases of a diagnostic child, then exits.
`warm_call_ms` and process peaks are therefore warm experiment measurements, NOT
the plugin's cold end-to-end latency or RAM. Model hashes use streaming reads;
memory peaks are captured before result/provenance serialization, avoiding the
artifact of loading a whole FP32 model file merely to hash it. Do not use the
initial non-streaming experiment's memory numbers for inference comparisons.

Translation needs human semantic review, not exact reference wording. More
search or FP32 may alter wording without fixing an error. Keep numeric roles,
proper names, intended font size and legitimate repeated events separate from
fluency. No output is corrected with these criteria.

Results and limits of the 2026-10-26 run are documented in
`docs/report/devProgress/translation_hypotheses_20261026.md`.

## Subsequent production spacing comparison (0.3.2)

The probe now retains BOTH `raw_text` (CTC only) and `text` (production spatial
recovery), plus `local_uncertainty`. The original 64 render cases are unchanged.
An additional 256 continuous-number/decimal/date/IP/code controls span Segoe UI,
Consolas, Arial and Tahoma, sizes 16/20/28/36 and both color schemes. The extra
fonts/sizes were added after initial rule development; these are limited
controls subsequently used to refine alignment guards, NOT an independent blind
holdout or a broad real-world accuracy claim. Do not reinterpret the original
hypothesis report as having tested this new rule.

```text
python tests/translation/evaluate_spacing.py <NEW-probe-results.json> <NEW-summary.json>
```

This gate rejects damage to previously correct grouping and ANY spacing change
in the continuous-number counterexamples. It does not count unchanged wrong
recognition as correct. The ordinary `NskryOcrSpacingHypothesisTests` now also
checks production pure logic (uncertainty/timing/pixel evidence, explicit spaces,
decimals/IDs, ambiguous gaps and glyph coverage), without loading a model.
