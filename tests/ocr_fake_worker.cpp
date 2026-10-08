#include <windows.h>
int main() {
    wchar_t mode[16]{}; GetEnvironmentVariableW(L"NSKRY_OCR_FAKE_MODE", mode, 16);
    if (wcscmp(mode, L"hang") == 0) { Sleep(30000); return 0; }
    const char invalid[]{'n', 'o', 't', 'o', 'c', 'r'}; DWORD count{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), invalid, sizeof(invalid), &count, nullptr);
    return 0;
}
