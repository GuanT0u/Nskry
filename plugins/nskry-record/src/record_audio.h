#pragma once
#include "record_audio_source.h"
#include <audioclient.h>
#include <winrt/base.h>

namespace nskry::record {
class MfEncoder;
class LoopbackAudio {
  public:
    explicit LoopbackAudio(const AudioSource &source);
    ~LoopbackAudio();
    void Pump(MfEncoder &encoder, int64_t origin, int64_t duration, bool stop);

  private:
    void Stop();
    winrt::com_ptr<IAudioClient> m_client;
    winrt::com_ptr<IAudioCaptureClient> m_capture;
    bool m_running{};
    int64_t m_origin{};
};
}
