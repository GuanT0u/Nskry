#include "record_session.h"
#include "mf_encoder.h"
#include <chrono>
#include <memory>
#include <shellscalingapi.h>
#include <vector>
#include <winrt/base.h>

namespace nskry::record {
int64_t RecordSession::Now() {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
RecordSession::RecordSession(NskryCaptureServiceV1 service, RECT region, std::wstring output)
    : m_service(service), m_region(region), m_output(std::move(output)) {
}
RecordSession::~RecordSession() {
    Shutdown();
}
bool RecordSession::Start() {
    std::lock_guard lock(m_mutex);
    if (!m_timeline.Start())
        return false;
    try {
        m_worker = std::thread([this] { Run(); });
    } catch (...) {
        m_timeline.Complete(false);
        m_status.message = L"Could not start recording thread.";
        return false;
    }
    return true;
}
bool RecordSession::PauseResume() {
    std::lock_guard lock(m_mutex);
    const bool changed = m_timeline.state == State::Paused ? m_timeline.Resume(Now()) : m_timeline.Pause(Now());
    if (changed)
        m_changed.notify_one();
    return changed;
}
bool RecordSession::Stop(bool discard) {
    std::lock_guard lock(m_mutex);
    const auto changed = m_timeline.Stop(Now(), discard);
    if (changed)
        m_changed.notify_one();
    return changed;
}
void RecordSession::Shutdown() {
    Stop(true);
    if (m_worker.joinable())
        m_worker.join();
}
Snapshot RecordSession::Status() const {
    std::lock_guard lock(m_mutex);
    auto copy = m_status;
    copy.state = m_timeline.state;
    copy.duration = m_timeline.Elapsed(Now());
    return copy;
}
void RecordSession::Run() noexcept {
    constexpr UINT fps = 20;
    struct Capture {
        NskryCaptureServiceV1 &service;
        void *value{};
        void Close() {
            if (value) {
                service.close(value);
                value = nullptr;
            }
        }
        ~Capture() {
            Close();
        }
    } capture{m_service};
    std::unique_ptr<MfEncoder> encoder;
    std::wstring temporary;
    bool apartment = false, owned = false, success = false;
    std::wstring outcome;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        GUID id{};
        winrt::check_hresult(CoCreateGuid(&id));
        wchar_t guid[40]{};
        StringFromGUID2(id, guid, 40);
        const auto slash = m_output.find_last_of(L"\\/");
        temporary = m_output.substr(0, slash + 1) + L".nskry-record-" + guid + L".partial.mp4";
        NskryCaptureDescriptionV1 description{sizeof(description), 1};
        wchar_t error[512]{};
        if (Status().state == State::Discarding)
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        auto hr = m_service.open(m_region, fps, &capture.value, &description, error, 512);
        if (FAILED(hr))
            throw winrt::hresult_error(hr, error);
        const auto expected = description;
        if (description.version != 1 || description.width < 2 || description.height < 2 || description.width > 1920 ||
            description.height > 1920 || uint64_t(description.width) * description.height > 1920 * 1080 ||
            description.stride != description.width * 4)
            winrt::throw_hresult(E_BOUNDS);
        if (Status().state == State::Discarding)
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        std::vector<uint8_t> pixels(uint64_t(description.stride) * description.height);
        std::vector<uint8_t> pending(pixels.size());
        encoder = std::make_unique<MfEncoder>();
        encoder->Open(temporary, description.width, description.height, fps);
        {
            std::lock_guard lock(m_mutex);
            m_status.backend = encoder->Backend();
        }
        FrameSchedule schedule(fps);
        bool ready = false, haveFrame = false, havePending = false;
        int64_t pendingTime = 0;
        auto startupDeadline = Now() + 100000000; // ten seconds, cancel-aware waits
        MONITORINFOEXW initial{};
        initial.cbSize = sizeof(initial);
        const auto monitor = MonitorFromRect(&m_region, MONITOR_DEFAULTTONULL);
        if (!monitor || !GetMonitorInfoW(monitor, &initial))
            winrt::throw_hresult(E_FAIL);
        UINT initialDpiX{}, initialDpiY{};
        winrt::check_hresult(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &initialDpiX, &initialDpiY));
        while (true) {
            Snapshot snapshot = Status();
            if (snapshot.state == State::Stopping || snapshot.state == State::Discarding)
                break;
            if (snapshot.state == State::Paused) {
                capture.Close(); // no WGC/GPU capture allocations or polling while paused
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [this] { return m_timeline.state != State::Paused; });
                continue;
            }
            if (!capture.value) {
                MONITORINFOEXW now{};
                now.cbSize = sizeof(now);
                UINT dpiX{}, dpiY{};
                if (!GetMonitorInfoW(monitor, &now) || !EqualRect(&initial.rcMonitor, &now.rcMonitor) ||
                    wcscmp(initial.szDevice, now.szDevice) ||
                    FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX != initialDpiX ||
                    dpiY != initialDpiY)
                    winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED));
                winrt::check_hresult(m_service.open(m_region, fps, &capture.value, &description, error, 512));
                if (description.width != expected.width || description.height != expected.height)
                    winrt::throw_hresult(E_BOUNDS);
                haveFrame = false;
                startupDeadline = Now() + 100000000;
            }
            if (schedule.Due(snapshot.duration) || !ready) {
                const HRESULT read = m_service.read(capture.value, pixels.data(), uint32_t(pixels.size()));
                winrt::check_hresult(read);
                if (read == S_OK)
                    haveFrame = true;
                if (!haveFrame) {
                    if (Now() > startupDeadline)
                        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                } else {
                    const bool first = !ready;
                    if (first) {
                        std::lock_guard lock(m_mutex);
                        if (!m_timeline.Ready(Now()))
                            break;
                        ready = true;
                    }
                    // Fixed screen region: source HWND closing/minimizing is ordinary desktop content.
                    // Static desktops may legitimately have no new frames. Service checks item,
                    // layout and device health even on S_FALSE; repeat last pixels at real timestamps.
                    const auto at = first ? int64_t(0) : Status().duration;
                    if (havePending && at > pendingTime) {
                        encoder->Write(pending.data(), pendingTime, at - pendingTime);
                        std::lock_guard lock(m_mutex);
                        ++m_status.frames;
                        if (at - pendingTime > schedule.interval * 2)
                            ++m_status.skipped;
                    }
                    pending.swap(pixels);
                    havePending = true;
                    pendingTime = at;
                    // pixels must retain latest content if WGC reports no new frame.
                    pixels = pending;
                    schedule.Submitted(at);
                }
            }
            std::unique_lock lock(m_mutex);
            const auto elapsed = m_timeline.Elapsed(Now());
            const auto delay =
                ready && haveFrame ? (std::max)(int64_t(10000), schedule.next - elapsed) : int64_t(100000);
            m_changed.wait_for(lock, std::chrono::nanoseconds(delay * 100), [this] {
                return m_timeline.state == State::Paused || m_timeline.state == State::Stopping ||
                       m_timeline.state == State::Discarding;
            });
        }
        capture.Close();
        const auto final = Status();
        owned = encoder->CreatedFile();
        if (final.state == State::Discarding) {
            encoder.reset();
            if (owned && !DeleteFileW(temporary.c_str()))
                winrt::throw_last_error();
            owned = false;
            outcome = L"Recording discarded.";
            success = true;
        } else if (!havePending) {
            encoder.reset();
            if (owned && !DeleteFileW(temporary.c_str()))
                winrt::throw_last_error();
            owned = false;
            outcome = L"Stopped before the first frame. No file was saved.";
            success = true;
        } else {
            encoder->Write(pending.data(), pendingTime, (std::max)(int64_t(1), final.duration - pendingTime));
            {
                std::lock_guard lock(m_mutex);
                ++m_status.frames;
            }
            encoder->Finalize();
            encoder.reset();
            // Cancellation may race Finalize. Recheck before publishing the completed file.
            bool discard;
            {
                std::lock_guard lock(m_mutex);
                discard = m_timeline.state == State::Discarding;
                if (!discard)
                    m_timeline.Publish();
            } // Never hold the UI/state mutex during filesystem operations.
            if (discard) {
                if (!DeleteFileW(temporary.c_str()))
                    winrt::throw_last_error();
                outcome = L"Recording discarded.";
            } else {
                // Never overwrite a different file that appeared while recording.
                if (!MoveFileExW(temporary.c_str(), m_output.c_str(), MOVEFILE_WRITE_THROUGH))
                    winrt::throw_last_error();
                outcome = L"Saved: " + m_output;
            }
            owned = false;
            success = true;
        }
    } catch (const winrt::hresult_error &e) {
        outcome = L"Recording failed (0x";
        wchar_t hex[16]{};
        swprintf_s(hex, L"%08X", unsigned(e.code().value));
        outcome += hex;
        outcome += L"): ";
        outcome += e.message();
    } catch (...) {
        outcome = L"Recording failed: resource allocation or capture/encoder error.";
    }
    capture.Close();
    owned = owned || (encoder && encoder->CreatedFile());
    encoder.reset();
    if (owned) {
        const auto state = Status().state;
        if (state == State::Discarding && DeleteFileW(temporary.c_str()))
            outcome = L"Recording discarded.";
        else
            outcome += L"\nPartial file (may be unplayable): " + temporary;
    } else if (Status().state == State::Discarding && !success) {
        outcome = L"Recording discarded.";
        success = true;
    }
    if (apartment)
        winrt::uninit_apartment();
    // Last action: completion is published only after ALL capture/encoder/COM resources are gone.
    std::lock_guard lock(m_mutex);
    m_status.message = std::move(outcome);
    if (m_timeline.state == State::Recording)
        m_timeline.Pause(Now());
    m_timeline.Complete(success);
}
} // namespace nskry::record
