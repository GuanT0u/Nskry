#pragma once
#include "nskry_capture.h"
#include "record_policy.h"
#include "record_options.h"
#include <mutex>
#include <string>
#include <thread>

namespace nskry::record {
struct Snapshot {
    State state{};
    int64_t duration{};
    uint64_t frames{}, skipped{};
    std::wstring backend, message;
    std::wstring recoveryPath;
    bool recoveryFinalized = false;
    int64_t encodedDuration{};
    DWORD encoderProcessId{};                 // Diagnostic only; not an ownership/control handle.
    uint64_t encoderCpu100ns{};               // Final child CPU total, includes child teardown.
    int64_t maxRead100ns{}, maxEncode100ns{}; // Wall spans, not CPU attribution.
    RECT region{};
    uint32_t width{}, height{}, fps{};
    uint32_t bitrate{};
};
struct SessionOptions {
    uint32_t fps = 20;
    uint32_t bitrate = 0; // 0 preserves the previous resolution-aware automatic target.
    NskryCaptureServiceV2 capture{};
    HWND followWindow{};
    bool gif = false;
    bool deliveryCache = false;
    Quality quality = Quality::High;
};
// UI owns this object and its thread; worker owns capture/encoder/COM/files.
// UI never joins until Snapshot says finished. Shutdown cancels and joins before DLL unload.
class RecordSession {
  public:
    RecordSession(NskryCaptureServiceV1 service, RECT region, std::wstring output, bool preferHardware = true,
                  SessionOptions options = {});
    ~RecordSession();
    bool Start();
    bool PauseResume();
    bool Stop(bool discard);
    void Shutdown();
    Snapshot Status() const;
    static int64_t Now();

  private:
    struct Timing {
        State state;
        int64_t duration;
    };
    // Sampling/pacing must not copy diagnostic strings on every frame.
    Timing ReadTiming() const;
    void Run() noexcept;
    NskryCaptureServiceV1 m_service;
    RECT m_region{};
    std::wstring m_output;
    bool m_preferHardware = true;
    SessionOptions m_options;
    mutable std::mutex m_mutex;
    HANDLE m_wakeEvent{};   // One on-demand command signal; closed after joining.
    HANDLE m_cancelEvent{}; // Manual-reset cancellation also interrupts encoder IPC.
    Timeline m_timeline;
    std::thread m_worker;
    Snapshot m_status;
};
} // namespace nskry::record
