#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace nskry::translate {
// Task-owned pixels, cancellation and private package assets. Never resolves an
// installed OCR plugin, borrows its DLL, or keeps a model alive between tasks.
std::wstring RecognizeImage(const std::vector<unsigned char> &pixels, int width, int height,
                            const std::filesystem::path &directory, HANDLE cancel);
} // namespace nskry::translate
