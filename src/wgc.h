#pragma once
#include "common.h"
#include <memory>

// Windows Graphics Capture of one window (a borderless fullscreen game).
// Frames come straight from the game's own surface instead of the composed
// desktop, so nothing drawn on top of it (including the overlay) is seen.
class WindowCapture
{
public:
    WindowCapture();
    ~WindowCapture();
    // hdr: capture as FP16 scRGB instead of 8-bit BGRA.
    bool Start(ID3D11Device* dev, HWND hwnd, bool hdr);
    void Stop();
    bool Active() const;
    HWND Target() const;
    // Signalled whenever a frame arrives.
    HANDLE Event() const;
    // Newest frame since the last call, or false if none arrived (or the
    // capture ended; then Active() turns false). The texture stays valid until
    // the next call. qpc = when the frame arrived.
    bool Acquire(ComPtr<ID3D11Texture2D>& tex, LONGLONG& qpc, int& skipped);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
