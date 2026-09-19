#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nskry::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::wstring, Value, std::less<>>;

    Value() = default;
    explicit Value(bool value) : m_data(value) {}
    explicit Value(double value) : m_data(value) {}
    explicit Value(std::wstring value) : m_data(std::move(value)) {}
    explicit Value(Array value) : m_data(std::move(value)) {}
    explicit Value(Object value) : m_data(std::move(value)) {}

    bool IsNull() const { return std::holds_alternative<std::nullptr_t>(m_data); }
    bool IsBool() const { return std::holds_alternative<bool>(m_data); }
    bool IsNumber() const { return std::holds_alternative<double>(m_data); }
    bool IsString() const { return std::holds_alternative<std::wstring>(m_data); }
    bool IsArray() const { return std::holds_alternative<Array>(m_data); }
    bool IsObject() const { return std::holds_alternative<Object>(m_data); }

    const bool* AsBool() const { return std::get_if<bool>(&m_data); }
    const double* AsNumber() const { return std::get_if<double>(&m_data); }
    const std::wstring* AsString() const { return std::get_if<std::wstring>(&m_data); }
    const Array* AsArray() const { return std::get_if<Array>(&m_data); }
    const Object* AsObject() const { return std::get_if<Object>(&m_data); }

    const Value* Find(std::wstring_view key) const;
    bool GetString(std::wstring_view key, std::wstring& value) const;
    bool GetBool(std::wstring_view key, bool& value) const;
    bool GetUnsigned(std::wstring_view key, unsigned int& value) const;

private:
    std::variant<std::nullptr_t, bool, double, std::wstring, Array, Object> m_data;
};

/// Parses one complete JSON document. Duplicate object keys, invalid Unicode,
/// non-finite numbers, trailing content, and excessive nesting are rejected.
bool Parse(std::wstring_view text, Value& value, std::wstring* error = nullptr);

/// Reads a UTF-8 (optionally BOM-prefixed) file and parses it as JSON.
bool ParseUtf8File(const std::wstring& path, Value& value, std::wstring* error = nullptr);

/// Escapes a string for use between JSON quotation marks.
std::wstring EscapeString(std::wstring_view value);

/// Strict UTF-16/UTF-32 to UTF-8 conversion for JSON writers.
bool ToUtf8(std::wstring_view value, std::string& result);

} // namespace nskry::json
