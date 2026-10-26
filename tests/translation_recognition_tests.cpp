#include "image_recognition.h"
#include <iostream>
#include <winrt/base.h>

int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel)
        return 3;
    bool ok = true;
    // Pure input validation is before either recognition engine starts.
    try {
        nskry::translate::RecognizeImage({}, 20, 20, argv[1], cancel);
        ok = false;
    } catch (const winrt::hresult_error &e) {
        ok &= e.code() == E_BOUNDS;
    }
    SetEvent(cancel);
    try {
        nskry::translate::RecognizeImage({}, 0, 0, argv[1], cancel);
        ok = false;
    } catch (const winrt::hresult_error &e) {
        ok &= e.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    ResetEvent(cancel);
#if NSKRY_TRANSLATE_MODEL_OCR
    // Missing PRIVATE assets fail explicitly, not by searching installed plugins
    // or silently switching to a less reliable recognition path.
    try {
        nskry::translate::RecognizeImage(std::vector<unsigned char>(4, 255), 1, 1,
                                         L"Z:\\nskry-missing-private-image-runtime", cancel);
        ok = false;
    } catch (const std::wstring &e) {
        ok &= e.find(L"runtime missing") != e.npos;
    }
#endif
    // Exercise real task-owned pixels and process completion on synthetic text.
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 400, -80, 1, 32, BI_RGB};
    void *bits{};
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits)
        return 4;
    auto old = SelectObject(dc, bitmap);
    RECT area{0, 0, 400, 80};
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    auto font = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto oldFont = SelectObject(dc, font);
    TextOutW(dc, 20, 20, L"Hello world", 11);
    GdiFlush();
    std::vector<unsigned char> pixels(static_cast<unsigned char *>(bits),
                                      static_cast<unsigned char *>(bits) + 400 * 80 * 4);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    try {
        const auto text = nskry::translate::RecognizeImage(pixels, 400, 80, argv[1], cancel);
        ok &= text.find(L"Hello") != text.npos && text.find(L"world") != text.npos;
    } catch (...) {
        ok = false;
    }
    CloseHandle(cancel);
    winrt::uninit_apartment();
    std::cout << "Recognition bounds/cancel/private-runtime/task completion=" << ok << '\n';
    return ok ? 0 : 1;
}
