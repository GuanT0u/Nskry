#include "record_session.h"
#include "isolated_encoder.h"
#include "record_delivery.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <shellscalingapi.h>
#include <vector>
#include <winrt/base.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace nskry::record {
namespace {
bool RemoveTemporary(const std::wstring &path) {
    if (DeleteFileW(path.c_str()))
        return true;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
} // namespace
int64_t RecordSession::Now() {
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
RecordSession::RecordSession(NskryCaptureServiceV1 service, RECT region, std::wstring output, bool preferHardware,
                             SessionOptions options)
    : m_service(service), m_region(region), m_output(std::filesystem::absolute(output).wstring()),
      m_preferHardware(preferHardware), m_options(options) {
    m_status.region = region;
}
RecordSession::~RecordSession() {
    Shutdown();
}
bool RecordSession::Start() {
    std::lock_guard lock(m_mutex);
    if (!m_timeline.Start())
        return false;
    try {
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_wakeEvent || !m_cancelEvent)
            winrt::throw_last_error();
        m_worker = std::thread([this] { Run(); });
    } catch (...) {
        if (m_wakeEvent) {
            CloseHandle(m_wakeEvent);
            m_wakeEvent = nullptr;
        }
        if (m_cancelEvent) {
            CloseHandle(m_cancelEvent);
            m_cancelEvent = nullptr;
        }
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
        SetEvent(m_wakeEvent);
    return changed;
}
bool RecordSession::Stop(bool discard) {
    std::lock_guard lock(m_mutex);
    const auto changed = m_timeline.Stop(Now(), discard);
    if (changed) {
        SetEvent(m_wakeEvent);
        if (discard)
            SetEvent(m_cancelEvent);
    }
    return changed;
}
void RecordSession::Shutdown() {
    Stop(true);
    if (m_worker.joinable())
        m_worker.join();
    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    if (m_cancelEvent) {
        CloseHandle(m_cancelEvent);
        m_cancelEvent = nullptr;
    }
}
Snapshot RecordSession::Status() const {
    std::lock_guard lock(m_mutex);
    auto copy = m_status;
    copy.state = m_timeline.state;
    copy.duration = m_timeline.Elapsed(Now());
    return copy;
}
void RecordSession::Run() noexcept {
    const UINT fps = m_options.fps;
    // Use one transport for the complete session; never change capture semantics
    // silently when a source closes or is resized/minimized.
    const bool v2 = m_options.capture.version == 2;
    if (v2)
        m_service = {sizeof(m_service), 1, nullptr, m_options.capture.read, m_options.capture.close};
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
    std::unique_ptr<IsolatedEncoder> encoder;
    std::wstring temporary;
    bool apartment = false, owned = false, success = false, finalizedFile = false, encoderExited = true;
    std::wstring outcome;
    const auto releaseEncoder = [&] {
        if (encoder) {
            encoderExited = encoder->Close();
            // Ownership can become visible during failed/canceled Open. Read it
            // AFTER child shutdown, before deleting or reporting a partial.
            owned = owned || encoder->CreatedFile();
            finalizedFile = finalizedFile || encoder->Finalized();
            const auto cpu = encoder->CpuTime();
            encoder.reset();
            std::lock_guard lock(m_mutex);
            m_status.encoderCpu100ns = cpu;
        }
        return encoderExited;
    };
    try {
        if (fps < 1 || fps > 60 || (!v2 && (fps > 30 || m_options.followWindow)) ||
            (v2 && (!m_options.capture.open || !m_options.capture.read || !m_options.capture.close ||
                    !m_options.capture.bounds)))
            winrt::throw_hresult(E_INVALIDARG);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (m_options.deliveryCache) PrepareRecordingCache(m_output, m_cancelEvent);
        // Local, on-demand pacing only. No global timeBeginPeriod or busy-spin.
        struct Timer {
            HANDLE value{};
            ~Timer() {
                if (value)
                    CloseHandle(value);
            }
        } timer;
        timer.value = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer.value)
            timer.value = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!timer.value)
            winrt::throw_last_error();
        GUID id{};
        winrt::check_hresult(CoCreateGuid(&id));
        wchar_t guid[40]{};
        if (!StringFromGUID2(id, guid, 40))
            winrt::throw_hresult(E_FAIL);
        const auto slash = m_output.find_last_of(L"\\/");
        temporary = m_output.substr(0, slash + 1) + L".nskry-record-" + guid +
                    (m_options.gif ? L".partial.gif" : L".partial.mp4");
        NskryCaptureDescriptionV1 description{sizeof(description), 1};
        wchar_t error[512]{};
        if (Status().state == State::Discarding)
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        RECT originalWindow{};
        DWORD sourcePid{};
        DWORD sourceThread{};
        if (m_options.followWindow) {
            sourceThread = GetWindowThreadProcessId(m_options.followWindow, &sourcePid);
            winrt::check_hresult(DwmGetWindowAttribute(m_options.followWindow, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                       &originalWindow, sizeof(originalWindow)));
        }
        const auto openCapture = [&] {
            if (!v2)
                return m_service.open(m_region, fps, &capture.value, &description, error, 512);
            RECT region = m_region;
            if (m_options.followWindow) {
                RECT now{};
                DWORD pid{};
                if (GetWindowThreadProcessId(m_options.followWindow, &pid) != sourceThread || pid != sourcePid ||
                    FAILED(DwmGetWindowAttribute(m_options.followWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &now, sizeof(now))) ||
                    now.right - now.left != originalWindow.right - originalWindow.left ||
                    now.bottom - now.top != originalWindow.bottom - originalWindow.top)
                    return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
                OffsetRect(&region, now.left - originalWindow.left, now.top - originalWindow.top);
            }
            NskryCaptureRequestV2 request{sizeof(request), 2, region, fps, m_options.followWindow,
                                           m_options.gif ? 960u : 1920u, m_options.gif ? 540u : 1080u};
            return m_options.capture.open(&request, &capture.value, &description, error, 512);
        };
        auto hr = openCapture();
        if (FAILED(hr))
            throw winrt::hresult_error(hr, error);
        const auto expected = description;
        if (description.structSize < sizeof(description) || description.version != 1 ||
            !ValidFrameFormat(description.width, description.height, fps) ||
            description.stride != description.width * 4)
            winrt::throw_hresult(E_BOUNDS);
        if (Status().state == State::Discarding)
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        std::vector<uint8_t> pixels(uint64_t(description.stride) * description.height);
        std::vector<uint8_t> pending(pixels.size());
        const uint32_t targetBitrate = m_options.gif ? 0
            : m_options.bitrate ? m_options.bitrate
            : v2 ? Bitrate(description.width, description.height, fps, m_options.quality)
                 : DefaultVideoBitrate(description.width, description.height, fps);
        encoder = std::make_unique<IsolatedEncoder>(m_cancelEvent);
        encoder->Open(temporary, description.width, description.height, fps, m_preferHardware,
                      targetBitrate, m_options.gif);
        {
            std::lock_guard lock(m_mutex);
            m_status.backend = encoder->Backend();
            m_status.width = description.width;
            m_status.height = description.height;
            m_status.fps = fps;
            m_status.bitrate = targetBitrate;
            m_status.encoderProcessId = encoder->ProcessId();
        }
        FrameSchedule schedule(fps);
        bool ready = false, haveFrame = false, havePending = false;
        int64_t pendingTime = 0;
        int64_t maxRead = 0;
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
            if (m_options.gif && snapshot.duration >= 119LL * 10000000) {
                Stop(false); // Bounded animation: at most two minutes, never grow a frame cache.
                snapshot = Status();
            }
            if (snapshot.state == State::Stopping || snapshot.state == State::Discarding)
                break;
            if (snapshot.state == State::Paused) {
                capture.Close(); // no WGC/GPU capture allocations or polling while paused
                CancelWaitableTimer(timer.value);
                if (WaitForSingleObject(m_wakeEvent, INFINITE) != WAIT_OBJECT_0)
                    winrt::throw_last_error();
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
                winrt::check_hresult(openCapture());
                if (description.structSize < sizeof(description) || description.version != expected.version ||
                    description.width != expected.width || description.height != expected.height ||
                    description.stride != expected.stride)
                    winrt::throw_hresult(E_BOUNDS);
                haveFrame = false;
                startupDeadline = Now() + 100000000;
            }
            if (schedule.Due(snapshot.duration) || !ready) {
                const auto readStarted = Now();
                const HRESULT read = m_service.read(capture.value, pixels.data(), uint32_t(pixels.size()));
                maxRead = (std::max)(maxRead, Now() - readStarted);
                winrt::check_hresult(read);
                if (v2) {
                    RECT bounds{};
                    winrt::check_hresult(m_options.capture.bounds(capture.value, &bounds));
                    std::lock_guard lock(m_mutex);
                    m_status.region = bounds;
                }
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
                        const auto encodeStarted = Now();
                        encoder->Write(pending.data(), pendingTime, at - pendingTime);
                        const auto encodeSpan = Now() - encodeStarted;
                        std::lock_guard lock(m_mutex);
                        ++m_status.frames;
                        m_status.encodedDuration = encoder->EncodedDuration();
                        m_status.maxRead100ns = maxRead;
                        m_status.maxEncode100ns = (std::max)(m_status.maxEncode100ns, encodeSpan);
                        if (at - pendingTime > schedule.interval * 2)
                            ++m_status.skipped;
                    }
                    if (read == S_OK)
                        pending.swap(pixels);
                    havePending = true;
                    pendingTime = at;
                    // On S_FALSE repeat pending directly. Scratch bytes are not
                    // a frame; don't copy 8 MB back every 1080p sampling tick.
                    schedule.Submitted(at);
                }
            }
            const auto elapsed = Status().duration;
            const auto delay =
                ready && haveFrame ? (std::max)(int64_t(10000), schedule.next - elapsed) : int64_t(100000);
            LARGE_INTEGER due{};
            due.QuadPart = -delay;
            if (!SetWaitableTimerEx(timer.value, &due, 0, nullptr, nullptr, nullptr, 0))
                winrt::throw_last_error();
            const HANDLE signals[]{m_wakeEvent, timer.value};
            const DWORD waited = WaitForMultipleObjects(2, signals, FALSE, INFINITE);
            if (waited == WAIT_FAILED)
                winrt::throw_last_error();
            if (waited == WAIT_OBJECT_0)
                CancelWaitableTimer(timer.value);
        }
        capture.Close();
        const auto final = Status();
        owned = encoder->CreatedFile();
        if (final.state == State::Discarding) {
            if (!releaseEncoder())
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Encoder process did not exit.");
            if (owned && !RemoveTemporary(temporary))
                winrt::throw_last_error();
            owned = false;
            outcome = L"Recording discarded.";
            success = true;
        } else if (!havePending) {
            if (!releaseEncoder())
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Encoder process did not exit.");
            if (owned && !RemoveTemporary(temporary))
                winrt::throw_last_error();
            owned = false;
            outcome = L"Stopped before the first frame. No file was saved.";
            success = true;
        } else {
            const auto encodeStarted = Now();
            encoder->Write(pending.data(), pendingTime, (std::max)(int64_t(1), final.duration - pendingTime));
            const auto encodeSpan = Now() - encodeStarted;
            {
                std::lock_guard lock(m_mutex);
                ++m_status.frames;
                m_status.encodedDuration = encoder->EncodedDuration();
                m_status.maxRead100ns = maxRead;
                m_status.maxEncode100ns = (std::max)(m_status.maxEncode100ns, encodeSpan);
            }
            encoder->Finalize();
            finalizedFile = true;
            if (!releaseEncoder())
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Encoder process did not exit.");
            // Cancellation may race Finalize. Recheck before publishing the completed file.
            bool discard;
            {
                std::lock_guard lock(m_mutex);
                discard = m_timeline.state == State::Discarding;
                if (!discard)
                    m_timeline.Publish();
            } // Never hold the UI/state mutex during filesystem operations.
            if (discard) {
                if (!RemoveTemporary(temporary))
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
        {
            std::lock_guard lock(m_mutex);
            m_timeline.BeginRecovery(Now());
        }
        outcome = L"Recording failed (0x";
        wchar_t hex[16]{};
        swprintf_s(hex, L"%08X", unsigned(e.code().value));
        outcome += hex;
        outcome += L"): ";
        outcome += e.message();
    } catch (...) {
        {
            std::lock_guard lock(m_mutex);
            m_timeline.BeginRecovery(Now());
        }
        outcome = L"Recording failed: resource allocation or capture/encoder error.";
    }
    capture.Close();
    owned = owned || (encoder && encoder->CreatedFile());
    // A capture/source failure must not automatically destroy otherwise healthy
    // encoded data. One best-effort finalize on the worker, never retry a failed
    // finalize or publish a truncated recording as a successful full recording.
    bool finalized = owned && (finalizedFile || (encoder && encoder->Finalized()));
    if (owned && encoder && encoder->Samples() && !encoder->FinalizationAttempted() &&
        Status().state != State::Discarding) {
        try {
            encoder->Finalize();
            finalized = true;
        } catch (const winrt::hresult_error &e) {
            wchar_t hex[32]{};
            swprintf_s(hex, L" (recovery: 0x%08X)", unsigned(e.code().value));
            outcome += hex;
        } catch (...) {
            outcome += L" (recovery failed)";
        }
    }
    releaseEncoder();
    if (!encoderExited) {
        outcome += L"\nEncoder process did not exit within the cleanup deadline.";
        success = false;
    }
    finalized = finalized || finalizedFile;
    if (owned) {
        bool retain;
        {
            std::lock_guard lock(m_mutex);
            retain = m_timeline.SealRecovery();
        }
        if (!retain && encoderExited && RemoveTemporary(temporary)) {
            outcome = L"Recording discarded.";
            success = true;
        } else {
            outcome += finalized ? L"\nCaptured portion finalized (incomplete recording): "
                                 : L"\nPartial file (may be unplayable): ";
            outcome += temporary;
            std::lock_guard lock(m_mutex);
            m_status.recoveryPath = temporary;
            m_status.recoveryFinalized = finalized;
        }
    } else if (Status().state == State::Discarding && !success && encoderExited) {
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
