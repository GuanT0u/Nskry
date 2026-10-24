#include "ocr_wait.h"
#include <chrono>
#include <iostream>
using namespace std::chrono_literals;
using namespace winrt::Windows::Foundation;
IAsyncAction SlowOperation() { co_await winrt::resume_after(180ms); }
int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::handle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!cancel)
        return 2;
    bool reproduced = false;
    auto legacy = SlowOperation();
    try {
        while (legacy.wait_for(20ms) == AsyncStatus::Started) {
        }
    } catch (const winrt::hresult_error &error) {
        reproduced = error.code() == E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    nskry::translate::WaitForOcr(legacy, cancel.get(), 2000);
    legacy.GetResults();
    auto slow = SlowOperation();
    nskry::translate::WaitForOcr(slow, cancel.get(), 2000);
    bool ok = reproduced && slow.Status() == AsyncStatus::Completed;
    slow.GetResults();
    auto canceled = SlowOperation();
    SetEvent(cancel.get());
    try {
        nskry::translate::WaitForOcr(canceled, cancel.get(), 2000);
        ok = false;
    } catch (const winrt::hresult_error &error) {
        ok = error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED) && ok;
    }
    ResetEvent(cancel.get());
    auto timeout = SlowOperation();
    try {
        nskry::translate::WaitForOcr(timeout, cancel.get(), 5);
        ok = false;
    } catch (const winrt::hresult_error &error) {
        ok = error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && ok;
    }
    // Let this executable's synthetic coroutine timers unwind before COM teardown.
    Sleep(220);
    std::cout << "Reproduced 0x80000018=" << reproduced << "; slow completion/cancel/timeout fix=" << ok
              << '\n';
    winrt::uninit_apartment();
    return ok ? 0 : 1;
}
