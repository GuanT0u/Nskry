#include "mf_encoder.h"
#include "pixel_conversion.h"
#include "record_policy.h"
#include <algorithm>
#include <codecapi.h>
#include <cstring>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

namespace nskry::record {
MfEncoder::MfEncoder() {
    winrt::check_hresult(MFStartup(MF_VERSION));
}
MfEncoder::~MfEncoder() {
    ClosePipeline();
    MFShutdown();
}
void MfEncoder::ClosePipeline() noexcept {
    m_writer = nullptr;
    m_file = nullptr;
}
void MfEncoder::SetFormat(UINT width, UINT height, UINT fps) {
    if (m_writer || m_file || m_ownsFile || m_finalizationAttempted || !ValidFrameFormat(width, height, fps))
        winrt::throw_hresult(E_INVALIDARG);
    m_width = width;
    m_height = height;
    m_fps = fps;
}
void MfEncoder::OpenStream(IMFByteStream *stream, UINT width, UINT height, UINT fps) {
    if (!stream)
        winrt::throw_hresult(E_INVALIDARG);
    SetFormat(width, height, fps);
    m_file.copy_from(stream);
    Configure(false);
}
void MfEncoder::Open(const std::wstring &path, UINT width, UINT height, UINT fps, bool hardware,
                     volatile LONG *created, uint32_t bitrate, bool audio) {
    m_audio = audio;
    if (bitrate && (bitrate < 500000 || bitrate > 24000000))
        winrt::throw_hresult(E_INVALIDARG);
    m_bitrate = bitrate;
    SetFormat(width, height, fps);
    m_path = path;
    // FAIL_IF_EXIST protects unrelated files, including previous partial recordings.
    winrt::check_hresult(
        MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, path.c_str(), m_file.put()));
    m_ownsFile = true;
    if (created)
        InterlockedExchange(created, 1);
    try {
        Configure(hardware);
    } catch (...) {
        if (!hardware)
            throw;
        ClosePipeline();
        // This is exactly the file this encoder created, never the requested final output.
        if (!DeleteFileW(m_path.c_str()))
            winrt::throw_last_error();
        m_ownsFile = false;
        if (created)
            InterlockedExchange(created, 0);
        winrt::check_hresult(MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE,
                                          path.c_str(), m_file.put()));
        m_ownsFile = true;
        if (created)
            InterlockedExchange(created, 1);
        Configure(false);
        m_backend = L"Software H.264 (hardware initialization failed)";
    }
}
void MfEncoder::Configure(bool hardware) {
    winrt::com_ptr<IMFAttributes> attributes;
    winrt::check_hresult(MFCreateAttributes(attributes.put(), 4));
    winrt::check_hresult(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    winrt::check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware));
    winrt::check_hresult(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
    winrt::check_hresult(MFCreateSinkWriterFromURL(nullptr, m_file.get(), attributes.get(), m_writer.put()));
    winrt::com_ptr<IMFMediaType> output, input;
    winrt::check_hresult(MFCreateMediaType(output.put()));
    winrt::check_hresult(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    // Match the actual SDR integer NV12 conversion, including full color
    // description: incomplete descriptions may be omitted from MP4 by MF.
    winrt::check_hresult(output->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    winrt::check_hresult(output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    winrt::check_hresult(output->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    winrt::check_hresult(output->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    const auto bitrate = std::clamp(uint64_t(m_width) * m_height * m_fps / 6, uint64_t(500000), uint64_t(8000000));
    winrt::check_hresult(output->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate ? m_bitrate : UINT32(bitrate)));
    winrt::check_hresult(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    winrt::check_hresult(MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, m_width, m_height));
    winrt::check_hresult(MFSetAttributeRatio(output.get(), MF_MT_FRAME_RATE, m_fps, 1));
    winrt::check_hresult(MFSetAttributeRatio(output.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    winrt::check_hresult(m_writer->AddStream(output.get(), &m_stream));
    winrt::check_hresult(MFCreateMediaType(input.put()));
    winrt::check_hresult(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    winrt::check_hresult(input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    winrt::check_hresult(input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    winrt::check_hresult(input->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    winrt::check_hresult(input->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    winrt::check_hresult(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    winrt::check_hresult(MFSetAttributeSize(input.get(), MF_MT_FRAME_SIZE, m_width, m_height));
    winrt::check_hresult(MFSetAttributeRatio(input.get(), MF_MT_FRAME_RATE, m_fps, 1));
    winrt::check_hresult(MFSetAttributeRatio(input.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    winrt::check_hresult(m_writer->SetInputMediaType(m_stream, input.get(), nullptr));
    // Optional system encoder setting: do not let software fallback select all CPU cores.
    // Drivers may not expose this; this does not cap Windows' internal helper pools.
    winrt::com_ptr<ICodecAPI> codec;
    if (SUCCEEDED(m_writer->GetServiceForStream(m_stream, GUID_NULL, __uuidof(ICodecAPI), codec.put_void()))) {
        VARIANT count{};
        count.vt = VT_UI4;
        count.ulVal = 2;
        if (codec->IsSupported(&CODECAPI_AVEncNumWorkerThreads) == S_OK)
            codec->SetValue(&CODECAPI_AVEncNumWorkerThreads, &count);
    }
    if (m_audio) {
        const auto audioType = [](GUID subtype, UINT32 bytesPerSecond) {
            winrt::com_ptr<IMFMediaType> type;
            winrt::check_hresult(MFCreateMediaType(type.put()));
            winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
            winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE, subtype));
            winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
            winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
            winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioRate));
            winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond));
            return type;
        };
        auto aac = audioType(MFAudioFormat_AAC, 16000);
        auto pcm = audioType(MFAudioFormat_PCM, AudioRate * AudioBlock);
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AudioBlock));
        winrt::check_hresult(m_writer->AddStream(aac.get(), &m_audioStream));
        winrt::check_hresult(m_writer->SetInputMediaType(m_audioStream, pcm.get(), nullptr));
    }
    winrt::check_hresult(m_writer->BeginWriting());
    DescribeBackend();
    if (!hardware)
        m_backend = L"Software H.264";
}
void MfEncoder::DescribeBackend() {
    m_backend = L"OS-selected H.264 (hardware not confirmed)";
    auto extended = m_writer.try_as<IMFSinkWriterEx>();
    if (!extended)
        return;
    for (DWORD i = 0; i < 8; ++i) {
        GUID category{};
        winrt::com_ptr<IMFTransform> transform;
        if (FAILED(extended->GetTransformForStream(m_stream, i, &category, transform.put())))
            break;
        if (category != MFT_CATEGORY_VIDEO_ENCODER)
            continue;
        winrt::com_ptr<IMFAttributes> attrs;
        if (SUCCEEDED(transform->GetAttributes(attrs.put()))) {
            UINT32 length{};
            if (SUCCEEDED(attrs->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length)) && length) {
                m_backend = L"Hardware H.264 (active encoder confirmed)";
                return;
            }
        }
        // Hardware flag alone is not evidence. Unknown drivers stay explicitly unconfirmed.
    }
}
void MfEncoder::Write(const uint8_t *bgra, int64_t time, int64_t duration) {
    WritePixels(bgra, time, duration, false);
}
void MfEncoder::WriteNv12(const uint8_t *pixels, int64_t time, int64_t duration) {
    WritePixels(pixels, time, duration, true);
}
void MfEncoder::WritePixels(const uint8_t *pixels, int64_t time, int64_t duration, bool nv12) {
    if (!m_writer || m_finalizationAttempted || !pixels || !ValidSampleTime(time, duration, m_lastEnd))
        winrt::throw_hresult(E_INVALIDARG);
    winrt::com_ptr<IMFSample> sample;
    winrt::com_ptr<IMFMediaBuffer> buffer;
    const DWORD length = m_width * m_height * 3 / 2;
    winrt::check_hresult(MFCreateMemoryBuffer(length, buffer.put()));
    BYTE *bytes{};
    winrt::check_hresult(buffer->Lock(&bytes, nullptr, nullptr));
    if (nv12)
        std::memcpy(bytes, pixels, length);
    else
        BgraToNv12(pixels, bytes, m_width, m_height);
    winrt::check_hresult(buffer->Unlock());
    winrt::check_hresult(buffer->SetCurrentLength(length));
    winrt::check_hresult(MFCreateSample(sample.put()));
    winrt::check_hresult(sample->AddBuffer(buffer.get()));
    winrt::check_hresult(sample->SetSampleTime(time));
    winrt::check_hresult(sample->SetSampleDuration(duration));
    winrt::check_hresult(m_writer->WriteSample(m_stream, sample.get()));
    ++m_samples;
    m_lastEnd = time + duration;
}
void MfEncoder::WriteAudio(const uint8_t *pcm, uint32_t frames, int64_t first) {
    if (!m_audio || !m_writer || m_finalizationAttempted || first < 0 || first > INT64_MAX / 1000)
        winrt::throw_hresult(E_INVALIDARG);
    const auto end = first + frames;
    while (m_audioFrames < end) {
        const bool gap = m_audioFrames < first;
        const auto count = (std::min)(int64_t(4096), (gap ? first : end) - m_audioFrames);
        const auto size = DWORD(count * AudioBlock);
        winrt::com_ptr<IMFMediaBuffer> buffer;
        winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(MFCreateMemoryBuffer(size, buffer.put()));
        BYTE *data{};
        winrt::check_hresult(buffer->Lock(&data, nullptr, nullptr));
        if (gap || !pcm)
            memset(data, 0, size);
        else
            memcpy(data, pcm + (m_audioFrames - first) * AudioBlock, size);
        winrt::check_hresult(buffer->Unlock());
        winrt::check_hresult(buffer->SetCurrentLength(size));
        winrt::check_hresult(MFCreateSample(sample.put()));
        winrt::check_hresult(sample->AddBuffer(buffer.get()));
        const auto time = AudioTime(m_audioFrames);
        winrt::check_hresult(sample->SetSampleTime(time));
        winrt::check_hresult(sample->SetSampleDuration(AudioTime(m_audioFrames + count) - time));
        winrt::check_hresult(m_writer->WriteSample(m_audioStream, sample.get()));
        m_audioFrames += count;
    }
}
void MfEncoder::Finalize() {
    if (!m_writer || !m_samples || m_finalizationAttempted)
        winrt::throw_hresult(MF_E_INVALIDREQUEST);
    if (m_audio)
        WriteAudio(nullptr, 0, AudioFrames(m_lastEnd));
    m_finalizationAttempted = true;
    auto hr = m_writer->Finalize();
    if (FAILED(hr))
        throw winrt::hresult_error(hr, L"MP4 sink Finalize failed");
    // The MP4 sink owns the byte stream's close/flush protocol. After Finalize
    // it may already be closed; Flush on that stream returns E_INVALIDARG.
    ClosePipeline();
    m_finalized = true;
}
} // namespace nskry::record
