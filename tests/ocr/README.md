# OCR accuracy / resource probes

These are opt-in development tools, not runtime dependencies of Nskry. They do
not install anything into the user's plugin registry. Native-model tests exercise
the production on-demand worker; Python probes remain optional comparison tools.
Fixtures contain user-provided screenshots. Outputs stay on the local machine.

## Production native worker

Prepare dependencies with `python tools/prepare_ocr_dependencies.py`, then build
the project. CTest includes wire/CTC, process failure/cancellation, fixture and UI
workflow tests. The UI test touches only its own process windows and substitutes
clipboard/editor callbacks; it does not change your clipboard.

```powershell
build/verify/NskryOcrModelTests.exe build/verify/plugins/nskry-ocr/package/engine/nskry-ocr-worker.exe tests/ocr/fixtures/explorer_dark_zh.png --regions tests/ocr/fixtures/explorer_dark_zh.regions.json 10 build/verify/explorer-native.json
build/verify/NskryOcrModelTests.exe build/verify/plugins/nskry-ocr/package/engine/nskry-ocr-worker.exe tests/ocr/fixtures/paragraph_dark_zh.png --content tests/ocr/fixtures/paragraph_dark_zh.expected.txt 10 build/verify/paragraph-native.json
```

Every run records line/glyph geometry, confidence, load/inference/end-to-end timing,
CPU time, peak working set/commit, thread count and worker exit. `model_pass_count=1`
means one detection/recognition pipeline, **not** one neural-network invocation.
The fixture gates check spatial substrings and Han/letter/digit content respectively;
use `evaluate_results.py` for exact-region and punctuation-sensitive CER.

## Windows OCR preprocessing matrix

Build `NskryOcrNativeProbe` in a configured Windows build. Run:

```powershell
build/verify/NskryOcrNativeProbe.exe tests/ocr/fixtures/explorer_dark_zh.png 330 60 90 40
```

The optional four integers are x/y/width/height in original pixels. Each scale,
padding and treatment combination is an independent diagnostic call, NOT one
production pass. Use a crop first to avoid a large full-image matrix.

## Optional offline model comparison

The tested external Python environment already had `rapidocr-onnxruntime==1.4.4`
and `onnxruntime==1.29.0` with local PP-OCRv4 ONNX models. No automatic downloads
or package installation are performed by the script. Other adapter versions
must be revalidated (the benchmark configures its session-options factory).

```powershell
python tests/ocr/benchmark_rapidocr.py tests/ocr/fixtures/explorer_dark_zh.png --threads 4 --arena --runs 10 --output build/verify/explorer-model.json
python tests/ocr/benchmark_rapidocr.py tests/ocr/fixtures/paragraph_dark_zh.png --threads 2 --det-size 320 --arena --runs 10 --output build/verify/paragraph-model.json
python tests/ocr/evaluate_results.py build/verify/explorer-model.json --regions tests/ocr/fixtures/explorer_dark_zh.regions.json
python tests/ocr/evaluate_results.py build/verify/paragraph-model.json --expected tests/ocr/fixtures/paragraph_dark_zh.expected.txt
python -m unittest discover -s tests/ocr -p test_evaluation.py
```

Each request starts a new process and waits for exit. This includes interpreter
startup, imports and model load; it is not a cold-OS-cache test and excludes
production UI/IPC. Run performance probes sequentially, without concurrent
builds/tests. The report records peak working set AND peak process commit;
neither should be confused with the installed model file size.

Use `--native-trace` with `evaluate_results.py` to read the production trace
enabled by `NSKRY_OCR_TRACE_FILE`. Old traces lack final bounding boxes, so spatial
checks apply to raw A/B/C only. Full-text comparison remains available for final
output. `contains`, `exact`, full CER and Han-only equality are deliberately
separate metrics; do not count substring hits or correct text elsewhere as a
fully correct local result. Expected strings are test data, never OCR fixes.
