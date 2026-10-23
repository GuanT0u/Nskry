#pragma once
#include <cstddef>
#include <string_view>

namespace nskry::translate {
// Work limits are independent of provider. Never silently truncate user text.
inline constexpr size_t MaxTextUnits = 16000;
inline constexpr size_t MaxReplyBytes = 2 * 1024 * 1024;
inline constexpr unsigned TaskTimeoutMs = 60000;
enum class InputError { None, Empty, TooLong, InvalidUnicode };
inline InputError ValidateText(std::wstring_view text) {
    if (text.size() > MaxTextUnits)
        return InputError::TooLong;
    bool content = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned>(text[i]);
        if (!c)
            return InputError::InvalidUnicode;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                return InputError::InvalidUnicode;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return InputError::InvalidUnicode;
        }
        content = content || (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n');
    }
    return content ? InputError::None : InputError::Empty;
}
} // namespace nskry::translate
