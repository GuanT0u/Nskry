#include "translation_policy.h"
#include <iostream>
#include <string>
using namespace nskry::translate;
int main() {
    bool ok = ValidateText(L"Hello\n\n你好") == InputError::None;
    ok = ValidateText(L"\t \r\n") == InputError::Empty && ok;
    ok = ValidateText(std::wstring(MaxTextUnits, L'a')) == InputError::None && ok;
    ok = ValidateText(std::wstring(MaxTextUnits + 1, L'a')) == InputError::TooLong && ok;
    ok = ValidateText(std::wstring{wchar_t(0xd83d), wchar_t(0xde00)}) == InputError::None && ok;
    ok = ValidateText(std::wstring{wchar_t(0xd83d)}) == InputError::InvalidUnicode && ok;
    ok = ValidateText(std::wstring{wchar_t(0xde00)}) == InputError::InvalidUnicode && ok;
    ok = ValidateText(std::wstring{L'a', 0, L'b'}) == InputError::InvalidUnicode && ok;
    std::cout << "Translation input limits and Unicode=" << ok << '\n';
    return ok ? 0 : 1;
}
