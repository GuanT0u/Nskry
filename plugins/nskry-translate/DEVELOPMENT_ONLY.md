# Local experiment — not a public release

This package is for the owner's local MVP evaluation only. It is not in the
installer's bundled directory and is never installed automatically. The optional
language packs are external and must be prepared separately. An online-only
variant omits the complete frozen engine and needs no language packs. Do not redistribute
this development bundle until the native runtime dependency audit, complete
third-party notices and corresponding source/build-material review are complete.

The plugin itself is GPL-3.0-or-later, with the repository's plugin exception.
The offline-enabled variant's frozen engine includes CTranslate2 4.6.0, SentencePiece 0.2.0, CPython 3.12,
NumPy 2.2.6/OpenBLAS, setuptools 80.9.0 and PyInstaller 6.16.0 components, plus
vendor native runtimes collected by their hooks. Project licensing does not
replace their individual licenses. The original CTranslate2 wheel can also
contain CUDA/cuDNN or Intel OpenMP components despite CPU-only inference;
the exact collected dependency set is a release gate, not an assertion that
these are all necessary or already cleared for redistribution.

The model packs retain original cards and revision/hash provenance. En→zh:
Helsinki-NLP/opus-mt-en-zh (Apache-2.0 per original card); zh→en:
Helsinki-NLP/opus-mt-zh-en (CC-BY-4.0 per original card). The developer converter
changes weight storage to CTranslate2 INT8; no claim of original unmodified weights.

0.3.0 adds explicitly selected OpenAI-compatible, LibreTranslate and MyMemory
online adapters. Each task requires a text-upload confirmation; first run is
offline. MyMemory's terms describe retention/sharing and impose quotas. Read
ONLINE_ENGINES.md before enabling it; sensitive text should remain offline.
No user screenshots/content are uploaded during ordinary build/CTest.
