#include "pch.h"
#include "core/plugin_package_manager.h"
#include "update/package_verifier.h"

#include <fstream>
#include <regex>

namespace nskry {
namespace {

bool EnsureDirectory(const std::wstring& path) {
    return !path.empty() && (::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS);
}

bool IsChildOf(const std::wstring& root, const std::wstring& path) {
    if (path.size() <= root.size() || _wcsnicmp(root.c_str(), path.c_str(), root.size()) != 0) return false;
    return path[root.size()] == L'\\';
}

bool DeleteTree(const std::wstring& root, const std::wstring& path) {
    if (!IsChildOf(root, path)) return false;
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            const std::wstring child = path + L"\\" + data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (!::RemoveDirectoryW(child.c_str())) { ::FindClose(find); return false; }
                } else if (!DeleteTree(root, child)) {
                    ::FindClose(find);
                    return false;
                }
            } else if (!::DeleteFileW(child.c_str())) {
                ::FindClose(find);
                return false;
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }
    return ::RemoveDirectoryW(path.c_str()) != FALSE;
}

bool ValidateTreeRecursive(const std::wstring& root, const std::wstring& path, std::wstring* error) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (error) *error = L"Plugin package extraction produced a reparse point.";
            ::FindClose(find);
            return false;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY &&
            !ValidateTreeRecursive(root, path + L"\\" + data.cFileName, error)) {
            ::FindClose(find);
            return false;
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return true;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(required, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

bool ReadUtf8File(const std::wstring& path, std::wstring& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) { content.clear(); return true; }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (required <= 0) return false;
    content.resize(required);
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), content.data(), required) > 0;
}

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'\"') quoted += L"\\\"";
        else quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

bool IsSafeRelativeStagingPath(const std::wstring& path) {
    return path.rfind(L"staging/pending-updates/", 0) == 0 && path.find(L"..") == std::wstring::npos &&
        path.find_first_of(L"\\:") == std::wstring::npos;
}

} // namespace

PluginPackageManager::PluginPackageManager(PluginRegistry& registry)
    : m_registry(registry) {}

bool PluginPackageManager::Initialize() {
    if (m_registry.AppRoot().empty()) return false;
    m_stagingRoot = m_registry.AppRoot() + L"\\staging";
    m_packagesRoot = m_stagingRoot + L"\\packages";
    m_updatesRoot = m_stagingRoot + L"\\pending-updates";
    m_rollbackRoot = m_stagingRoot + L"\\rollback";
    m_pendingPath = m_stagingRoot + L"\\pending-operations.json";
    return EnsureDirectories();
}

bool PluginPackageManager::EnsureDirectories() const {
    return EnsureDirectory(m_stagingRoot) && EnsureDirectory(m_packagesRoot) &&
        EnsureDirectory(m_updatesRoot) && EnsureDirectory(m_rollbackRoot);
}

std::wstring PluginPackageManager::CreateStagingDirectory(const std::wstring& parent, const wchar_t* prefix) const {
    wchar_t temporary[MAX_PATH]{};
    if (!::GetTempFileNameW(parent.c_str(), prefix, 0, temporary)) return {};
    ::DeleteFileW(temporary);
    if (!::CreateDirectoryW(temporary, nullptr)) return {};
    return temporary;
}

bool PluginPackageManager::ExtractToStaging(const std::wstring& packagePath, const std::wstring& destination,
                                            std::wstring* error) const {
    wchar_t systemDirectory[MAX_PATH]{};
    if (::GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0) {
        if (error) *error = L"Unable to locate the Windows system directory.";
        return false;
    }
    const std::wstring tarPath = std::wstring(systemDirectory) + L"\\tar.exe";
    const std::wstring command = QuoteArgument(tarPath) + L" -xf " + QuoteArgument(packagePath) + L" -C " + QuoteArgument(destination);
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(tarPath.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &process)) {
        if (error) *error = L"Unable to start the Windows ZIP extractor.";
        return false;
    }
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (exitCode != 0) {
        if (error) *error = L"The plugin package could not be extracted.";
        return false;
    }
    return true;
}

bool PluginPackageManager::ValidateExtractedTree(const std::wstring& root, std::wstring* error) const {
    const DWORD attributes = ::GetFileAttributesW((root + L"\\manifest.json").c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = L"Extracted package is missing manifest.json.";
        return false;
    }
    return ValidateTreeRecursive(root, root, error);
}

bool PluginPackageManager::InspectPackage(const std::wstring& packagePath, PluginManifest& manifest,
                                          std::wstring* error) {
    if (!EnsureDirectories()) {
        if (error) *error = L"Package staging directory is unavailable.";
        return false;
    }
    VerifiedPackage verified;
    if (!PackageVerifier::VerifyNskryPluginArchive(packagePath, verified, error)) return false;

    const std::wstring stage = CreateStagingDirectory(m_packagesRoot, L"chk");
    if (stage.empty()) {
        if (error) *error = L"Unable to create package inspection directory.";
        return false;
    }
    const auto cleanup = [&]() {
        if (::GetFileAttributesW(stage.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteTree(m_packagesRoot, stage);
    };
    const bool valid = ExtractToStaging(packagePath, stage, error) &&
        ValidateExtractedTree(stage, error) &&
        PluginManifest::LoadFromFile(stage + L"\\manifest.json", manifest, error);
    if (valid) {
        const DWORD entryAttributes = ::GetFileAttributesW((stage + L"\\" + manifest.entry).c_str());
        if (entryAttributes == INVALID_FILE_ATTRIBUTES || (entryAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (error) *error = L"Package entry DLL is missing.";
            cleanup();
            return false;
        }
    }
    cleanup();
    return valid;
}

PluginPackageResult PluginPackageManager::InstallPackage(const std::wstring& packagePath, PluginSource source) {
    PluginPackageResult result;
    if (!EnsureDirectories()) { result.message = L"Package staging directory is unavailable."; return result; }

    VerifiedPackage verified;
    if (!PackageVerifier::VerifyNskryPluginArchive(packagePath, verified, &result.message)) return result;

    const std::wstring stage = CreateStagingDirectory(m_packagesRoot, L"pkg");
    if (stage.empty()) { result.message = L"Unable to create package staging directory."; return result; }
    const auto cleanupStage = [&]() { if (::GetFileAttributesW(stage.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteTree(m_packagesRoot, stage); };

    if (!ExtractToStaging(packagePath, stage, &result.message) || !ValidateExtractedTree(stage, &result.message)) {
        cleanupStage();
        return result;
    }

    PluginManifest manifest;
    if (!PluginManifest::LoadFromFile(stage + L"\\manifest.json", manifest, &result.message)) {
        cleanupStage();
        return result;
    }
    const DWORD entryAttributes = ::GetFileAttributesW((stage + L"\\" + manifest.entry).c_str());
    if (entryAttributes == INVALID_FILE_ATTRIBUTES || (entryAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.message = L"Package entry DLL is missing.";
        cleanupStage();
        return result;
    }
    result.pluginId = manifest.id;

    const std::wstring target = TargetPathForId(manifest.id);
    if (m_registry.Find(manifest.id)) {
        const std::wstring updateStage = CreateStagingDirectory(m_updatesRoot, L"upd");
        if (updateStage.empty() || !::RemoveDirectoryW(updateStage.c_str()) || !::MoveFileExW(stage.c_str(), updateStage.c_str(), MOVEFILE_WRITE_THROUGH)) {
            result.message = L"Unable to stage plugin update.";
            if (!updateStage.empty() && ::GetFileAttributesW(updateStage.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteTree(m_updatesRoot, updateStage);
            cleanupStage();
            return result;
        }
        std::vector<PendingOperation> operations;
        ReadPending(operations);
        // Keep only the newest staged update for a plugin. This also prevents
        // repeated development installs from applying obsolete packages first.
        std::erase_if(operations, [&](const PendingOperation& pending) {
            if (pending.type != PendingType::Update || pending.pluginId != manifest.id ||
                !IsSafeRelativeStagingPath(pending.stagedPath)) return false;
            std::wstring relative = pending.stagedPath;
            std::replace(relative.begin(), relative.end(), L'/', L'\\');
            const std::wstring obsoleteStage = m_registry.AppRoot() + L"\\" + relative;
            if (IsChildOf(m_updatesRoot, obsoleteStage) &&
                ::GetFileAttributesW(obsoleteStage.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteTree(m_updatesRoot, obsoleteStage);
            }
            return true;
        });
        operations.push_back({ PendingType::Update, manifest.id,
            L"staging/pending-updates/" + updateStage.substr(m_updatesRoot.size() + 1) });
        if (!WritePending(operations)) {
            result.message = L"Unable to record pending plugin update.";
            DeleteTree(m_updatesRoot, updateStage);
            return result;
        }
        result.success = true;
        result.restartRequired = true;
        result.message = L"Plugin update is staged and will apply when Nskry restarts.";
        return result;
    }

    if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES || !::MoveFileExW(stage.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        result.message = L"Unable to install plugin package.";
        cleanupStage();
        return result;
    }
    if (!m_registry.UpsertInstalled(std::move(manifest), target, source)) {
        DeleteTree(m_registry.PluginRoot(), target);
        result.message = L"Unable to save installed plugin metadata.";
        return result;
    }
    result.success = true;
    result.message = L"Plugin installed successfully.";
    return result;
}

PluginPackageResult PluginPackageManager::Uninstall(const std::wstring& pluginId) {
    PluginPackageResult result;
    result.pluginId = pluginId;
    if (!PluginManifest::IsSafePluginId(pluginId) || !m_registry.Find(pluginId)) {
        result.message = L"Plugin is not installed.";
        return result;
    }
    std::vector<PendingOperation> operations;
    ReadPending(operations);
    operations.push_back({ PendingType::Delete, pluginId, {} });
    if (!WritePending(operations)) {
        result.message = L"Unable to record pending plugin removal.";
        return result;
    }
    result.success = true;
    result.restartRequired = true;
    result.message = L"Plugin removal is scheduled for the next Nskry restart.";
    return result;
}

bool PluginPackageManager::ReadPending(std::vector<PendingOperation>& operations) const {
    operations.clear();
    std::wstring json;
    if (!ReadUtf8File(m_pendingPath, json)) return true;
    const std::wregex item(LR"json(\{\s*"type"\s*:\s*"(update|delete)"\s*,\s*"id"\s*:\s*"([^"\\]+)"\s*,\s*"staged_path"\s*:\s*"([^"\\]*)"\s*\})json");
    for (std::wsregex_iterator it(json.begin(), json.end(), item), end; it != end; ++it) {
        const std::wstring type = (*it)[1].str();
        const std::wstring id = (*it)[2].str();
        const std::wstring stagedPath = (*it)[3].str();
        if (!PluginManifest::IsSafePluginId(id) || (type == L"update" && !IsSafeRelativeStagingPath(stagedPath))) continue;
        operations.push_back({ type == L"update" ? PendingType::Update : PendingType::Delete, id, stagedPath });
    }
    return true;
}

bool PluginPackageManager::WritePending(const std::vector<PendingOperation>& operations) const {
    if (operations.empty()) {
        if (::GetFileAttributesW(m_pendingPath.c_str()) != INVALID_FILE_ATTRIBUTES) return ::DeleteFileW(m_pendingPath.c_str()) != FALSE;
        return true;
    }
    std::ofstream output(m_pendingPath, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "{\n  \"operations\": [";
    bool first = true;
    for (const auto& operation : operations) {
        if (!first) output << ',';
        output << "\n    {\"type\": \"" << (operation.type == PendingType::Update ? "update" : "delete")
               << "\", \"id\": \"" << ToUtf8(operation.pluginId)
               << "\", \"staged_path\": \"" << ToUtf8(operation.stagedPath) << "\"}";
        first = false;
    }
    output << "\n  ]\n}\n";
    return static_cast<bool>(output);
}

bool PluginPackageManager::ApplyPendingOperations(std::wstring* error) {
    std::vector<PendingOperation> operations;
    if (!ReadPending(operations)) return false;
    std::vector<PendingOperation> remaining;
    bool success = true;
    for (const PendingOperation& operation : operations) {
        std::wstring operationError;
        const bool applied = operation.type == PendingType::Update ? ApplyUpdate(operation, &operationError) : ApplyDelete(operation, &operationError);
        if (!applied) {
            success = false;
            remaining.push_back(operation);
            if (error && error->empty()) *error = operationError;
        }
    }
    return WritePending(remaining) && success;
}

bool PluginPackageManager::ApplyUpdate(const PendingOperation& operation, std::wstring* error) {
    if (!IsSafeRelativeStagingPath(operation.stagedPath)) { if (error) *error = L"Pending update path is invalid."; return false; }
    std::wstring relativeStage = operation.stagedPath;
    std::replace(relativeStage.begin(), relativeStage.end(), L'/', L'\\');
    const std::wstring stage = m_registry.AppRoot() + L"\\" + relativeStage;
    const std::wstring target = TargetPathForId(operation.pluginId);
    if (!IsChildOf(m_updatesRoot, stage) || ::GetFileAttributesW(stage.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error) *error = L"Pending update files are missing.";
        return false;
    }
    const std::wstring backup = CreateStagingDirectory(m_rollbackRoot, L"bak");
    if (backup.empty() || !::RemoveDirectoryW(backup.c_str())) { if (error) *error = L"Unable to create plugin rollback directory."; return false; }

    const bool targetExists = ::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (targetExists && !::MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = L"Unable to preserve the current plugin for rollback.";
        return false;
    }
    if (!::MoveFileExW(stage.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (targetExists) ::MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
        else ::RemoveDirectoryW(backup.c_str());
        if (error) *error = L"Unable to apply pending plugin update.";
        return false;
    }
    if (targetExists) DeleteTree(m_rollbackRoot, backup);
    return true;
}

bool PluginPackageManager::ApplyDelete(const PendingOperation& operation, std::wstring* error) {
    const std::wstring target = TargetPathForId(operation.pluginId);
    if (::GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (!DeleteTree(m_registry.PluginRoot(), target)) {
        if (error) *error = L"Unable to remove plugin files.";
        return false;
    }
    return true;
}

std::wstring PluginPackageManager::TargetPathForId(const std::wstring& pluginId) const {
    return PluginManifest::IsSafePluginId(pluginId) ? m_registry.PluginRoot() + L"\\" + pluginId : std::wstring{};
}

} // namespace nskry
