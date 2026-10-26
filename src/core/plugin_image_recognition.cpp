#include "core/plugin_manager.h"
#include "sdk/nskry_text_validation.h"
#include <array>
#include <thread>

namespace nskry {
struct PluginManager::ImageJob {
    HBITMAP bitmap{};
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    std::atomic<bool> done{false};
    int32_t status = NSKRY_IMAGE_FAILED;
    std::array<wchar_t, NSKRY_IMAGE_MAX_TEXT + 1> text{};
    std::array<wchar_t, 256> error{};
    uint32_t length{};
    ~ImageJob() {
        if (cancel)
            SetEvent(cancel);
        if (worker.joinable())
            worker.join();
        if (cancel)
            CloseHandle(cancel);
        if (bitmap)
            DeleteObject(bitmap);
    }
};
bool PluginManager::HasImageRecognition(const wchar_t *id) const {
    if (GetCurrentThreadId() != m_uiThread.load() || !m_initialized || !m_registry || !id)
        return false;
    try {
        const auto *record = m_registry->Find(id);
        return record && record->enabled && record->manifest.IsCompatibleWithHost() &&
               record->manifest.imageRecognitionApi == 1;
    } catch (...) {
        return false;
    }
}
int32_t PluginManager::BeginImageRecognition(const wchar_t *id, HBITMAP bitmap, uint64_t *ticket) {
    if (GetCurrentThreadId() != m_uiThread.load())
        return NSKRY_IMAGE_UNAVAILABLE;
    if (!ticket)
        return NSKRY_IMAGE_INVALID;
    *ticket = 0;
    if (!HasImageRecognition(id))
        return NSKRY_IMAGE_UNAVAILABLE;
    if (m_imageJob)
        return NSKRY_IMAGE_BUSY; // Bounded: one job, no waiting queue.
    BITMAP info{};
    if (!bitmap || GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) || info.bmWidth < 1 ||
        info.bmHeight < 1 || info.bmWidth > 4096 || info.bmHeight > 4096 ||
        uint64_t(info.bmWidth) * info.bmHeight > 4194304)
        return NSKRY_IMAGE_INVALID;
    try {
        if (!EnsureLoaded(id))
            return NSKRY_IMAGE_UNAVAILABLE;
        const auto fn = m_plugins.at(id).fnRecognizeImage;
        if (!fn)
            return NSKRY_IMAGE_UNAVAILABLE;
        auto job = std::make_shared<ImageJob>();
        job->bitmap = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        if (!job->cancel || !job->bitmap)
            return NSKRY_IMAGE_FAILED;
        m_imageProvider = id;
        // No Registry/UI/plugin-lifecycle operations on this thread. The DLL
        // stays loaded until release/shutdown joins this owned worker.
        job->worker = std::thread([state = job.get(), fn, width = info.bmWidth, height = info.bmHeight] {
            const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                NskryImageInputV1 input{sizeof(input), 1, state->bitmap, width, height, state->cancel};
                NskryImageTextV1 output{sizeof(output),
                                        1,
                                        state->text.data(),
                                        uint32_t(state->text.size()),
                                        0,
                                        state->error.data(),
                                        uint32_t(state->error.size())};
                if (FAILED(com))
                    state->status = NSKRY_IMAGE_FAILED;
                else
                    state->status = fn(&input, &output);
                if (state->status == NSKRY_IMAGE_SUCCESS &&
                    NskryValidText(state->text.data(), output.textLength, NSKRY_IMAGE_MAX_TEXT) &&
                    !state->text[output.textLength])
                    state->length = output.textLength;
                else if (state->status != NSKRY_IMAGE_CANCELED)
                    state->status = NSKRY_IMAGE_FAILED;
            } catch (...) {
                state->status = NSKRY_IMAGE_FAILED;
            }
            state->error.back() = 0;
            if (SUCCEEDED(com))
                CoUninitialize();
            state->done.store(true);
        });
        m_imageJob = std::move(job);
        if (++m_imageTicket == 0)
            ++m_imageTicket;
        *ticket = m_imageTicket;
        return NSKRY_IMAGE_PENDING;
    } catch (...) {
        m_imageProvider.clear();
        return NSKRY_IMAGE_FAILED;
    }
}
int32_t PluginManager::PollImageRecognition(uint64_t ticket, NskryImageTextV1 *out) {
    if (GetCurrentThreadId() != m_uiThread.load())
        return NSKRY_IMAGE_UNAVAILABLE;
    if (!NskryValidImageText(out))
        return NSKRY_IMAGE_INVALID;
    out->text[0] = out->error[0] = 0;
    out->textLength = 0;
    if (!m_imageJob || !ticket || ticket != m_imageTicket)
        return NSKRY_IMAGE_UNAVAILABLE;
    if (!m_imageJob->done.load())
        return NSKRY_IMAGE_PENDING;
    if (WaitForSingleObject(m_imageJob->cancel, 0) == WAIT_OBJECT_0)
        return NSKRY_IMAGE_CANCELED;
    out->textLength = m_imageJob->length;
    lstrcpynW(out->text, m_imageJob->text.data(), out->capacity);
    lstrcpynW(out->error, m_imageJob->error.data(), out->errorCapacity);
    return m_imageJob->status;
}
void PluginManager::CancelImageRecognition(uint64_t ticket) {
    if (GetCurrentThreadId() == m_uiThread.load() && m_imageJob && ticket == m_imageTicket)
        SetEvent(m_imageJob->cancel);
}
void PluginManager::ReleaseImageRecognition(uint64_t ticket) {
    if (GetCurrentThreadId() != m_uiThread.load() || !m_imageJob || ticket != m_imageTicket)
        return;
    m_imageJob.reset(); // cancel + join; completed tasks return immediately.
    m_imageProvider.clear();
}
} // namespace nskry
