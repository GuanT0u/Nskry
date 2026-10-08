#pragma once
#include <windows.h>
#include <utility>

namespace nskry::ocr {
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : m_value(value == INVALID_HANDLE_VALUE ? nullptr : value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) Reset(std::exchange(other.m_value, nullptr)); return *this; }
    HANDLE Get() const { return m_value; }
    explicit operator bool() const { return m_value != nullptr; }
    void Reset(HANDLE value = nullptr) { if (m_value) CloseHandle(m_value); m_value = value; }
private:
    HANDLE m_value{};
};
struct MappingView {
    void* value{};
    ~MappingView() { if (value) UnmapViewOfFile(value); }
};
} // namespace nskry::ocr
