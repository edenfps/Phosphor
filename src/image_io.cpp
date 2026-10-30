#include "image_io.h"
#include <wincodec.h>
#include <algorithm>

static ComPtr<IWICImagingFactory> Wic()
{
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool LoadImageBGRA(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h)
{
    auto f = Wic();
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (!f || FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    conv->GetSize(&w, &h);
    px.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()));
}

bool SavePngBGRA(const std::wstring& path, const uint8_t* px, UINT w, UINT h, UINT stride)
{
    auto f = Wic();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (!f || FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(w, h)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA)
        return false;
    return SUCCEEDED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(px))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

bool MakePattern(const std::wstring& path, UINT w, UINT h)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = int(w);
    bi.bmiHeader.biHeight = -int(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    float s = h / 2160.0f;
    auto S = [&](float v) { return int(v * s + 0.5f); };
    auto Fill = [&](int x, int y, int cw, int ch, COLORREF c) {
        RECT r{x, y, x + cw, y + ch};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    auto Text = [&](int x, int y, int px, const wchar_t* face, COLORREF c, const wchar_t* str, int weight = FW_NORMAL) {
        HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
        HGDIOBJ of = SelectObject(dc, f);
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, x, y, str, int(wcslen(str)));
        SelectObject(dc, of);
        DeleteObject(f);
    };

    Fill(0, 0, w, h, RGB(0, 0, 0));

    // Text block, white on black, many sizes.
    const wchar_t* sample = L"The quick brown fox jumps over the lazy dog 0123456789 {}[]()<>";
    int y = S(40);
    int sizes[] = {11, 13, 15, 18, 22, 28, 36, 48};
    for (int px : sizes)
    {
        Text(S(40), y, S(float(px)), L"Segoe UI", RGB(255, 255, 255), sample);
        y += S(px * 1.45f);
    }
    Text(S(40), y, S(16), L"Consolas", RGB(200, 200, 200), L"C:\\> dir /w   int main(void) { return 0; }   ||| ::: ...");
    y += S(30);
    COLORREF tc[] = {RGB(255, 60, 60), RGB(60, 255, 60), RGB(80, 120, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(255, 0, 255)};
    for (int i = 0; i < 6; i++)
        Text(S(40 + i * 250.0f), y, S(22), L"Segoe UI", tc[i], L"Colour text", FW_SEMIBOLD);
    y += S(50);

    // Black text on white panel.
    Fill(S(40), y, S(1500), S(140), RGB(255, 255, 255));
    Text(S(60), y + S(15), S(18), L"Segoe UI", RGB(0, 0, 0), sample);
    Text(S(60), y + S(50), S(28), L"Segoe UI", RGB(0, 0, 0), L"Black on white: paper-like documents");
    Text(S(60), y + S(95), S(13), L"Segoe UI", RGB(0, 0, 0), sample);
    y += S(180);

    // Gray steps 0..255 in 16 bars + near-black steps 0..15.
    for (int i = 0; i < 16; i++)
    {
        int v = std::min(255, i * 17);
        Fill(S(40 + i * 100.0f), y, S(100), S(90), RGB(v, v, v));
    }
    y += S(100);
    for (int i = 0; i < 16; i++)
        Fill(S(40 + i * 100.0f), y, S(100), S(90), RGB(i, i, i));
    y += S(100);
    // Smooth ramp.
    int rw = S(1600);
    for (int x = 0; x < rw; x++)
    {
        int v = x * 255 / (rw - 1);
        Fill(S(40) + x, y, 1, S(60), RGB(v, v, v));
    }
    y += S(80);
    // 75% colour bars.
    COLORREF bars[] = {RGB(191, 191, 191), RGB(191, 191, 0), RGB(0, 191, 191), RGB(0, 191, 0),
                       RGB(191, 0, 191), RGB(191, 0, 0), RGB(0, 0, 191), RGB(0, 0, 0)};
    for (int i = 0; i < 8; i++)
        Fill(S(40 + i * 200.0f), y, S(200), S(160), bars[i]);

    // Right side: fine line patterns.
    int rx = S(1900);
    int ry = S(40);
    uint32_t* p = (uint32_t*)bits;
    auto Px = [&](int x, int yy, uint32_t c) {
        if (x >= 0 && yy >= 0 && x < int(w) && yy < int(h))
            p[size_t(yy) * w + x] = c;
    };
    GdiFlush();
    int bw = S(400);
    for (int yy = 0; yy < bw; yy++)
        for (int x = 0; x < bw; x++)
        {
            Px(rx + x, ry + yy, (x & 1) ? 0xFFFFFF : 0);                         // 1px vertical lines
            Px(rx + bw + S(40) + x, ry + yy, (yy & 1) ? 0xFFFFFF : 0);          // 1px horizontal lines
            Px(rx + 2 * (bw + S(40)) + x, ry + yy, ((x ^ yy) & 1) ? 0xFFFFFF : 0); // checker
            Px(rx + 3 * (bw + S(40)) + x, ry + yy, ((x / 2) & 1) ? 0xFFFFFF : 0);  // 2px stripes
        }
    ry += bw + S(60);

    // Halation targets: highlights on black.
    Fill(rx, ry, S(120), S(120), RGB(255, 255, 255));
    Fill(rx + S(300), ry + S(50), S(20), S(20), RGB(255, 255, 255));
    for (int i = 0; i < 12; i++)
        Fill(rx + S(500 + i * 40.0f), ry + S(55), S(3), S(3), RGB(255, 255, 255));
    for (int i = 0; i < 6; i++)
        Fill(rx + S(500 + i * 60.0f), ry + S(110), S(8), S(8), tc[i]);
    ry += S(200);
    Text(rx, ry, S(64), L"Segoe UI", RGB(255, 255, 255), L"Phosphor", FW_BOLD);
    Text(rx + S(420), ry + S(10), S(48), L"Segoe UI", RGB(255, 140, 0), L"glow test", FW_SEMIBOLD);
    ry += S(120);
    Text(rx, ry, S(14), L"Segoe UI", RGB(255, 255, 255), L"Small white text on black shows beam spot + diffusion.");
    ry += S(30);
    Text(rx, ry, S(14), L"Segoe UI", RGB(128, 128, 128), L"Mid-gray text: spot narrows as beam current drops.");
    ry += S(60);
    // Big white field next to black for halation spill.
    Fill(rx, ry, S(800), S(400), RGB(255, 255, 255));
    Fill(rx + S(250), ry + S(130), S(300), S(140), RGB(0, 0, 0));

    GdiFlush();
    for (size_t i = 0; i < size_t(w) * h; i++)
        p[i] |= 0xFF000000;
    bool ok = SavePngBGRA(path, (const uint8_t*)bits, w, h, w * 4);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}
