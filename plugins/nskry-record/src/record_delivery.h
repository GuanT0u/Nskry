#pragma once
#include "record_options.h"
#include <string>
#include <windows.h>
namespace nskry::record {
Options LoadDefaults();
bool SaveDefaults(Options options);
std::wstring NewRecordingPath(bool gif);
void PrepareRecordingCache(const std::wstring &output, HANDLE canceled = nullptr); // Worker only, no idle janitor.
bool CopyRecordingFile(HWND owner, const std::wstring &path);
} // namespace nskry::record
