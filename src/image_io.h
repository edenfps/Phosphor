#pragma once
#include "common.h"

bool LoadImageBGRA(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h);
bool SavePngBGRA(const std::wstring& path, const uint8_t* px, UINT w, UINT h, UINT stride);
// Calibration chart: text at many sizes, gray/near-black steps, colour bars,
// fine line patterns and isolated highlights on black (for halation).
bool MakePattern(const std::wstring& path, UINT w, UINT h);
