#include "mf_encoder.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mfapi.h>
#include <mferror.h>
#include <mutex>

namespace {
struct QuotaState {
    std::atomic<bool> failIndex{false};
    std::atomic<unsigned> failures{}, alive{};
    std::atomic<uint64_t> limit{2 * 1024 * 1024};
};
// Real MF MP4 sink, but storage is test-owned and bounded. Never fills a real disk.
struct QuotaStream : winrt::implements<QuotaStream, IStream, ISequentialStream> {
    std::shared_ptr<QuotaState> state;
    winrt::com_ptr<IStream> bytes;
    std::mutex mutex;
    explicit QuotaStream(std::shared_ptr<QuotaState> value) : state(std::move(value)) {
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, bytes.put()));
        ++state->alive;
    }
    ~QuotaStream() {
        --state->alive;
    }
    HRESULT __stdcall Read(void *p, ULONG n, ULONG *done) noexcept override {
        std::lock_guard lock(mutex);
        return bytes->Read(p, n, done);
    }
    HRESULT __stdcall Write(const void *p, ULONG n, ULONG *done) noexcept override {
        std::lock_guard lock(mutex);
        if (done)
            *done = 0;
        ULARGE_INTEGER position{};
        LARGE_INTEGER zero{};
        const HRESULT hr = bytes->Seek(zero, STREAM_SEEK_CUR, &position);
        if (FAILED(hr))
            return hr;
        const auto limit = state->limit.load();
        // Fail the actual final MP4 index, not still-queued sample data. Arming
        // unconditional failure after WriteSample raced asynchronous MF writes
        // and could hang native Finalize rather than testing the intended index.
        const bool index = n >= 8 && !std::memcmp(static_cast<const uint8_t *>(p) + 4, "moov", 4);
        if ((state->failIndex && index) || position.QuadPart > limit || n > limit - position.QuadPart) {
            ++state->failures;
            return STG_E_MEDIUMFULL;
        }
        return bytes->Write(p, n, done);
    }
    HRESULT __stdcall Seek(LARGE_INTEGER to, DWORD origin, ULARGE_INTEGER *at) noexcept override {
        std::lock_guard lock(mutex);
        return bytes->Seek(to, origin, at);
    }
    HRESULT __stdcall SetSize(ULARGE_INTEGER size) noexcept override {
        std::lock_guard lock(mutex);
        if (size.QuadPart > state->limit) {
            ++state->failures;
            return STG_E_MEDIUMFULL;
        }
        return bytes->SetSize(size);
    }
    HRESULT __stdcall CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) noexcept override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall Commit(DWORD flags) noexcept override {
        std::lock_guard lock(mutex);
        return bytes->Commit(flags);
    }
    HRESULT __stdcall Revert() noexcept override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT __stdcall UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT __stdcall Stat(STATSTG *stat, DWORD flags) noexcept override {
        std::lock_guard lock(mutex);
        const HRESULT hr = bytes->Stat(stat, flags);
        if (SUCCEEDED(hr))
            stat->grfMode = STGM_READWRITE;
        return hr;
    }
    HRESULT __stdcall Clone(IStream **) noexcept override {
        return E_NOTIMPL;
    }
};
bool Run(bool failAtFinalize) {
    auto state = std::make_shared<QuotaState>();
    bool failed = false, finalAttempted = false, finalSucceeded = false;
    {
        nskry::record::MfEncoder encoder;
        auto stream = winrt::make_self<QuotaStream>(state);
        winrt::com_ptr<IMFByteStream> wrapped;
        winrt::check_hresult(MFCreateMFByteStreamOnStream(stream.get(), wrapped.put()));
        DWORD caps{};
        winrt::check_hresult(wrapped->GetCapabilities(&caps));
        std::cout << "Wrapped stream capabilities=" << caps << '\n';
        encoder.OpenStream(wrapped.get(), 320, 180, 20);
        std::cout << "stream_opened scenario=" << failAtFinalize << '\n';
        std::vector<uint8_t> pixels(320 * 180 * 4, 128);
        if (!failAtFinalize)
            state->limit = 256;
        try {
            for (int i = 0; i < 20; ++i)
                encoder.Write(pixels.data(), i * 500000LL, 500000);
            std::cout << "frames_written\n";
            if (failAtFinalize)
                state->failIndex = true;
            encoder.Finalize();
            std::cout << "finalize_returned\n";
        } catch (const winrt::hresult_error &e) {
            failed = true;
            std::cout << (failAtFinalize ? "finalize-write" : "quota-full") << " HRESULT=0x" << std::hex
                      << e.code().value << std::dec << '\n';
        }
        finalAttempted = encoder.FinalizationAttempted();
        std::cout << "leaving encoder, attempted=" << finalAttempted << '\n';
        finalSucceeded = encoder.Finalized();
        if (finalAttempted) {
            try {
                encoder.Finalize();
                return false;
            } catch (const winrt::hresult_error &e) {
                if (e.code() != MF_E_INVALIDREQUEST)
                    return false;
            }
            try {
                encoder.Write(pixels.data(), 10000000, 500000);
                return false;
            } catch (const winrt::hresult_error &) {
            }
        }
    }
    const auto deadline = GetTickCount64() + 2000;
    while (state->alive && GetTickCount64() < deadline)
        Sleep(10);
    std::cout << "injected=" << state->failures << " stream_alive=" << state->alive
              << " finalize_attempted=" << finalAttempted << " finalize_success=" << finalSucceeded << '\n';
    return failed && state->failures && !state->alive && !finalSucceeded && (!failAtFinalize || finalAttempted);
}
} // namespace
int main() {
    std::cout << std::unitbuf;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    bool result = false;
    try {
        result = Run(false) && Run(true);
    } catch (const winrt::hresult_error &e) {
        std::cerr << "Storage test setup error: 0x" << std::hex << e.code().value << '\n';
    }
    winrt::uninit_apartment();
    return result ? 0 : 1;
}
