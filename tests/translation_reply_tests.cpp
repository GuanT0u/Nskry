#include "worker_reply.h"
#include <iostream>
using nskry::translate::DecodeWorkerReply;
int main() {
    bool ok = DecodeWorkerReply(LR"({"protocol":1,"ok":true,"text":"你好","parts":3})", 0).success;
    ok = DecodeWorkerReply(LR"({"ok":true,"text":"Legacy engine"})", 0).success && ok;
    const auto missing = DecodeWorkerReply(LR"({"protocol":1,"ok":false,"error_code":"pack_missing"})", 1);
    ok = !missing.success && missing.error.find(L"Language pack missing") != std::wstring::npos && ok;
    const auto unknown =
        DecodeWorkerReply(LR"({"ok":false,"error":"PRIVATE SOURCE","error_code":"PRIVATE KEY"})", 1);
    ok = !unknown.success && unknown.error.find(L"PRIVATE") == std::wstring::npos && ok;
    for (const auto *invalid :
         {L"broken", LR"({"ok":true,"text":""})", LR"({"ok":true,"text":"\u0000"})",
          LR"({"ok":true,"text":"\ud800"})", LR"({"protocol":2,"ok":true,"text":"x"})",
          LR"({"protocol":1,"ok":true,"text":"x","parts":129})",
          LR"({"protocol":1,"ok":true,"text":"x","parts":0})", LR"({"protocol":1,"ok":true,"text":"x"})"})
        ok = !DecodeWorkerReply(invalid, 0).success && ok;
    ok = !DecodeWorkerReply(LR"({"ok":true,"text":"partial"})", 1).success && ok;
    ok = DecodeWorkerReply(LR"({"ok":false,"error_code":"pack_missing"})", 0).error != missing.error && ok;
    std::cout << "Worker reply compatibility, failure codes and fail-closed bounds=" << ok << '\n';
    return ok ? 0 : 1;
}
