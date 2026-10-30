#pragma once
#include "common.h"

struct DisplayTarget
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput6> output;
    std::wstring gdiName;
    std::wstring friendlyName;
    RECT rect{};
    bool hdr = false;
    float sdrWhiteNits = 80.0f;
    float maxNits = 0.0f;
    float widthMm = 0.0f;
};

// sel: "" = largest display, "N" = index in the logged list, else a
// substring of the monitor's name or GDI device name.
bool FindDisplay(const std::wstring& sel, DisplayTarget& out);
