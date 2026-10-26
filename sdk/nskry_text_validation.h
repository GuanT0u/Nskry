#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#include <cstdint>

// Length-delimited UTF-16, no embedded NUL, with at least one non-whitespace
// unit. No ownership transfer, allocation or dependency on a business service.
inline bool NskryValidText(const wchar_t* text, uint32_t length, uint32_t limit) {
    if (!text || !length || length > limit)
        return false;
    bool content = false;
    for (uint32_t i = 0; i < length; ++i) {
        const auto c = text[i];
        if (!c)
            return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == length || text[i] < 0xdc00 || text[i] > 0xdfff)
                return false;
        } else if (c >= 0xdc00 && c <= 0xdfff)
            return false;
        content = content || (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n');
    }
    return content;
}
