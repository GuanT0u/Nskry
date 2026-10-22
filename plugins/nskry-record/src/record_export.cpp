#include "record_export.h"
#include "isolated_encoder.h"
#include "record_delivery.h"
#include "record_trim.h"
#include <filesystem>
#include <objbase.h>
#include <winrt/base.h>
namespace nskry::record {
ExportTask::ExportTask() {
    m_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_cancel)
        winrt::throw_last_error();
}
ExportTask::~ExportTask() {
    Shutdown();
    CloseHandle(m_cancel);
}
void ExportTask::Shutdown() {
    Cancel();
    if (m_thread.joinable())
        m_thread.join();
}
void ExportTask::Cancel() {
    SetEvent(m_cancel);
}
ExportStatus ExportTask::Status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
}
void ExportTask::Start(std::wstring input, std::wstring output, Snapshot source, bool gif, int64_t start, int64_t end,
                       bool trim) {
    if (m_thread.joinable() || input == output || (trim && !ValidTrim(start, end, source.encodedDuration)))
        winrt::throw_hresult(E_INVALIDARG);
    m_thread = std::thread([=, this] {
        ExportStatus result;
        bool apartment = false, owned = false;
        std::wstring temporary;
        std::unique_ptr<IsolatedEncoder> encoder;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            if (trim)
                PrepareRecordingCache(output, m_cancel);
            GUID guid{};
            winrt::check_hresult(CoCreateGuid(&guid));
            wchar_t text[40]{};
            if (!StringFromGUID2(guid, text, 40))
                winrt::throw_hresult(E_FAIL);
            temporary = (std::filesystem::path(output).parent_path() /
                         (std::wstring(L".nskry-export-") + text + (gif ? L".gif" : L".mp4")))
                            .wstring();
            if (trim) {
                encoder = std::make_unique<IsolatedEncoder>(m_cancel);
                encoder->Open(temporary, source.width, source.height, source.fps, true, source.bitrate, gif);
                encoder->Trim(input, start, end);
                encoder->Finalize();
                if (!encoder->Close())
                    winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                owned = encoder->CreatedFile();
            } else {
                const auto progress = [](LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                         HANDLE, HANDLE, LPVOID value) -> DWORD {
                    return WaitForSingleObject(static_cast<HANDLE>(value), 0) == WAIT_OBJECT_0 ? PROGRESS_CANCEL
                                                                                               : PROGRESS_CONTINUE;
                };
                // Reserve only our generated temporary. Never copy over a user's file.
                HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                    winrt::throw_last_error();
                CloseHandle(file);
                owned = true;
                if (!CopyFileExW(input.c_str(), temporary.c_str(), progress, m_cancel, nullptr, 0))
                    winrt::throw_last_error();
            }
            if (WaitForSingleObject(m_cancel, 0) == WAIT_OBJECT_0)
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
                winrt::throw_last_error();
            owned = false;
            result.success = true;
            result.message = trim ? L"Trim applied. Choose Save or Copy file." : L"Recording saved.";
        } catch (const winrt::hresult_error &e) {
            result.message = e.message().c_str();
        } catch (...) {
            result.message = L"Export failed. The original recording is unchanged.";
        }
        if (encoder) {
            const bool exited = encoder->Close();
            owned = owned || encoder->CreatedFile();
            encoder.reset();
            if (!exited)
                result.message += L" Encoder exit could not be confirmed.";
        }
        if (owned && !DeleteFileW(temporary.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            result.message += L" Temporary file retained: " + temporary;
        if (apartment)
            winrt::uninit_apartment();
        result.done = true;
        std::lock_guard lock(m_mutex);
        m_status = std::move(result);
    });
}
} // namespace nskry::record
