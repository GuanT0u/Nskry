# Native offline OCR components

This plugin uses PP-OCRv4 DB detection and CTC recognition. Screenshots are
processed locally; this package does not contain a Python runtime.

- ONNX Runtime 1.29.0, Microsoft and contributors: MIT plus the bundled
  `licenses/onnxruntime-ThirdPartyNotices.txt`. Runtime archive SHA256:
  `c9b4b7086b529ad814f428c1bad028e20a25d7dc0699836775faace4ab5b78b2`.
  Source: https://github.com/microsoft/onnxruntime/tree/v1.29.0
- OpenCV 4.10.0 core/imgproc, OpenCV contributors: Apache-2.0. Statically linked
  with IPP, ITT, OpenCL, TBB, video, UI and image codecs disabled. Zlib and
  SoftFloat notices/source are also retained in `licenses/`.
  Source: https://github.com/opencv/opencv/tree/4.10.0
- Clipper 6.4.2, Copyright Angus Johnson 2010–2017: Boost Software License 1.0.
  Source files obtained unchanged from the pinned pyclipper 1.3.0.post6 source
  distribution; `licenses/clipper.hpp` retains its original notices. Pyclipper
  wrapper code is not linked; its MIT license is retained as source provenance.
  Source: https://pypi.org/project/pyclipper/1.3.0.post6/
- Detection/recognition geometry follows PaddleOCR/RapidOCR algorithms under
  Apache-2.0. Source: https://github.com/RapidAI/RapidOCR/tree/v1.4.4
  and https://github.com/PaddlePaddle/PaddleOCR . The runtime code in this plugin
  is a native implementation, not the RapidOCR Python package.

The local development package contains the PP-OCRv4 Chinese/English ONNX models
from the official `rapidocr-onnxruntime` 1.4.4 wheel (SHA256
`971d7d5f223a7a808662229df1ef69893809d8457d834e6373d3854bc1782cbf`):

- `ch_PP-OCRv4_det_infer.onnx`, SHA256
  `d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9`
- `ch_PP-OCRv4_rec_infer.onnx`, SHA256
  `48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b`

Model copyright belongs to Baidu and the applicable PaddleOCR rights holders.
RapidOCR converted these upstream PP-OCRv4 models to ONNX. Nskry ships the
ONNX files byte-for-byte from the pinned wheel. Both hashes above also appear
in the v3.10.0 RapidOCR model registry as `ch_PP-OCRv4_det_mobile` and
`ch_PP-OCRv4_rec_mobile` under ONNX Runtime. RapidOCR's current model license
notice identifies every model in that registry as Apache-2.0. The full license
text is included in `licenses/RapidOCR-LICENSE.txt`.

- Registry and exact hashes: https://github.com/RapidAI/RapidOCR/blob/v3.10.0/python/rapidocr/default_models.yaml
- Model license and upstream attribution: https://github.com/RapidAI/RapidOCR/blob/main/python/MODEL_LICENSES.md
