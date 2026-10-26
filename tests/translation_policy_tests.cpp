#include "nskry_text_translation.h"
#include "translation_policy.h"
#include <iostream>
#include <string>
using namespace nskry::translate;
int main() {
    bool ok = ValidateText(L"Hello\n\n你好") == InputError::None;
    ok = !NskryValidText(nullptr, 1, 16000) && !NskryValidText(L"", 0, 16000) && ok;
    ok = !NskryValidText(L"\t \r\n", 4, 16000) && ok;
    const wchar_t surrogate[] = {wchar_t(0xd83d), wchar_t(0xde00)};
    ok = NskryValidText(surrogate, 2, 2) && !NskryValidText(surrogate, 1, 2) &&
         !NskryValidText(surrogate + 1, 1, 2) && !NskryValidText(surrogate, 2, 1) && ok;
    const wchar_t embedded[] = {L'a', 0, L'b'};
    ok = !NskryValidText(embedded, 3, 16000) && ok;
    ok = ValidateText(L"\t \r\n") == InputError::Empty && ok;
    ok = ValidateText(std::wstring(MaxTextUnits, L'a')) == InputError::None && ok;
    ok = ValidateText(std::wstring(MaxTextUnits + 1, L'a')) == InputError::TooLong && ok;
    ok = ValidateText(std::wstring{wchar_t(0xd83d), wchar_t(0xde00)}) == InputError::None && ok;
    ok = ValidateText(std::wstring{wchar_t(0xd83d)}) == InputError::InvalidUnicode && ok;
    ok = ValidateText(std::wstring{wchar_t(0xde00)}) == InputError::InvalidUnicode && ok;
    ok = ValidateText(std::wstring{L'a', 0, L'b'}) == InputError::InvalidUnicode && ok;
    using L = SourceLanguage;
    ok = DetectSourceLanguage(L"Save the screenshot.") == L::English && ok;
    ok = DetectSourceLanguage(L"将截图保存到剪贴板。") == L::Chinese && ok;
    ok = DetectSourceLanguage(L"将 screenshot 复制到 clipboard") == L::Chinese && ok;
    ok = DetectSourceLanguage(L"README.md 文件") == L::Chinese && ok;
    ok = DetectSourceLanguage(L"2026 / 123 !?") == L::Unknown && ok;
    ok = DetectSourceLanguage(L"こんにちは世界") == L::Unsupported && ok;
    ok = DetectSourceLanguage(L"안녕하세요") == L::Unsupported && ok;
    ok = DetectSourceLanguage(L"Привет") == L::Unsupported && ok;
    const wchar_t borrowed[] = {L'H', L'i'}; // Length-delimited, no terminator required.
    NskryTextTranslationV1 request{sizeof(request), 1, nullptr, borrowed, 2};
    ok = NskryValidTranslation(&request) && ok;
    request.version = 2;
    ok = !NskryValidTranslation(&request) && ok;
    request.version = 1;
    request.textLength = 16001;
    ok = !NskryValidTranslation(&request) && ok;
    wchar_t error[4]{};
    NskryTranslationResultV1 reply{sizeof(reply), 1, error, 4};
    NskryTranslationError(&reply, L"Too long");
    ok = std::wstring(error) == L"Too" && ok;
    std::cout << "Translation input limits and Unicode=" << ok << '\n';
    return ok ? 0 : 1;
}
