#pragma once
#include "nskry_ocr_layout.h"
#include <memory>
#include <string>

namespace nskry::ocr_client {
struct Lease {
    NskryOcrLeaseV1 api{};
    ~Lease() { if (api.module) FreeLibrary(api.module); }
    Lease() = default;
    Lease(const Lease&) = delete;
};
inline std::shared_ptr<Lease> Acquire(NskryAcquireOcrV1 acquire) {
    auto lease = std::make_shared<Lease>();
    return acquire && acquire(nullptr, &lease->api) == NSKRY_OCR_OK && lease->api.recognize ? lease : nullptr;
}
inline const wchar_t *Message(int status) {
    switch (status) {
    case NSKRY_OCR_EMPTY: return L"No text found in this image.";
    case NSKRY_OCR_LANGUAGE: return L"Install a Windows OCR language pack or select Accurate.";
    case NSKRY_OCR_TIMEOUT: return L"OCR timed out. Try a smaller selection.";
    case NSKRY_OCR_BUSY: return L"OCR is busy. Retry after the active request.";
    case NSKRY_OCR_UNAVAILABLE: return L"Install or enable a compatible OCR provider in Settings.";
    case NSKRY_OCR_CANCELED: return L"OCR canceled.";
    case NSKRY_OCR_INVALID: return L"Invalid OCR input.";
    default: return L"Image recognition failed.";
    }
}
inline bool Valid(const NskryOcrLayoutV1 *out) {
    if (!out || !out->text || !out->length || out->length > 1048576 ||
        (out->wordCount && !out->words) || (out->lineCount && !out->lines) ||
        out->wordCount > out->length || out->lineCount > out->length) return false;
    for (uint32_t i = 0; i < out->wordCount; ++i) {
        const auto &w = out->words[i];
        if (!w.length || w.offset > out->length || w.length > out->length - w.offset ||
            w.line >= out->lineCount) return false;
    }
    return true;
}
}
