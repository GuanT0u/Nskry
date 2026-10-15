#pragma once
#include "nskry_capture.h"
#include "record_policy.h"
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
};
// UI owns this object and its thread; worker owns capture/encoder/COM/files.
// UI never joins until Snapshot says finished. Shutdown cancels and joins before DLL unload.
class RecordSession {
  public:
    RecordSession(NskryCaptureServiceV1 service, RECT region, std::wstring output, bool preferHardware = true);
    ~RecordSession();
    bool Start();
    bool PauseResume();
    bool Stop(bool discard);
    void Shutdown();
    Snapshot Status() const;
    static int64_t Now();

  private:
    void Run() noexcept;
    NskryCaptureServiceV1 m_service;
    RECT m_region{};
    std::wstring m_output;
    bool m_preferHardware = true;
    mutable std::mutex m_mutex;
    HANDLE m_wakeEvent{}; // One on-demand command signal; closed after joining.
    Timeline m_timeline;
    std::thread m_worker;
    Snapshot m_status;
};
} // namespace nskry::record
