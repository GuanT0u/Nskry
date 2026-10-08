# Development inputs. Only the one-shot worker links the inference runtime.
set(NSKRY_OCR_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/ocr-deps" CACHE PATH "Native OCR dependency cache")
set(NSKRY_ORT_DIR "${NSKRY_OCR_DEPS_DIR}/onnxruntime-win-x64-1.29.0")
set(NSKRY_CLIPPER_DIR "${NSKRY_OCR_DEPS_DIR}/pyclipper-1.3.0.post6")
set(NSKRY_OCR_MODELS_DIR "${NSKRY_OCR_DEPS_DIR}/rapidocr_onnxruntime/models")
if (NOT EXISTS "${NSKRY_ORT_DIR}/include/onnxruntime_cxx_api.h" OR
    NOT EXISTS "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/CMakeLists.txt" OR
    NOT EXISTS "${NSKRY_OCR_MODELS_DIR}/ch_PP-OCRv4_rec_infer.onnx")
    message(FATAL_ERROR "Run: python tools/prepare_ocr_dependencies.py (or -DNSKRY_OCR_MODEL_ENGINE=OFF for Windows-only OCR).")
endif()
set(BUILD_LIST "core,imgproc" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_STATIC_CRT OFF CACHE BOOL "" FORCE)
foreach(flag BUILD_TESTS BUILD_PERF_TESTS BUILD_EXAMPLES BUILD_opencv_apps
    BUILD_opencv_python2 BUILD_opencv_python3 BUILD_JAVA WITH_IPP WITH_ITT
    WITH_OPENCL WITH_TBB WITH_EIGEN WITH_LAPACK WITH_PROTOBUF WITH_WIN32UI
    WITH_MSMF WITH_FFMPEG WITH_PTHREADS_PF WITH_JPEG WITH_PNG WITH_TIFF WITH_WEBP
    WITH_OPENEXR WITH_OPENJPEG WITH_JASPER WITH_QUIRC)
    set(${flag} OFF CACHE BOOL "" FORCE)
endforeach()
set(CPU_BASELINE "SSE2" CACHE STRING "" FORCE)
set(CPU_DISPATCH "SSE4_1;AVX2" CACHE STRING "" FORCE)
add_subdirectory("${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0" "${CMAKE_CURRENT_BINARY_DIR}/opencv" EXCLUDE_FROM_ALL)
add_library(nskry_ort SHARED IMPORTED)
set_target_properties(nskry_ort PROPERTIES
    IMPORTED_IMPLIB "${NSKRY_ORT_DIR}/lib/onnxruntime.lib"
    IMPORTED_LOCATION "${NSKRY_ORT_DIR}/lib/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${NSKRY_ORT_DIR}/include")
