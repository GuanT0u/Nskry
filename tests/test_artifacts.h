#pragma once
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <windows.h>

namespace nskry::test {
// Own only a uniquely created child, never the supplied build root.
// No destructor deletion: failed/aborted tests retain their evidence.
class Artifacts {
  public:
    Artifacts(const std::filesystem::path &base, const wchar_t *prefix) {
        const std::wstring name(prefix);
        if (name.empty() || name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-") != name.npos)
            throw std::invalid_argument("Invalid test artifact prefix");
        const auto parent = std::filesystem::canonical(base);
        m_root = parent / (name + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(m_root))
            throw std::runtime_error("Test artifact directory already exists");
    }
    Artifacts(const Artifacts &) = delete;
    Artifacts &operator=(const Artifacts &) = delete;
    const std::filesystem::path &Path() const { return m_root; }
    bool Finish(bool passed) const {
        if (!passed || GetEnvironmentVariableW(L"NSKRY_KEEP_TEST_ARTIFACTS", nullptr, 0)) {
            std::wcout << L"Artifacts retained: " << m_root << L'\n';
            return true;
        }
        try {
            // Validate the exact target immediately before recursive deletion.
            // Refuse redirected roots/descendants rather than following links.
            const auto redirected = [](const std::filesystem::path &path) {
                const auto attr = GetFileAttributesW(path.c_str());
                return attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT);
            };
            if (redirected(m_root) || std::filesystem::canonical(m_root) != m_root)
                throw std::runtime_error("Artifact root was redirected");
            for (const auto &entry : std::filesystem::recursive_directory_iterator(m_root))
                if (redirected(entry.path()))
                    throw std::runtime_error("Artifact descendant was redirected");
            std::filesystem::remove_all(m_root);
            std::wcout << L"Successful test artifacts removed: " << m_root << L'\n';
            return true;
        } catch (const std::exception &e) {
            std::cerr << "Artifact cleanup refused/failed: " << e.what() << '\n';
            std::wcout << L"Artifacts retained: " << m_root << L'\n';
            return false;
        }
    }
  private:
    std::filesystem::path m_root;
};
} // namespace nskry::test
