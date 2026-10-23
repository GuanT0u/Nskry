#pragma once
#include <cstddef>
#include <string_view>

namespace nskry::translate {
// Work limits are independent of provider. Never silently truncate user text.
inline constexpr size_t MaxTextUnits = 16000;
inline constexpr size_t MaxReplyBytes = 2 * 1024 * 1024;
inline constexpr unsigned TaskTimeoutMs = 60000;
enum class InputError { None, Empty, TooLong, InvalidUnicode };
enum class SourceLanguage { Unknown, English, Chinese, Unsupported };
// A bounded script-based choice for the two installed language packs, not a
// universal statistical language detector. Latin-only foreign text may require
// manual direction selection. Do not load an extra model merely to choose a pack.
inline SourceLanguage DetectSourceLanguage(std::wstring_view text) {
    size_t han = 0, latin = 0;
    bool otherScript = false;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned c = static_cast<unsigned>(text[i]);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size()) {
            const auto low = static_cast<unsigned>(text[++i]);
            c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
        }
        if ((c >= 0x3400 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x323af))
            ++han;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            ++latin;
        else if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x31f0 && c <= 0x31ff) ||
                 (c >= 0xac00 && c <= 0xd7af) || (c >= 0x1100 && c <= 0x11ff) ||
                 (c >= 0x0370 && c <= 0x052f) || (c >= 0x0590 && c <= 0x1fff))
            otherScript = true;
    }
    if (otherScript)
        return SourceLanguage::Unsupported;
    if (han && han * 5 >= latin)
        return SourceLanguage::Chinese;
    if (latin)
        return SourceLanguage::English;
    return han ? SourceLanguage::Chinese : SourceLanguage::Unknown;
}
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
