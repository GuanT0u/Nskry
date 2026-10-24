# Translation 0.2.1 — experimental offline translation

Independent API2 plugin. Select a screenshot, a pinned image, a monitor's static
frame, or an editor subregion and choose Translation. Windows OCR runs locally,
then translation starts automatically. Auto chooses English→Chinese or Chinese→English
using a bounded script-based rule; mixed text uses the dominant supported script.
It is not a universal language classifier (other Latin-alphabet languages can be
mistaken for English). Unknown/numeric-only or clearly unsupported scripts need
manual direction/source review. You can edit the source and translate again, or
override Auto with either direction. No extra detector model is loaded.
Copy result copies all translated text; normal text selection also works.
Settings opens the same window without OCR, model loading or networking.

0.2.1 fixes direct capture OCR lasting longer than the initial wait slice:
repeated WinRT wait_for calls registered multiple completion handlers and raised
0x80000018. The worker now queries status with a cancelable timed wait instead;
no completion callback can outlive the plugin. OCR-stage and translation-stage
errors are reported separately. Existing runtime/language packs are unchanged.

With the matching Core, OCR 0.6.3+ has a Translate button below its text pane.
It forwards the existing OCR text through the host's optional text translation v1
extension, immediately starts translation, and never recognizes the image again.
OCR/Translation remain independently usable; a missing/disabled/incompatible
provider disables the OCR entry, and a busy task is not silently overwritten.
Closing OCR does not close a handed-off translation window. Update Core + both
plugins for this integration; the external language packs do not need conversion again.

The source bitmap is copied before execute returns. OCR does not depend on the
OCR plugin. Missing Windows language resources may reduce recognition quality;
manual source entry remains available. This MVP does not reuse the heavier OCR
plugin's model or private UI.

## Development setup (explicit, not part of normal build)

1. Prepare `build/translate-dev` with CPU torch 2.9.0, transformers 4.49.0,
   ctranslate2 4.6.0, sentencepiece 0.2.0 and setuptools 80.9.0.
2. Run `tools/offline_probe.py build/translate-models --prepare` with that venv.
   It downloads only public original models, records revisions/hashes and converts
   them to INT8. It does not upload screenshot text.
3. In a separate `build/translate-runtime` venv install ctranslate2 4.6.0,
   sentencepiece 0.2.0, numpy 2.2.6, setuptools 80.9.0 and pyinstaller 6.16.0.
   Do NOT install torch/transformers there.
4. Run `tools/freeze_worker.ps1 -RuntimePython <venv-python> -OutputRoot <build-folder>`
   to freeze `src/offline_worker.py` with PyInstaller onedir, collect ctranslate2
   binaries/data and exclude torch/transformers/tensorflow. Put the complete
   resulting directory under `package/engine/`, not just its EXE.
5. Run `tools/stage_language_packs.py build/translate-models build/translate-languages`.
   Set that absolute path in the plugin window; saving it stores only ModelRoot
   under HKCU/Software/Nskry/Plugins/nskry-translate.
6. For the real-engine regression configure
   `NSKRY_TRANSLATE_TEST_MODELS` to the staged language directory. Ordinary CMake
   builds never download models or freeze Python automatically.
   The built `NskryTranslationPluginTests.exe <DLL> <language-root> [local-image.png]`
   also accepts an optional screenshot fixture for three direct-translation runs;
   it logs only source/result lengths and elapsed times for that image.
7. Optional local-only package: run `tools/package_development.py
   <build-plugin-stage> <new-build-path>.nskryplugin`. Language packs stay external;
   use `DEVELOPMENT_ONLY.md` release restrictions. This does not install anything.

No system Python is needed by the frozen worker. The current developer runtime
is not a finalized redistributable: vendor runtime notice collection and a
CPU-only binary dependency audit remain release gates. No automatic installer
component or public bundled package is enabled before these gates pass.

## Resource/privacy limits

No worker/model/polling at init. One active task, 2 inference threads, CPU INT8.
Measured cold private peak is about 650 MiB; job hard cap 1 GiB, deadline 60 seconds.
File size is not RAM usage. Four short sentences took about 1.1 seconds including
fresh-process startup on the development machine, not a low-end guarantee.
Per paragraph maximum 256 source tokens/output tokens; the UI rejects excessive
input rather than silently truncating. Close/Cancel interrupts the owned job.

The worker is atomically assigned to a kill-on-close job before it runs. Input
and reply use unique per-task files in LocalAppData/Nskry/cache/translation,
protected with Windows current-user DPAPI and deleted on normal
success/failure/cancel. A host crash can leave encrypted files; there is no
startup scan or automatic deletion of historical/user files. DPAPI is not a
sandbox against other code running as the same user. Only public synthetic
fixtures were used for development measurements.

## Model quality and licensing

OPUS-MT en→zh and zh→en INT8 weights are about 77.2 MiB each, plus tokenizers.
Models are experimental: e.g. screen recording may be translated as audio
recording; no dictionary/hardcoded output repair is applied. Source text remains
editable. Each pack retains its source model card, pinned revision and hashes.
Check individual model licenses (en→zh Apache-2.0, zh→en CC-BY-4.0 as recorded
by their original model cards), attribution and quantization-change notices
before redistribution. CTranslate2, SentencePiece, Python, NumPy/OpenBLAS,
setuptools, frozen bootloader and bundled native runtimes also need their own
complete notices/source obligations; project GPL does not replace those.

## Future providers (not implemented in 0.2.0)

Keep distinct provider categories: user-configured online (BYOK or another user
credential scheme), approved free online service, lightweight offline, and
user-configured larger offline engine. No default public/free endpoint has been
chosen. This MVP has no networking or API key storage, and never silently falls
back to an online service. Future online adapters must require explicit consent,
validate HTTPS and redirects, protect credentials, bound requests and support
cancellation. They must not turn screenshot OCR into continuous uploads.
