#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
namespace nskry::record {
struct PreviewFrame {
    UINT width{}, height{};
    int64_t time{};
    std::vector<uint8_t> pixels;
    std::wstring error;
};
// Created only for an active seek gesture. Single latest request/result, <=8fps,
// one isolated decoder process. Finish drains the final seek then exits.
class PreviewTask {
  public:
    PreviewTask(std::wstring path, UINT sourceWidth, UINT sourceHeight, bool gif);
    ~PreviewTask();
    bool Request(int64_t time);
    void Finish();
    void Cancel();
    bool Done() const;
    bool Take(PreviewFrame &frame, bool &done);

  private:
    void Run(std::wstring path, UINT sourceWidth, UINT sourceHeight, bool gif) noexcept;
    HANDLE m_cancel{}, m_request{};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    int64_t m_time{};
    uint64_t m_serial{};
    bool m_finishing = false, m_done = false, m_ready = false;
    PreviewFrame m_frame;
};
} // namespace nskry::record
