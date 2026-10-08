#pragma once
#include "ocr_wire.h"
#include <filesystem>
#include <span>

namespace nskry::ocr {
wire::Reply RecognizeModel(std::span<const uint8_t> bgra, int width, int height,
                           const std::filesystem::path& models, int threads);
}
