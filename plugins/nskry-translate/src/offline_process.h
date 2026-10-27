#pragma once
#include "worker_reply.h"
#include <filesystem>
#include <windows.h>

namespace nskry::translate {
struct TranslationFailure {
    std::wstring message;
};
// Blocking worker-thread operation. Owns its one-shot process/job and encrypted
// temporary files; never calls UI or keeps a translation-window pointer.
WorkerReply RunOffline(const std::filesystem::path &pluginDirectory, HANDLE cancel, const std::wstring &text,
                       const std::wstring &languageRoot, bool toChinese);
} // namespace nskry::translate
