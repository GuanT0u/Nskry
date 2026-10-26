# Explicit packaged-image translation audit

This is an opt-in development probe, not a runtime dependency, installed plugin
or external-network CTest. It loads the exact DLL from an explicitly extracted
package, invokes its real capture/text exports, and inspects its own result UI.
No production OCR/translation behavior is patched or replaced.

## Baseline and public images

`public_image_review_v1.json` was written by visually reading two images **before
running Nskry on them**. It records original URLs, hashes, dimensions, source
transcriptions and semantic translation requirements. Images stay in an ignored
build directory, not in the public package. No Tesseract/PaddleOCR engine is
installed or used: their repositories supply only public sample images.

- English: `tesseract-ocr/test/testing/phototest.tif` from its public repository.
  Decode to lossless PNG with Windows System.Drawing; no rescale/crop/text edits.
- Chinese: `PaddlePaddle/PaddleOCR`, branch `release/2.7`, `doc/imgs/12.jpg`.
  Use the original JPEG: four readable rows on an outdoor direction sign.

The exact URLs/hashes are in the review manifest; downloads are manual and
explicit. Normal configure/build/CTest never downloads these images. Do not use
user screenshots or private fixtures with the public-network opt-in.

## Run

Build `NskryTranslationImageAudit` in a configured Windows test build. Extract
each desired `.nskryplugin` into a NEW independent build directory; do not install
it into Nskry's registry. Example arguments (all paths should be absolute):

```text
NskryTranslationImageAudit.exe <packaged-DLL> <public-images-dir> <review.json> <external-language-root-or-dash> offline|mymemory <1..3> <NEW-result.json> [--allow-public-network] [--gold-soft-wrap-control]
```

The normal audit does N image captures and one **separate** manual-gold text
control per image. The gold control is not image recognition and never counts as
OCR accuracy. A diagnostic `--gold-soft-wrap-control` replaces only English gold
text's physical line breaks with spaces; it changes neither the images nor the
frozen reference and is labeled separately in output. Do not retroactively use
this control to claim the actual capture pipeline passed.

The MyMemory path requires explicit `--allow-public-network` AND matching image
hashes. Its test-only handler approves only the current process's own upload
dialog for these reviewed public fixtures. A detected quota failure stops the
remaining matrix; there is no automatic retry. Do not add this live audit to
ordinary CTest or change it to upload arbitrary screenshots silently.

Each process redirects HKCU to a newly created owned test key, never reads user
API keys/preferences, and removes its own key at exit. Models remain external;
no automatic download, install, user-process termination or DLL replacement.
Sampling is test-only: 20 ms memory samples and 100 ms owned-worker discovery,
with parent and child CPU/commit/physical-memory figures kept separate. The UI
is pumped while tasks run. Observed child handles must indicate exit afterward.
These observations are not a low-end performance guarantee or a whole-system
memory measurement; OS/library caches and observer overhead are not subtracted.

For 0.3.1+, the sampler separately tracks owned `nskry-ocr-worker.exe` processes
and offline translation workers. It also records sampled simultaneous parent +
live-child memory and peak concurrent child count; do not add independent peaks
from successive recognition/translation stages. Previous 0.3.0 logs retain their
original fields and frozen standard and are never overwritten.

## Evaluate

```text
python tests/translation/evaluate_images.py <review.json> <result1.json> <result2.json> --output <evaluation.json> --require-exact-ocr
python tests/translation/test_image_evaluation.py
```

Source metrics include non-whitespace CER (case/punctuation sensitive),
Han/Latin/digit content equality, digit groups, and case-folded Latin/digit WER.
Whitespace-insensitive CER alone can hide broken words/numbers such as
`works`→`wo rks` or `12`→`1 2`; word/digit diagnostics retain these differences.

`--require-exact-ocr` exits nonzero when actual-image text differs, including when
all UI tasks completed. Gold controls cannot make a failed image pass. Without
that flag the evaluator is a reporting tool, not a quality pass assertion.
Translation quality needs human review against every meaning unit: subject/object,
all sentences/repetitions, entities and numbers. Do not use nonempty text, a
reference-string match, or keyword hits as a translation-quality oracle.

The initial audit's findings and scope are recorded in
`docs/report/devProgress/translation_public_image_audit_20261025.md`.
The unchanged-baseline 0.3.1 comparison, remaining failures and new package hashes
are in `docs/report/devProgress/translation_0_3_1_quality_progress.md`.

Separate root-cause controls (fixed crops, synthetic number spacing and correct
text INT8/FP32/beam comparisons) are described in `HYPOTHESIS_TESTS.md`.
