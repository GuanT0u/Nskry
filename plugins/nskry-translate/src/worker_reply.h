#pragma once
#include <string>
#include <string_view>

namespace nskry::translate {
struct WorkerReply {
    bool success{};
    std::wstring text;
    std::wstring error;
    unsigned parts{};
};
// Private worker protocol, not an SDK extension. Legacy success replies remain
// readable; only known error codes are shown, never raw child exception text.
WorkerReply DecodeWorkerReply(std::wstring_view json, unsigned exitCode);
} // namespace nskry::translate
