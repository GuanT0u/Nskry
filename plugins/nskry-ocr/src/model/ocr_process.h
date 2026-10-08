#pragma once
#include "ocr_wire.h"
#include <windows.h>
#include <filesystem>
#include <functional>

namespace nskry::ocr {
enum class ModelStatus { Success, Unavailable, Canceled, Failed };
struct ModelResult {
    ModelStatus status{ModelStatus::Failed};
    wire::Reply reply;
    double elapsedMs{};
    DWORD processId{};
    int threads{};
    bool workerExited{};
};
ModelResult RunModelProcess(HBITMAP bitmap, int width, int height,
    const std::filesystem::path& executable, const std::function<bool()>& canceled,
    DWORD timeoutMs = 20000);
} // namespace nskry::ocr
