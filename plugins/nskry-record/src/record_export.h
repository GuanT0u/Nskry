#pragma once
#include "record_session.h"
#include <mutex>
#include <thread>
namespace nskry::record {
struct ExportStatus {
    bool done = false, success = false;
    std::wstring message;
};
class ExportTask {
  public:
    ExportTask();
    ~ExportTask();
    void Start(std::wstring input, std::wstring output, Snapshot source, bool gif, int64_t start, int64_t end,
               bool trim);
    void Cancel();
    void Shutdown(); // App exit only: cancel and join before DLL unload.
    ExportStatus Status() const;

  private:
    HANDLE m_cancel{};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    ExportStatus m_status;
};
} // namespace nskry::record
