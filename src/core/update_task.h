#pragma once
#include "core/plugin_update_policy.h"
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
namespace nskry {
struct PreparedPluginPackage;
struct DownloadedUpdate {
    std::wstring path;
    std::shared_ptr<PreparedPluginPackage> package;
    ~DownloadedUpdate() {
        if (!path.empty())
            DeleteFileW(path.c_str());
    }
};
struct UpdateTaskResult {
    std::vector<PluginUpdate> updates;
    std::shared_ptr<DownloadedUpdate> download;
    std::wstring message;
    bool cancelled = false;
};
// UI-thread owner. No thread/window until Start; worker receives values only.
class UpdateTask {
  public:
    using Work = std::function<UpdateTaskResult(std::stop_token)>;
    using Completion = std::function<void(UpdateTaskResult)>;
    ~UpdateTask();
    bool Start(Work work, Completion completion);
    void Cancel();
    void Shutdown();
    bool Busy() const {
        return m_thread.joinable();
    }

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void Complete();
    HWND m_window{};
    std::thread m_thread;
    std::stop_source m_stop{std::nostopstate};
    UpdateTaskResult m_result;
    Completion m_completion;
    WPARAM m_generation{};
};
} // namespace nskry
