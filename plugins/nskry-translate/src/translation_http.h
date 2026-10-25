#pragma once
#include "online_translation.h"

namespace nskry::translate {
// Worker-thread only. No retries, redirects, cookies, implicit credentials,
// certificate bypass, persistent session or content/error-body logging.
std::wstring FetchTranslation(const OnlineRequest &request, HANDLE cancel, unsigned timeoutMs);
} // namespace nskry::translate
