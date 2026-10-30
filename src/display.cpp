#include "display.h"
#include <algorithm>
#include <cwctype>

namespace
{
struct PathInfo
{
    std::wstring gdi;
    std::wstring friendly;
    float sdrWhite = 80.0f;
};

std::vector<PathInfo> QueryPaths()
{
    std::vector<PathInfo> out;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
        return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
        return out;
    for (UINT32 i = 0; i < np; i++)
    {
        const auto& p = paths[i];
        PathInfo info;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS)
            info.gdi = src.viewGdiDeviceName;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS)
            info.friendly = tn.monitorFriendlyDeviceName;

        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = p.targetInfo.adapterId;
        wl.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel > 0)
            info.sdrWhite = wl.SDRWhiteLevel / 1000.0f * 80.0f;

        out.push_back(info);
    }
    return out;
}

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}
} // namespace

bool FindDisplay(const std::wstring& sel, DisplayTarget& out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    auto paths = QueryPaths();

    std::vector<DisplayTarget> all;
    for (UINT a = 0;; a++)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0;; o++)
        {
            ComPtr<IDXGIOutput> op;
            if (adapter->EnumOutputs(o, &op) == DXGI_ERROR_NOT_FOUND)
                break;
            DisplayTarget t;
            t.adapter = adapter;
            if (FAILED(op.As(&t.output)))
                continue;
            DXGI_OUTPUT_DESC1 d;
            if (FAILED(t.output->GetDesc1(&d)) || !d.AttachedToDesktop)
                continue;
            t.gdiName = d.DeviceName;
            t.rect = d.DesktopCoordinates;
            t.hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            t.maxNits = d.MaxLuminance;
            for (auto& p : paths)
                if (p.gdi == t.gdiName)
                {
                    t.friendlyName = p.friendly;
                    t.sdrWhiteNits = p.sdrWhite;
                }
            if (HDC dc = CreateDCW(L"DISPLAY", d.DeviceName, nullptr, nullptr))
            {
                t.widthMm = (float)GetDeviceCaps(dc, HORZSIZE);
                DeleteDC(dc);
            }
            all.push_back(t);
        }
    }

    for (size_t i = 0; i < all.size(); i++)
    {
        auto& t = all[i];
        Log(L"display %zu: %s '%s' %ldx%ld at (%ld,%ld) hdr=%d sdrWhite=%.0f nits peak=%.0f nits width=%.0f mm", i,
            t.gdiName.c_str(), t.friendlyName.c_str(), t.rect.right - t.rect.left, t.rect.bottom - t.rect.top,
            t.rect.left, t.rect.top, t.hdr, t.sdrWhiteNits, t.maxNits, t.widthMm);
    }
    if (all.empty())
        return false;

    int pick = -1;
    if (!sel.empty())
    {
        if (iswdigit(sel[0]))
        {
            int idx = _wtoi(sel.c_str());
            if (idx >= 0 && idx < int(all.size()))
                pick = idx;
        }
        else
        {
            std::wstring s = Lower(sel);
            for (size_t i = 0; i < all.size() && pick < 0; i++)
                if (Lower(all[i].friendlyName).find(s) != std::wstring::npos ||
                    Lower(all[i].gdiName).find(s) != std::wstring::npos)
                    pick = int(i);
        }
        if (pick < 0)
        {
            // Never jump to another monitor (e.g. while the target switches modes).
            Log(L"display: '%s' not found, waiting for it", sel.c_str());
            return false;
        }
    }
    if (pick < 0)
    {
        long best = -1;
        for (size_t i = 0; i < all.size(); i++)
        {
            long area = (all[i].rect.right - all[i].rect.left) * (all[i].rect.bottom - all[i].rect.top);
            if (area > best)
            {
                best = area;
                pick = int(i);
            }
        }
    }
    out = all[pick];
    Log(L"display: using %d (%s)", pick, out.friendlyName.c_str());
    return true;
}
