#include "pch.h"
#include "core/json.h"

#include <charconv>
#include <climits>
#include <cmath>
#include <fstream>
#include <limits>

namespace nskry::json {
namespace {

constexpr size_t kMaximumDepth = 128;
constexpr size_t kMaximumFileBytes = 16u * 1024u * 1024u;

bool IsHighSurrogate(wchar_t ch) {
    return static_cast<unsigned int>(ch) >= 0xD800u && static_cast<unsigned int>(ch) <= 0xDBFFu;
}

bool IsLowSurrogate(wchar_t ch) {
    return static_cast<unsigned int>(ch) >= 0xDC00u && static_cast<unsigned int>(ch) <= 0xDFFFu;
}

void SetError(std::wstring* error, const std::wstring& message) {
    if (error) *error = message;
}

class Parser {
public:
    Parser(std::wstring_view input, std::wstring* error) : m_input(input), m_error(error) {}

    bool Run(Value& value) {
        if (!m_input.empty() && m_input.front() == 0xFEFF) ++m_position;
        SkipWhitespace();
        if (!ParseValue(value, 0)) return false;
        SkipWhitespace();
        if (m_position != m_input.size()) return Fail(L"Unexpected trailing content");
        return true;
    }

private:
    bool ParseValue(Value& value, size_t depth) {
        if (depth > kMaximumDepth) return Fail(L"JSON nesting is too deep");
        if (m_position >= m_input.size()) return Fail(L"Unexpected end of JSON input");
        switch (m_input[m_position]) {
        case L'n': return ParseLiteral(L"null", Value{}, value);
        case L't': return ParseLiteral(L"true", Value(true), value);
        case L'f': return ParseLiteral(L"false", Value(false), value);
        case L'"': {
            std::wstring string;
            if (!ParseString(string)) return false;
            value = Value(std::move(string));
            return true;
        }
        case L'[': return ParseArray(value, depth + 1);
        case L'{': return ParseObject(value, depth + 1);
        default:
            if (m_input[m_position] == L'-' ||
                (m_input[m_position] >= L'0' && m_input[m_position] <= L'9')) {
                return ParseNumber(value);
            }
            return Fail(L"Expected a JSON value");
        }
    }

    bool ParseLiteral(std::wstring_view literal, Value literalValue, Value& value) {
        if (m_input.substr(m_position, literal.size()) != literal) return Fail(L"Invalid JSON literal");
        m_position += literal.size();
        value = std::move(literalValue);
        return true;
    }

    bool ParseArray(Value& value, size_t depth) {
        ++m_position;
        SkipWhitespace();
        Value::Array array;
        if (Consume(L']')) {
            value = Value(std::move(array));
            return true;
        }
        while (true) {
            Value item;
            if (!ParseValue(item, depth)) return false;
            array.push_back(std::move(item));
            SkipWhitespace();
            if (Consume(L']')) break;
            if (!Consume(L',')) return Fail(L"Expected ',' or ']' in array");
            SkipWhitespace();
        }
        value = Value(std::move(array));
        return true;
    }

    bool ParseObject(Value& value, size_t depth) {
        ++m_position;
        SkipWhitespace();
        Value::Object object;
        if (Consume(L'}')) {
            value = Value(std::move(object));
            return true;
        }
        while (true) {
            if (m_position >= m_input.size() || m_input[m_position] != L'"') {
                return Fail(L"Expected a quoted object key");
            }
            std::wstring key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(L':')) return Fail(L"Expected ':' after object key");
            SkipWhitespace();
            Value member;
            if (!ParseValue(member, depth)) return false;
            if (!object.emplace(std::move(key), std::move(member)).second) {
                return Fail(L"Duplicate object key");
            }
            SkipWhitespace();
            if (Consume(L'}')) break;
            if (!Consume(L',')) return Fail(L"Expected ',' or '}' in object");
            SkipWhitespace();
        }
        value = Value(std::move(object));
        return true;
    }

    bool ParseString(std::wstring& result) {
        ++m_position;
        result.clear();
        while (m_position < m_input.size()) {
            const wchar_t ch = m_input[m_position++];
            if (ch == L'"') return true;
            if (static_cast<unsigned int>(ch) < 0x20u) return Fail(L"Unescaped control character in string");
            if (ch == L'\\') {
                if (m_position >= m_input.size()) return Fail(L"Incomplete string escape");
                const wchar_t escaped = m_input[m_position++];
                switch (escaped) {
                case L'"': result.push_back(L'"'); break;
                case L'\\': result.push_back(L'\\'); break;
                case L'/': result.push_back(L'/'); break;
                case L'b': result.push_back(L'\b'); break;
                case L'f': result.push_back(L'\f'); break;
                case L'n': result.push_back(L'\n'); break;
                case L'r': result.push_back(L'\r'); break;
                case L't': result.push_back(L'\t'); break;
                case L'u': if (!ParseUnicodeEscape(result)) return false; break;
                default: return Fail(L"Unknown string escape");
                }
                continue;
            }
#if WCHAR_MAX <= 0xFFFF
            if (IsHighSurrogate(ch)) {
                if (m_position >= m_input.size() || !IsLowSurrogate(m_input[m_position])) {
                    return Fail(L"Unpaired high surrogate in string");
                }
                result.push_back(ch);
                result.push_back(m_input[m_position++]);
            } else if (IsLowSurrogate(ch)) {
                return Fail(L"Unpaired low surrogate in string");
            } else {
                result.push_back(ch);
            }
#else
            if (static_cast<unsigned int>(ch) >= 0xD800u && static_cast<unsigned int>(ch) <= 0xDFFFu) {
                return Fail(L"Surrogate code point in string");
            }
            result.push_back(ch);
#endif
        }
        return Fail(L"Unterminated string");
    }

    bool ParseUnicodeEscape(std::wstring& result) {
        unsigned int first = 0;
        if (!ParseHexQuad(first)) return false;
        if (first >= 0xD800u && first <= 0xDBFFu) {
            if (m_position + 2 > m_input.size() || m_input[m_position] != L'\\' ||
                m_input[m_position + 1] != L'u') {
                return Fail(L"High surrogate must be followed by a low surrogate escape");
            }
            m_position += 2;
            unsigned int second = 0;
            if (!ParseHexQuad(second)) return false;
            if (second < 0xDC00u || second > 0xDFFFu) return Fail(L"Invalid low surrogate escape");
#if WCHAR_MAX <= 0xFFFF
            result.push_back(static_cast<wchar_t>(first));
            result.push_back(static_cast<wchar_t>(second));
#else
            result.push_back(static_cast<wchar_t>(0x10000u + ((first - 0xD800u) << 10u) + (second - 0xDC00u)));
#endif
            return true;
        }
        if (first >= 0xDC00u && first <= 0xDFFFu) return Fail(L"Unpaired low surrogate escape");
        result.push_back(static_cast<wchar_t>(first));
        return true;
    }

    bool ParseHexQuad(unsigned int& value) {
        if (m_position + 4 > m_input.size()) return Fail(L"Incomplete Unicode escape");
        value = 0;
        for (size_t index = 0; index < 4; ++index) {
            const wchar_t ch = m_input[m_position++];
            unsigned int digit = 0;
            if (ch >= L'0' && ch <= L'9') digit = static_cast<unsigned int>(ch - L'0');
            else if (ch >= L'a' && ch <= L'f') digit = static_cast<unsigned int>(ch - L'a' + 10);
            else if (ch >= L'A' && ch <= L'F') digit = static_cast<unsigned int>(ch - L'A' + 10);
            else return Fail(L"Invalid hexadecimal digit in Unicode escape");
            value = (value << 4u) | digit;
        }
        return true;
    }

    bool ParseNumber(Value& value) {
        const size_t start = m_position;
        if (Consume(L'-') && m_position >= m_input.size()) return Fail(L"Incomplete number");
        if (Consume(L'0')) {
            if (m_position < m_input.size() && m_input[m_position] >= L'0' && m_input[m_position] <= L'9') {
                return Fail(L"Leading zeros are not allowed in numbers");
            }
        } else {
            if (m_position >= m_input.size() || m_input[m_position] < L'1' || m_input[m_position] > L'9') {
                return Fail(L"Invalid number");
            }
            while (m_position < m_input.size() && m_input[m_position] >= L'0' && m_input[m_position] <= L'9') ++m_position;
        }
        if (Consume(L'.')) {
            if (m_position >= m_input.size() || m_input[m_position] < L'0' || m_input[m_position] > L'9') {
                return Fail(L"Fraction requires at least one digit");
            }
            while (m_position < m_input.size() && m_input[m_position] >= L'0' && m_input[m_position] <= L'9') ++m_position;
        }
        if (m_position < m_input.size() && (m_input[m_position] == L'e' || m_input[m_position] == L'E')) {
            ++m_position;
            if (m_position < m_input.size() && (m_input[m_position] == L'+' || m_input[m_position] == L'-')) ++m_position;
            if (m_position >= m_input.size() || m_input[m_position] < L'0' || m_input[m_position] > L'9') {
                return Fail(L"Exponent requires at least one digit");
            }
            while (m_position < m_input.size() && m_input[m_position] >= L'0' && m_input[m_position] <= L'9') ++m_position;
        }

        std::string bytes;
        bytes.reserve(m_position - start);
        for (size_t index = start; index < m_position; ++index) bytes.push_back(static_cast<char>(m_input[index]));
        double number = 0.0;
        const auto conversion = std::from_chars(bytes.data(), bytes.data() + bytes.size(), number, std::chars_format::general);
        if (conversion.ec != std::errc{} || conversion.ptr != bytes.data() + bytes.size() || !std::isfinite(number)) {
            return Fail(L"Number is outside the supported range");
        }
        value = Value(number);
        return true;
    }

    void SkipWhitespace() {
        while (m_position < m_input.size()) {
            const wchar_t ch = m_input[m_position];
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') break;
            ++m_position;
        }
    }

    bool Consume(wchar_t expected) {
        if (m_position >= m_input.size() || m_input[m_position] != expected) return false;
        ++m_position;
        return true;
    }

    bool Fail(const wchar_t* message) {
        if (m_error) {
            size_t line = 1;
            size_t column = 1;
            for (size_t index = 0; index < m_position && index < m_input.size(); ++index) {
                if (m_input[index] == L'\n') { ++line; column = 1; }
                else ++column;
            }
            *m_error = std::wstring(message) + L" at line " + std::to_wstring(line) +
                L", column " + std::to_wstring(column) + L".";
        }
        return false;
    }

    std::wstring_view m_input;
    size_t m_position = 0;
    std::wstring* m_error = nullptr;
};

} // namespace

const Value* Value::Find(std::wstring_view key) const {
    const Object* object = AsObject();
    if (!object) return nullptr;
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
}

bool Value::GetString(std::wstring_view key, std::wstring& value) const {
    const Value* member = Find(key);
    const std::wstring* string = member ? member->AsString() : nullptr;
    if (!string) return false;
    value = *string;
    return true;
}

bool Value::GetBool(std::wstring_view key, bool& value) const {
    const Value* member = Find(key);
    const bool* boolean = member ? member->AsBool() : nullptr;
    if (!boolean) return false;
    value = *boolean;
    return true;
}

bool Value::GetUnsigned(std::wstring_view key, unsigned int& value) const {
    const Value* member = Find(key);
    const double* number = member ? member->AsNumber() : nullptr;
    if (!number || *number < 0.0 || *number > static_cast<double>(std::numeric_limits<unsigned int>::max()) ||
        std::floor(*number) != *number) return false;
    value = static_cast<unsigned int>(*number);
    return true;
}

bool Parse(std::wstring_view text, Value& value, std::wstring* error) {
    if (error) error->clear();
    Value parsed;
    if (!Parser(text, error).Run(parsed)) return false;
    value = std::move(parsed);
    return true;
}

bool ParseUtf8File(const std::wstring& path, Value& value, std::wstring* error) {
    if (error) error->clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SetError(error, L"Unable to open JSON file.");
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0 || static_cast<unsigned long long>(length) > kMaximumFileBytes) {
        SetError(error, L"JSON file is too large.");
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(length), '\0');
    if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        SetError(error, L"Unable to read JSON file.");
        return false;
    }
    size_t offset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEFu &&
        static_cast<unsigned char>(bytes[1]) == 0xBBu && static_cast<unsigned char>(bytes[2]) == 0xBFu) offset = 3;
    if (bytes.size() - offset > static_cast<size_t>(std::numeric_limits<int>::max())) {
        SetError(error, L"JSON file is too large.");
        return false;
    }
    std::wstring text;
    if (offset < bytes.size()) {
        const int byteCount = static_cast<int>(bytes.size() - offset);
        const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset, byteCount, nullptr, 0);
        if (required <= 0) {
            SetError(error, L"JSON file is not valid UTF-8.");
            return false;
        }
        text.resize(static_cast<size_t>(required));
        if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset, byteCount,
                                  text.data(), required) != required) {
            SetError(error, L"JSON file is not valid UTF-8.");
            return false;
        }
    }
    return Parse(text, value, error);
}

std::wstring EscapeString(std::wstring_view value) {
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'"': escaped += L"\\\""; break;
        case L'\\': escaped += L"\\\\"; break;
        case L'\b': escaped += L"\\b"; break;
        case L'\f': escaped += L"\\f"; break;
        case L'\n': escaped += L"\\n"; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\t': escaped += L"\\t"; break;
        default:
            if (static_cast<unsigned int>(ch) < 0x20u) {
                escaped += L"\\u00";
                escaped.push_back(hex[(static_cast<unsigned int>(ch) >> 4u) & 0xFu]);
                escaped.push_back(hex[static_cast<unsigned int>(ch) & 0xFu]);
            } else {
                escaped.push_back(ch);
            }
        }
    }
    return escaped;
}

bool ToUtf8(std::wstring_view value, std::string& result) {
    result.clear();
    if (value.empty()) return true;
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    const int length = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                               nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    result.resize(static_cast<size_t>(required));
    return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                 result.data(), required, nullptr, nullptr) == required;
}

} // namespace nskry::json
