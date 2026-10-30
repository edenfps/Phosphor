#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

void Log(const wchar_t* fmt, ...);
void EnableConsoleLog();
std::wstring ExeDir();
// Looks next to the exe, then up to 4 parent directories (so running from
// build/Release picks up the project's live shader and config).
std::wstring FindAsset(const std::wstring& rel);
uint64_t FileStamp(const std::wstring& path);
