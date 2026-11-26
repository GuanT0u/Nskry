#include "core/plugin_manager.h"
#include "sdk/nskry_text_validation.h"
#include <array>
#include <thread>

namespace nskry {
int32_t PluginManager::AcquireOcr(const wchar_t *preferredId, NskryOcrLeaseV1 *lease) {
    if (!lease) return NSKRY_OCR_INVALID;
    *lease = {};
    if (!m_initialized || GetCurrentThreadId() != m_uiThread || !m_registry) return NSKRY_OCR_UNAVAILABLE;
    const auto *record = m_registry->ResolveOcr(preferredId);
    if (!record || !record->enabled || record->source == PluginSource::Legacy ||
        record->manifest.ocrLayoutApi != 1 || !record->manifest.IsCompatibleWithHost()) return NSKRY_OCR_UNAVAILABLE;
    const auto path = record->installPath + L"/" + record->manifest.entry;
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return NSKRY_OCR_UNAVAILABLE;
    const auto info = reinterpret_cast<decltype(&nskry_plugin_info)>(GetProcAddress(module, "nskry_plugin_info"));
    const auto recognize = reinterpret_cast<NskryRecognizeLayoutV1>(GetProcAddress(module, "nskry_plugin_ocr_layout_v1"));
    try {
        const auto *meta = info ? info() : nullptr;
        if (recognize && meta && meta->structSize >= sizeof(*meta) && meta->apiVersion == 2 &&
            meta->id && record->manifest.id == meta->id && meta->version && record->manifest.version == meta->version) {
            *lease = {module, recognize};
            return NSKRY_OCR_OK;
        }
    } catch (...) {}
    FreeLibrary(module);
    return NSKRY_OCR_UNAVAILABLE;
}

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
// API 0.6.x OCR acted as a v1 provider. Retain its identifier for legacy clients
// only while its replacement feature is installed and enabled.
static const wchar_t *LegacyImageProvider(const PluginRegistry &registry, const wchar_t *id) {
    if (id && !wcscmp(id, L"nskry-ocr")) {
        const auto *feature = registry.Find(id);
        if (feature && feature->enabled && feature->manifest.ocrLayoutClientApi && !feature->manifest.imageRecognitionApi)
            return L"nskry-ocr-provider";
    }
    return id;
}
bool PluginManager::HasImageRecognition(const wchar_t *id) const {
    if (GetCurrentThreadId() != m_uiThread.load() || !m_initialized || !m_registry || !id)
        return false;
    try {
        const auto *record = m_registry->Find(LegacyImageProvider(*m_registry, id));
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
    id = LegacyImageProvider(*m_registry, id);
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
