#include "record_audio.h"
#include "mf_encoder.h"
#include <algorithm>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

namespace nskry::record {
namespace {
struct Activation : winrt::implements<Activation, IActivateAudioInterfaceCompletionHandler> {
    Activation() : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!done)
            winrt::throw_last_error();
    }
    ~Activation() { CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) noexcept override {
        winrt::com_ptr<IUnknown> result;
        HRESULT activationResult{};
        status = operation->GetActivateResult(&activationResult, result.put());
        if (SUCCEEDED(status))
            status = activationResult;
        if (SUCCEEDED(status))
            status = result->QueryInterface(IID_PPV_ARGS(client.put()));
        SetEvent(done);
        return S_OK;
    }
    HANDLE done;
    HRESULT status = E_PENDING;
    winrt::com_ptr<IAudioClient> client;
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    PROPVARIANT value{};
};
winrt::com_ptr<IAudioClient> OpenProcess(const AudioSource &source) {
    winrt::handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, source.pid));
    FILETIME created{}, exited{}, kernel{}, user{};
    ULARGE_INTEGER creation{};
    if (!process || !GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;
    if (creation.QuadPart != source.processCreated || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"The selected audio application has exited.");
    auto callback = winrt::make_self<Activation>();
    callback->params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    callback->params.ProcessLoopbackParams = {source.pid, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE};
    callback->value.vt = VT_BLOB;
    callback->value.blob = {sizeof(callback->params), reinterpret_cast<BYTE *>(&callback->params)};
    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    winrt::check_hresult(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                     &callback->value, callback.get(), operation.put()));
    // The parent owns the bounded operation timeout and can terminate this private worker.
    if (WaitForSingleObject(callback->done, INFINITE) != WAIT_OBJECT_0)
        winrt::throw_last_error();
    winrt::check_hresult(callback->status);
    return callback->client;
}
}
LoopbackAudio::LoopbackAudio(const AudioSource &source) {
    if (source.pid) {
        try {
            m_client = OpenProcess(source);
        } catch (const winrt::hresult_error &e) {
            throw winrt::hresult_error(e.code(), L"Could not capture application audio. Process loopback requires Windows build 20348 or later and a running application. " + e.message());
        }
    } else {
        auto devices = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        winrt::com_ptr<IMMDevice> endpoint;
        winrt::check_hresult(devices->GetDevice(source.endpoint.c_str(), endpoint.put()));
        winrt::check_hresult(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, m_client.put_void()));
    }
    WAVEFORMATEX pcm{WAVE_FORMAT_PCM, 2, AudioRate, AudioRate * AudioBlock, AudioBlock, 16, 0};
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    winrt::check_hresult(m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &pcm, nullptr));
    winrt::check_hresult(m_client->GetService(IID_PPV_ARGS(m_capture.put())));
}
LoopbackAudio::~LoopbackAudio() {
    if (m_running)
        m_client->Stop();
}
void LoopbackAudio::Stop() {
    winrt::check_hresult(m_client->Stop());
    m_running = false;
    winrt::check_hresult(m_client->Reset());
}
void LoopbackAudio::Pump(MfEncoder &encoder, int64_t origin, int64_t duration, bool stop) {
    if (m_running && m_origin != origin)
        Stop(); // A pause/resume may have occurred between two video ticks.
    if (!m_running && !stop) {
        winrt::check_hresult(m_client->Start());
        m_running = true;
        m_origin = origin;
    }
    if (!m_running)
        return;
    const auto limit = AudioFrames(duration);
    for (unsigned packet = 0; packet < 64; ++packet) {
        UINT32 available{};
        winrt::check_hresult(m_capture->GetNextPacketSize(&available));
        if (!available)
            break;
        BYTE *bytes{};
        UINT32 frames{};
        DWORD flags{};
        UINT64 qpc{};
        winrt::check_hresult(m_capture->GetBuffer(&bytes, &frames, &flags, nullptr, &qpc));
        bool defer = false;
        try {
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc <= INT64_MAX) {
                const auto first = AudioFrames(static_cast<int64_t>(qpc) - origin);
                if (!stop && first + frames > limit) {
                    // Keep the entire packet for the next pump, without losing its tail.
                    defer = true;
                }
                const auto skip = (std::max)(int64_t(0), -first);
                const auto count = (std::min)(int64_t(frames) - skip, limit - first - skip);
                if (!defer && count > 0) {
                    const auto *pcm = flags & AUDCLNT_BUFFERFLAGS_SILENT ? nullptr : bytes + skip * AudioBlock;
                    encoder.WriteAudio(pcm, UINT32(count), first + skip);
                }
            }
        } catch (...) {
            m_capture->ReleaseBuffer(frames);
            throw;
        }
        winrt::check_hresult(m_capture->ReleaseBuffer(defer ? 0 : frames));
        if (defer)
            break;
    }
    // Keep the silent gap bounded, leaving time for delayed device packets.
    const auto pad = stop ? limit : AudioFrames((std::max)(int64_t(0), duration - 2500000));
    encoder.WriteAudio(nullptr, 0, pad);
    if (stop)
        Stop();
}
}
