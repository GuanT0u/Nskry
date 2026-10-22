#include "record_preview.h"
#include "isolated_encoder.h"
#include <algorithm>
#include <winrt/base.h>
namespace nskry::record {
PreviewTask::PreviewTask(std::wstring path, UINT width, UINT height, bool gif) {
    m_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    try {
        if (!m_cancel || !m_request)
            winrt::throw_last_error();
        m_thread = std::thread([this, path = std::move(path), width, height, gif] { Run(path, width, height, gif); });
    } catch (...) {
        if (m_cancel)
            CloseHandle(m_cancel);
        if (m_request)
            CloseHandle(m_request);
        throw;
    }
}
PreviewTask::~PreviewTask() {
    Cancel();
    if (m_thread.joinable())
        m_thread.join();
    CloseHandle(m_cancel);
    CloseHandle(m_request);
}
bool PreviewTask::Request(int64_t time) {
    std::lock_guard lock(m_mutex);
    if (m_done || m_finishing || time < 0)
        return false;
    if (m_serial && time == m_time)
        return true;
    m_time = time;
    ++m_serial;
    SetEvent(m_request);
    return true;
}
void PreviewTask::Finish() {
    std::lock_guard lock(m_mutex);
    m_finishing = true;
    SetEvent(m_request);
}
void PreviewTask::Cancel() {
    SetEvent(m_cancel);
}
bool PreviewTask::Done() const {
    std::lock_guard lock(m_mutex);
    return m_done;
}
bool PreviewTask::Take(PreviewFrame &frame, bool &done) {
    std::lock_guard lock(m_mutex);
    done = m_done;
    if (m_ready) {
        frame = std::move(m_frame);
        m_ready = false;
        return true;
    }
    return false;
}
void PreviewTask::Run(std::wstring path, UINT sourceWidth, UINT sourceHeight, bool gif) noexcept {
    bool apartment = false;
    std::unique_ptr<IsolatedEncoder> decoder;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (!sourceWidth || !sourceHeight)
            winrt::throw_hresult(E_INVALIDARG);
        const double scale = (std::min)({1.0, 640.0 / sourceWidth, 360.0 / sourceHeight});
        const UINT width = (std::max)(2u, UINT(sourceWidth * scale) & ~1u);
        const UINT height = (std::max)(2u, UINT(sourceHeight * scale) & ~1u);
        uint64_t processed = 0, lastStarted = 0;
        const HANDLE events[]{m_cancel, m_request};
        while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            const auto now = GetTickCount64();
            if (lastStarted && now - lastStarted < 125 &&
                WaitForSingleObject(m_cancel, DWORD(125 - (now - lastStarted))) == WAIT_OBJECT_0)
                break;
            int64_t time{};
            uint64_t serial{};
            {
                std::lock_guard lock(m_mutex);
                if (m_finishing && processed == m_serial)
                    break;
                time = m_time;
                serial = m_serial;
            }
            if (!serial)
                continue;
            if (serial == processed)
                continue;
            if (!decoder) {
                decoder = std::make_unique<IsolatedEncoder>(m_cancel);
                decoder->Open(path, width, height, gif ? 10 : 30, false, 0, gif, true, sourceWidth, sourceHeight);
            }
            lastStarted = GetTickCount64();
            PreviewFrame frame{width, height, time, std::vector<uint8_t>(uint64_t(width) * height * 4)};
            decoder->Preview(time, frame.pixels.data(), UINT(frame.pixels.size()));
            {
                std::lock_guard lock(m_mutex);
                m_frame = std::move(frame);
                m_ready = true;
                processed = serial;
                if (m_finishing && processed == m_serial)
                    break;
                if (processed != m_serial)
                    SetEvent(m_request);
            }
        }
    } catch (const winrt::hresult_error &e) {
        if (WaitForSingleObject(m_cancel, 0) != WAIT_OBJECT_0) {
            std::lock_guard lock(m_mutex);
            m_frame.error = e.message().c_str();
            m_ready = true;
        }
    } catch (...) {
        if (WaitForSingleObject(m_cancel, 0) != WAIT_OBJECT_0) {
            std::lock_guard lock(m_mutex);
            m_frame.error = L"Preview unavailable.";
            m_ready = true;
        }
    }
    if (decoder) {
        decoder->Close();
        decoder.reset();
    }
    if (apartment)
        winrt::uninit_apartment();
    std::lock_guard lock(m_mutex);
    m_done = true;
}
} // namespace nskry::record
