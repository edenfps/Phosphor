// Phosphor: whole-desktop CRT emulation.
//
// Captures the target monitor with Desktop Duplication, runs the CRT model and
// presents the result in a click-through, topmost window that is excluded from
// capture (so it never sees itself). Games must run borderless/windowed.

#include "common.h"
#include "config.h"
#include "detect.h"
#include "display.h"
#include "image_io.h"
#include "renderer.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace
{
enum { HK_TOGGLE = 1, HK_SPLIT, HK_PRESET, HK_RELOAD, HK_QUIT, HK_BEAM, HK_LAYOUT, HK_MOTION, HK_DEBLUR, HK_VECTORS, HK_CHARACTER, HK_AUTORES, HK_MENU };
enum { ID_ENABLE = 100, ID_SPLIT, ID_RELOAD, ID_OPENCFG, ID_EXIT, ID_AUTORES, ID_MOTION, ID_CHARACTER,
       ID_PRESET0 = 200, ID_BEAM0 = 300, ID_LAYOUT0 = 400 };
const UINT WM_TRAY = WM_APP + 1;

struct App
{
    Config cfg;
    std::wstring cfgPath, shaderPath;
    uint64_t cfgStamp = 0, shaderStamp = 0;
    DWORD lastCheck = 0, lastTopmost = 0, lastSetupTry = 0;

    DisplayTarget disp;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutputDuplication> dup;
    DXGI_FORMAT dupFmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> frame;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<ID3D11RenderTargetView> rtv;
    Renderer renderer;
    UINT w = 0, h = 0;

    HWND overlay = nullptr, ctrl = nullptr;
    NOTIFYICONDATAW tray{};
    int activeApp = -1; // index into cfg.apps for the focused exe
    DWORD lastAppCheck = 0;
    bool enabled = true, running = true, haveFrame = false, dirty = false, visible = false, needSetup = true;
    float split = 0.0f;
    DWORD osdUntil = 0; // info panel visible until this tick
    bool osdInfo = false;
    int beamLevel = 0;  // index into kBeamLevels
    bool characterOff = false; // A/B: veiling glow + analog softness forced off
    // Automatic resolution detection
    int autoRes = -1;          // override: -1 = from config
    UINT menuVk = 0;           // registered menu key
    int autoW = 0, autoH = 0;  // detected source size, 0 = native
    DWORD lastProbe = 0;
    bool framesSinceProbe = false;
    int layout = -1;    // index into kLayouts, -1 = from config
    int motion = -1;    // rolling scan override: -1 = from config, 0 off, 1 on
    long long lastField = -1;
    bool fieldReady = false; // next field already rendered, waiting for the beam
    int fieldStage = -1;     // stage of the field being rendered, -1 = idle
    // Motion pre-deblur
    int deblur = -1;         // override: -1 = from config
    bool vectors = false;    // motion debug tint
    bool newFrame = false;   // captured since last render: measure motion
    bool motionShown = false; // last render may carry motion compensation
    LONGLONG lastCaptureQpc = 0;
    double refreshMs = 1000.0 / 240.0;
} g;

void ShowOsd(DWORD ms = 6000);
void RegisterMenuKey();

bool MotionOn() { return g.motion >= 0 ? g.motion != 0 : g.cfg.motion; }
bool AutoResOn() { return g.autoRes >= 0 ? g.autoRes != 0 : g.cfg.autoResolution; }
ResolutionDetector gDetector;
// Motion pre-deblur is shelved: measured at ~0.5% less perceived blur, and even
// an optimal version is capped near 15-24% because pre-compensation would need
// light below black. Set to 1 to bring it back (D / V hotkeys, deblur keys).
#define PHOSPHOR_DEBLUR 0

bool DeblurOn() { return PHOSPHOR_DEBLUR && (g.deblur >= 0 ? g.deblur != 0 : g.cfg.deblur); }

LONGLONG Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Index of the panel refresh the frame being prepared now will appear on.
// The overlay is shown through DWM, so the loop runs in lockstep with DWM's
// compositions (DwmFlush after each present) and takes DWM's exact refresh
// counter: the frame drawn now is picked up by the next composition.
long long TargetRefresh()
{
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.qpcRefreshPeriod == 0)
        return -1;
    g.refreshMs = double(ti.qpcRefreshPeriod) * 1000.0 / double(freq.QuadPart);
    return LONGLONG(ti.cRefresh) + 1;
}

// Pacing diagnostics, logged once a second while the rolling scan runs.
struct ScanStats
{
    DWORD since = 0;
    long long last = -1;
    int frames = 0, skipped = 0, repeated = 0;
    // From DXGI frame statistics: what the panel actually showed.
    int sliceOf[256] = {}; // slice submitted, by present id
    UINT lastPresent = 0, lastRefresh = 0;
    int offset = -1;       // (displayed refresh - slice) mod K, should never change
    int shownLate = 0, dropped = 0, phaseJumps = 0, statFails = 0;
} gScan;

// Compare what was submitted with what the panel reports it displayed.
void CheckDisplayed(int k)
{
    DXGI_FRAME_STATISTICS st{};
    if (FAILED(g.sc->GetFrameStatistics(&st)))
    {
        gScan.statFails++;
        return;
    }
    if (st.PresentCount == gScan.lastPresent)
        return;
    if (gScan.lastPresent)
    {
        UINT dp = st.PresentCount - gScan.lastPresent, dr = st.PresentRefreshCount - gScan.lastRefresh;
        if (dr > dp)
            gScan.shownLate += int(dr - dp); // a refresh repeated the previous slice
        else if (dr < dp)
            gScan.dropped += int(dp - dr);   // a frame never reached the panel
    }
    int slice = gScan.sliceOf[st.PresentCount & 255];
    int off = int(((long long)st.PresentRefreshCount - slice) % k + k) % k;
    if (gScan.offset >= 0 && off != gScan.offset)
        gScan.phaseJumps++;
    gScan.offset = off;
    gScan.lastPresent = st.PresentCount;
    gScan.lastRefresh = st.PresentRefreshCount;
}

void NoteRefresh(long long idx)
{
    if (gScan.last >= 0)
    {
        long long d = idx - gScan.last;
        if (d == 0)
            gScan.repeated++;
        else if (d > 1)
            gScan.skipped += int(d - 1);
    }
    gScan.last = idx;
    gScan.frames++;
    DWORD now = GetTickCount();
    if (now - gScan.since >= 1000)
    {
        if (gScan.since)
            Log(L"scan: %d frames/s, %d skipped, %d twice | panel: %d late, %d dropped, %d phase jumps, %d stat "
                L"fails",
                gScan.frames, gScan.skipped, gScan.repeated, gScan.shownLate, gScan.dropped, gScan.phaseJumps,
                gScan.statFails);
        gScan.since = now;
        gScan.frames = gScan.skipped = gScan.repeated = 0;
        gScan.shownLate = gScan.dropped = gScan.phaseJumps = gScan.statFails = 0;
    }
}

int MotionK() { return std::max(1, int(std::lround(1000.0 / g.refreshMs / std::max(g.cfg.crtHz, 1.0f)))); }

// Beam sharpness override cycled with Ctrl+Alt+Shift+B, applied on top of
// whichever look is active. Level 0 keeps the look's own beam.
struct BeamLevel
{
    const wchar_t* name;
    const wchar_t* hint;
    float spotMinMm, spotMaxMm, spotTail, diffusion;
};
const BeamLevel kBeamLevels[] = {
    {L"as set in the look", L"", 0, 0, 0, 0},
    {L"Sharpest", L"Tightest beam. Almost pixel-sharp text, glow only on very bright things.", 0.012f, 0.03f, 0.0f, 0.004f},
    {L"Sharper", L"Between Sharpest and the main look.", 0.02f, 0.045f, 0.01f, 0.006f},
    {L"Main", L"The current main-look beam.", 0.03f, 0.06f, 0.02f, 0.008f},
    {L"Softer", L"The previous default: rounder, glowier text.", 0.045f, 0.09f, 0.04f, 0.012f},
    {L"Softest", L"A worn or lower-end tube. Clearly soft text.", 0.07f, 0.14f, 0.06f, 0.02f},
};

// Where each colour's emitter sits inside a panel pixel (px from centre).
// QD-OLED uses an undocumented triangle; cycle with Ctrl+Alt+Shift+L and keep
// the one where colour fringes on text disappear.
struct SubpixelLayout
{
    const wchar_t* name;
    float x[3], y[3]; // R, G, B
};
const float kA = 0.25f, kT = 0.18f, kS = 1.0f / 3.0f;
const SubpixelLayout kLayouts[] = {
    {L"Off (no correction)", {0, 0, 0}, {0, 0, 0}},
    {L"Triangle: green top, red bottom-left, blue bottom-right", {-kA, 0, kA}, {kT, -kT, kT}},
    {L"Triangle: green top, blue bottom-left, red bottom-right", {kA, 0, -kA}, {kT, -kT, kT}},
    {L"Triangle: green bottom, red top-left, blue top-right", {-kA, 0, kA}, {-kT, kT, -kT}},
    {L"Triangle: red top, green bottom-left, blue bottom-right", {0, -kA, kA}, {-kT, kT, kT}},
    {L"Triangle: blue top, red bottom-left, green bottom-right", {-kA, kA, 0}, {kT, kT, -kT}},
    {L"Triangle: blue top, green bottom-left, red bottom-right", {kA, -kA, 0}, {kT, kT, -kT}},
    {L"Stripe: red left, blue right (RGB)", {-kS, 0, kS}, {0, 0, 0}},
    {L"Stripe: blue left, red right (BGR)", {kS, 0, -kS}, {0, 0, 0}},
};

int CurrentLayout()
{
    int n = int(_countof(kLayouts));
    int i = g.layout >= 0 ? g.layout : g.cfg.subpixelLayout;
    return i >= 0 && i < n ? i : 0;
}

// User's preset, overridden by the focused app's profile and the beam level.
Look EffectiveLook()
{
    Look l = g.cfg.look();
    if (g.activeApp >= 0 && g.activeApp < int(g.cfg.apps.size()))
    {
        const AppProfile& a = g.cfg.apps[g.activeApp];
        int idx = a.preset.empty() ? -1 : g.cfg.FindPreset(a.preset);
        if (idx >= 0)
            l = g.cfg.presets[idx].look;
        if (a.sourceW >= 0)
        {
            l.sourceW = a.sourceW;
            l.sourceH = a.sourceH;
            l.lines = 0;
        }
    }
    // Detected content resolution, unless the look or a game profile sets one.
    if (AutoResOn() && g.autoH > 0 && l.sourceW < 1 && l.sourceH < 1 && l.lines < 1)
    {
        l.sourceW = float(g.autoW);
        l.sourceH = float(g.autoH);
    }
    if (g.characterOff)
    {
        l.veil = 0;
        l.analogSoftMm = 0;
    }
    if (g.beamLevel > 0)
    {
        const BeamLevel& b = kBeamLevels[g.beamLevel];
        l.spotMinMm = b.spotMinMm;
        l.spotMaxMm = b.spotMaxMm;
        l.spotTail = b.spotTail;
        l.diffusion = b.diffusion;
    }
    return l;
}

std::wstring ForegroundExe()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return L"";
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring exe;
    if (QueryFullProcessImageNameW(h, 0, path, &n))
    {
        exe = path;
        exe = exe.substr(exe.find_last_of(L"\\/") + 1);
        std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
    }
    CloseHandle(h);
    return exe;
}

void CheckApp()
{
    if (g.cfg.apps.empty())
    {
        g.activeApp = -1;
        return;
    }
    std::wstring exe = ForegroundExe();
    int found = -1;
    for (size_t i = 0; i < g.cfg.apps.size(); i++)
        if (g.cfg.apps[i].exe == exe)
            found = int(i);
    if (found != g.activeApp)
    {
        g.activeApp = found;
        g.dirty = true;
        ShowOsd();
        Log(found >= 0 ? L"app profile: %s" : L"app profile: none (%s)", exe.c_str());
    }
}

FrameParams MakeParams()
{
    FrameParams fp;
    fp.look = EffectiveLook();
    fp.outputBits = g.disp.hdr ? 16 : 10;
    fp.inputMode = g.dupFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 0;
    float sdrWhite = g.cfg.sdrWhiteNits > 0 ? g.cfg.sdrWhiteNits : g.disp.sdrWhiteNits;
    fp.sdrWhiteScale = sdrWhite / 80.0f;
    float peak = g.cfg.peakNits > 0 ? g.cfg.peakNits : (g.disp.maxNits > 0 ? g.disp.maxNits : 1000.0f);
    fp.peak = std::max(peak / sdrWhite, 1.0f);
    fp.outputMode = g.disp.hdr ? 2 : g.cfg.sdrOutput;
    float widthMm = g.cfg.panelWidthMm > 0 ? g.cfg.panelWidthMm : g.disp.widthMm;
    if (widthMm < 200 || widthMm > 3000)
        widthMm = 697.0f; // 31.5" 16:9 fallback
    fp.pixelMm = widthMm / float(std::max(g.w, 1u));
    fp.split = g.split;
    const SubpixelLayout& sl = kLayouts[CurrentLayout()];
    for (int c = 0; c < 3; c++)
    {
        fp.subX[c] = sl.x[c];
        fp.subY[c] = sl.y[c];
    }
    fp.motionK = MotionK();
    // HDR headroom lets a whole field's light fit in fewer refreshes: sharper motion.
    fp.motionPeak = g.disp.hdr ? fp.peak : 1.0f;
    fp.motionGain = g.cfg.motionGain;
    fp.afterglow = float(g.cfg.afterglowMs / g.refreshMs);
    fp.scanSoftness = g.cfg.scanSoftness;
    fp.motionMode = g.cfg.motionMode;
    fp.changeThreshold = g.cfg.changeThreshold;
    fp.maskDecay = g.cfg.maskDecay;
    fp.newFrame = g.newFrame;
    fp.deblur = DeblurOn() ? g.cfg.deblurStrength : 0.0f;
    fp.deblurMaxPx = g.cfg.deblurMaxPx;
    fp.motionDebug = g.vectors;
    return fp;
}

void UpdateTrayTip()
{
    swprintf_s(g.tray.szTip, L"Phosphor - %s%s", g.cfg.presets[g.cfg.presetIndex].name.c_str(),
               g.enabled ? L"" : L" (off)");
    g.tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

void Teardown()
{
    if (g.overlay)
        ShowWindow(g.overlay, SW_HIDE);
    g.visible = false;
    g.rtv.Reset();
    g.sc.Reset();
    g.frame.Reset();
    g.dup.Reset();
    if (g.ctx)
    {
        g.ctx->ClearState();
        g.ctx->Flush();
    }
    g.haveFrame = false;
}

bool Setup()
{
    Teardown();
    g.needSetup = false;
    DisplayTarget d;
    if (!FindDisplay(g.cfg.monitor, d))
        return false;

    bool sameAdapter = false;
    if (g.dev && g.disp.adapter)
    {
        DXGI_ADAPTER_DESC1 a, b;
        g.disp.adapter->GetDesc1(&a);
        d.adapter->GetDesc1(&b);
        sameAdapter = memcmp(&a.AdapterLuid, &b.AdapterLuid, sizeof(LUID)) == 0;
    }
    g.disp = d;
    if (!sameAdapter)
    {
        g.renderer = Renderer{};
        g.ctx.Reset();
        g.dev.Reset();
        D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(d.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       fls, _countof(fls), D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
        if (FAILED(hr))
        {
            Log(L"D3D11CreateDevice failed 0x%08x", hr);
            return false;
        }
        if (!g.renderer.Init(g.dev.Get(), g.shaderPath))
        {
            Log(L"renderer init failed (shader error?)");
            return false;
        }
    }

    ComPtr<IDXGIOutput5> o5;
    if (FAILED(d.output.As(&o5)))
        return false;
    DXGI_FORMAT fmt = d.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    HRESULT hr = o5->DuplicateOutput1(g.dev.Get(), 0, 1, &fmt, &g.dup);
    if (FAILED(hr))
    {
        Log(L"DuplicateOutput1 failed 0x%08x", hr);
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    g.dup->GetDesc(&dd);
    g.dupFmt = dd.ModeDesc.Format;
    g.w = dd.ModeDesc.Width;
    g.h = dd.ModeDesc.Height;
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        Log(L"warning: rotated displays are not supported");

    D3D11_TEXTURE2D_DESC td{};
    td.Width = g.w;
    td.Height = g.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = g.dupFmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &g.frame)))
        return false;

    const RECT& r = d.rect;
    SetWindowPos(g.overlay, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);

    ComPtr<IDXGIFactory2> factory;
    d.adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = g.w;
    scd.Height = g.h;
    // 10-bit SDR + dither: no banding after the tube response curve.
    scd.Format = d.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(g.dev.Get(), g.overlay, &scd, nullptr, nullptr, &g.sc);
    if (FAILED(hr))
    {
        Log(L"CreateSwapChainForHwnd failed 0x%08x", hr);
        return false;
    }
    factory->MakeWindowAssociation(g.overlay, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (d.hdr)
    {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(g.sc.As(&sc3)))
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    }
    ComPtr<IDXGIDevice1> dxgiDev;
    if (SUCCEEDED(g.dev.As(&dxgiDev)))
    {
        dxgiDev->SetMaximumFrameLatency(1);
        // Rolling-scan slices have a hard per-refresh deadline.
        HRESULT pr = dxgiDev->SetGPUThreadPriority(7);
        Log(L"gpu priority +7: %s", SUCCEEDED(pr) ? L"ok" : L"refused");
    }

    ComPtr<ID3D11Texture2D> bb;
    g.sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &g.rtv);

    FrameParams fp = MakeParams();
    Log(L"running: %ux%u %s, sdr white %.0f nits, peak x%.2f, pixel %.4f mm", g.w, g.h,
        d.hdr ? L"HDR scRGB" : L"SDR", fp.sdrWhiteScale * 80.0f, fp.peak, fp.pixelMm);
    ShowOsd();
    return true;
}

// On-screen help: which look is active and what to look for, plus labels for
// the split compare. Rasterised with GDI into (text coverage, panel opacity).
void RebuildOsd()
{
    if (!g.dev || !g.w)
        return;
    bool info = GetTickCount() < g.osdUntil;
    g.osdInfo = info;
    if (!info && g.split <= 0)
    {
        g.renderer.ClearOsd();
        return;
    }

    UINT w = g.w, h = g.h;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = int(w);
    bi.bmiHeader.biHeight = -int(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp)
    {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    float s = h / 2160.0f;
    auto S = [&](float v) { return int(v * s + 0.5f); };
    auto Font = [&](float px, int weight) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    };
    HFONT titleFont = Font(42, FW_SEMIBOLD), bodyFont = Font(30, FW_NORMAL), smallFont = Font(25, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 0, 0)); // red = text, green = panel
    auto Panel = [&](RECT r) {
        HBRUSH b = CreateSolidBrush(RGB(0, 255, 0));
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    // Measures (draw = false) or draws word-wrapped text; returns its height.
    auto Text = [&](HFONT f, const std::wstring& t, int x, int y, int width, bool draw) {
        SelectObject(dc, f);
        RECT r{x, y, x + width, y};
        DrawTextW(dc, t.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        if (draw)
            DrawTextW(dc, t.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        return int(r.bottom - r.top);
    };

    if (info)
    {
        const Preset* p = &g.cfg.presets[g.cfg.presetIndex];
        std::wstring profile;
        if (g.activeApp >= 0)
        {
            const AppProfile& a = g.cfg.apps[g.activeApp];
            int idx = a.preset.empty() ? -1 : g.cfg.FindPreset(a.preset);
            if (idx >= 0)
                p = &g.cfg.presets[idx];
            profile = L"   [game profile: " + a.exe + L"]";
        }
        int idx = int(p - g.cfg.presets.data()) + 1;
        std::wstring title = L"Phosphor   \x2022   " + p->name + L"   (look " + std::to_wstring(idx) + L" of " +
                             std::to_wstring(g.cfg.presets.size()) + L")" + profile;
        std::wstring body = p->description.empty() ? L"No description for this look." : p->description;
        const BeamLevel& bl = kBeamLevels[g.beamLevel];
        title += L"   \x2022   Beam: " + std::wstring(bl.name) + L" (" + std::to_wstring(g.beamLevel + 1) + L"/" +
                 std::to_wstring(_countof(kBeamLevels)) + L")";
        if (g.beamLevel > 0)
            body = L"Beam sharpness: " + std::wstring(bl.name) + L". " + bl.hint + L"\n" + body;
        {
            Look el = EffectiveLook();
            wchar_t ch[200];
            if (g.characterOff)
                swprintf_s(ch, L"\nGlass veil + analog softness: OFF (press G to turn back on)");
            else
                swprintf_s(ch, L"\nGlass veil + analog softness: ON  (veil %.3f, analog %.2f mm; press G to compare off)",
                           el.veil, el.analogSoftMm);
            body = ch + (L"\n" + body);
        }
        {
            wchar_t ar[160];
            if (!AutoResOn())
                swprintf_s(ar, L"\nAuto resolution: off (press A to turn on)");
            else if (g.autoH > 0)
                swprintf_s(ar, L"\nAuto resolution: content detected at %dx%d, redrawn at that resolution (A = off)",
                           g.autoW, g.autoH);
            else
                swprintf_s(ar, L"\nAuto resolution: on, native 4K content (A = off)");
            body += ar;
        }
        int li = CurrentLayout();
        body += L"\nColour fringe fix " + std::to_wstring(li) + L"/" + std::to_wstring(_countof(kLayouts) - 1) +
                L": " + kLayouts[li].name + L".";
        if (MotionOn())
        {
            wchar_t m[256];
            if (g.cfg.motionMode == 1)
                swprintf_s(m, L"\nMotion: ON (adaptive flash, %.0f Hz). Only areas that are moving strobe, all at "
                              L"once (no bands); static text and UI stay steady. Moving content is shown at %.0f fps.",
                           1000.0 / g.refreshMs / MotionK(), 1000.0 / g.refreshMs / MotionK());
            else
                swprintf_s(m, L"\nMotion: ON. Rolling CRT scan at %.0f Hz (%d panel refreshes per frame). Moving "
                              L"things stay sharp like a CRT. Content is shown at %.0f fps; may flicker like a real tube.",
                           1000.0 / g.refreshMs / MotionK(), MotionK(), 1000.0 / g.refreshMs / MotionK());
            body += m;
        }
        else
            body += L"\nMotion: off (normal OLED motion). Press M for CRT motion clarity.";
#if PHOSPHOR_DEBLUR
        if (DeblurOn())
            body += L"\nMotion pre-deblur: ON (experimental). Moving content is pre-sharpened along its motion "
                    L"so your eye's smear lands sharper. V shows the measured motion as colour.";
        else
            body += L"\nMotion pre-deblur: off. Press D to try it.";
#endif
        std::wstring keys = (g.menuVk ? g.cfg.menuKey + L"  all toggles      " : std::wstring()) +
                            L"Ctrl+Alt+Shift +     A  auto resolution      M  CRT motion      B  beam      "
                            L"P  next look      S  split      C  on/off      Q  quit";
        std::wstring hdr = g.disp.hdr ? L""
                                      : L"Windows HDR is off, so highlight glow and bright scanlines are limited. "
                                        L"Turn on HDR in Windows display settings for the full look.";

        int x = S(60), y = S(60), pad = S(30), tw = S(1800), gap = S(18);
        int hT = Text(titleFont, title, 0, 0, tw, false);
        int hB = Text(bodyFont, body, 0, 0, tw, false);
        int hK = Text(smallFont, keys, 0, 0, tw, false);
        int hH = hdr.empty() ? 0 : Text(smallFont, hdr, 0, 0, tw, false);
        int total = hT + gap + hB + gap * 2 + hK + (hH ? gap + hH : 0);
        Panel({x, y, x + tw + 2 * pad, y + total + 2 * pad});
        int cy = y + pad;
        cy += Text(titleFont, title, x + pad, cy, tw, true) + gap;
        cy += Text(bodyFont, body, x + pad, cy, tw, true) + gap * 2;
        cy += Text(smallFont, keys, x + pad, cy, tw, true);
        if (hH)
            Text(smallFont, hdr, x + pad, cy + gap, tw, true);
    }

    if (g.split > 0)
    {
        int cx = int(g.split * w);
        auto Label = [&](const std::wstring& t, int centerX) {
            SelectObject(dc, titleFont);
            SIZE sz;
            GetTextExtentPoint32W(dc, t.c_str(), int(t.size()), &sz);
            int pad = S(24), y = int(h) - S(140);
            Panel({centerX - sz.cx / 2 - pad, y - pad / 2, centerX + sz.cx / 2 + pad, y + sz.cy + pad / 2});
            TextOutW(dc, centerX - sz.cx / 2, y, t.c_str(), int(t.size()));
        };
        Label(L"ORIGINAL  (raw OLED, no CRT)", cx / 2);
        Label(L"CRT", cx + (int(w) - cx) / 2);
    }

    GdiFlush();
    std::vector<uint8_t> rg(size_t(w) * h * 2);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < size_t(w) * h; i++)
    {
        uint8_t r = src[i * 4 + 2], gch = src[i * 4 + 1];
        rg[i * 2] = r;
        rg[i * 2 + 1] = uint8_t(std::min(255, r + gch));
    }
    g.renderer.SetOsd(rg.data(), w, h);

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
    DeleteDC(dc);
}

void ShowOsd(DWORD ms)
{
    g.osdUntil = GetTickCount() + ms;
    RebuildOsd();
    g.dirty = true;
}

// Debug: save the raw captured desktop (before any CRT processing) as a PNG.
void DumpFrame(const std::wstring& path)
{
    D3D11_TEXTURE2D_DESC td;
    g.frame->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &st)))
        return;
    g.ctx->CopyResource(st.Get(), g.frame.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return;
    std::vector<uint8_t> px(size_t(td.Width) * td.Height * 4);
    float white = (g.cfg.sdrWhiteNits > 0 ? g.cfg.sdrWhiteNits : g.disp.sdrWhiteNits) / 80.0f;
    for (UINT y = 0; y < td.Height; y++)
    {
        const uint8_t* row = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
        uint8_t* out = &px[size_t(y) * td.Width * 4];
        if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
            memcpy(out, row, td.Width * 4);
        else
            for (UINT x = 0; x < td.Width; x++)
            {
                // scRGB fp16 -> SDR-relative linear -> sRGB code values (BGRA)
                const DirectX::PackedVector::HALF* hp = (const DirectX::PackedVector::HALF*)row + x * 4;
                for (int c = 0; c < 3; c++)
                {
                    float v = std::max(DirectX::PackedVector::XMConvertHalfToFloat(hp[c]) / white, 0.0f);
                    v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                    out[x * 4 + (2 - c)] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                out[x * 4 + 3] = 255;
            }
    }
    g.ctx->Unmap(st.Get(), 0);
    bool ok = SavePngBGRA(path, px.data(), td.Width, td.Height, td.Width * 4);
    Log(ok ? L"dumped frame to %s" : L"frame dump failed: %s", path.c_str());
}

void RenderPresent()
{
    g.renderer.Render(g.frame.Get(), g.rtv.Get(), g.w, g.h, MakeParams());
    g.newFrame = false;
    // Flip model releases the RTV binding on present; rebind each frame.
    g.sc->Present(1, 0);
    g.dirty = false;
    if (!g.visible)
    {
        ShowWindow(g.overlay, SW_SHOWNOACTIVATE);
        g.visible = true;
    }
}

// Rolling scan: one call per panel refresh. The full CRT model runs once per
// CRT field (when the beam returns to the top, picking up new content then,
// so nothing tears); every refresh draws its slice of the scan.
void MotionStep()
{
    FrameParams fp = MakeParams();
    long long idx = TargetRefresh();
    if (idx < 0)
        return;
    NoteRefresh(idx);
    fp.motionK = MotionK();
    long long field = idx / fp.motionK;
    int s = int(idx % fp.motionK);
    if (!g.renderer.HasField())
    {
        g.renderer.RenderField(g.frame.Get(), g.w, g.h, fp);
        g.renderer.LatchField(true);
        g.dirty = false;
        g.fieldReady = false;
        g.lastField = field;
    }
    else if (field != g.lastField)
    {
        // Beam back at the top: show the field prepared during the last one.
        g.renderer.LatchField(g.fieldReady);
        g.fieldReady = false;
        g.lastField = field;
    }
    g.renderer.RenderScan(g.rtv.Get(), g.w, g.h, fp, s);
    // Hand the frame to DWM immediately, then wait for the composition that
    // takes it: one frame per composition, slice matching the refresh.
    g.sc->Present(0, 0);
    UINT pid = 0;
    if (SUCCEEDED(g.sc->GetLastPresentCount(&pid)))
        gScan.sliceOf[pid & 255] = s;
    CheckDisplayed(fp.motionK);
    if (!g.visible)
    {
        ShowWindow(g.overlay, SW_SHOWNOACTIVATE);
        g.visible = true;
    }
    // The full CRT model for new content runs here, in the wait after a
    // present, one stage per refresh so no refresh ever carries more than a
    // sliver of extra GPU work and misses its slot.
    if (g.fieldStage < 0 && g.dirty && !g.fieldReady)
    {
        g.fieldStage = 0;
        g.dirty = false;
    }
    if (g.fieldStage >= 0)
    {
        g.renderer.RenderField(g.frame.Get(), g.w, g.h, fp, g.fieldStage);
        if (++g.fieldStage >= Renderer::kFieldStages)
        {
            g.fieldStage = -1;
            g.fieldReady = true;
        }
    }
    DwmFlush();
}

void SetEnabled(bool on)
{
    g.enabled = on;
    if (on)
        g.needSetup = true;
    else
        Teardown();
    UpdateTrayTip();
}

void NextPreset(int idx = -1)
{
    int n = int(g.cfg.presets.size());
    g.cfg.presetIndex = idx >= 0 ? std::min(idx, n - 1) : (g.cfg.presetIndex + 1) % n;
    Log(L"preset: %s", g.cfg.presets[g.cfg.presetIndex].name.c_str());
    UpdateTrayTip();
    ShowOsd();
}

void ReloadAll()
{
    LoadConfig(g.cfgPath, g.cfg);
    g.cfgStamp = FileStamp(g.cfgPath);
    g.shaderStamp = FileStamp(g.shaderPath);
    if (g.dev)
        g.renderer.Reload();
    RegisterMenuKey();
    UpdateTrayTip();
    ShowOsd();
}

// Toggles menu: every switch in one place, current state checked. Opened by
// the menu key (Insert by default) at the centre of the screen, or from the
// tray icon at the cursor.
void ShowToggleMenu(bool atCursor)
{
    auto Flag = [](bool on) { return UINT(MF_STRING | (on ? MF_CHECKED : 0)); };
    HMENU m = CreatePopupMenu();

    HMENU looks = CreatePopupMenu();
    for (size_t i = 0; i < g.cfg.presets.size(); i++)
        AppendMenuW(looks, Flag(int(i) == g.cfg.presetIndex), ID_PRESET0 + i, g.cfg.presets[i].name.c_str());
    HMENU beams = CreatePopupMenu();
    for (int i = 0; i < int(_countof(kBeamLevels)); i++)
        AppendMenuW(beams, Flag(i == g.beamLevel), ID_BEAM0 + i, kBeamLevels[i].name);
    HMENU layouts = CreatePopupMenu();
    for (int i = 0; i < int(_countof(kLayouts)); i++)
    {
        std::wstring name = std::to_wstring(i) + L": " + kLayouts[i].name;
        AppendMenuW(layouts, Flag(i == CurrentLayout()), ID_LAYOUT0 + i, name.c_str());
    }

    AppendMenuW(m, Flag(g.enabled), ID_ENABLE, L"Phosphor on\tCtrl+Alt+Shift+C");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)looks, L"Look\tCtrl+Alt+Shift+P");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)beams, L"Beam sharpness\tCtrl+Alt+Shift+B");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, Flag(AutoResOn()), ID_AUTORES, L"Auto resolution detection\tCtrl+Alt+Shift+A");
    AppendMenuW(m, Flag(MotionOn()), ID_MOTION, L"CRT motion (experimental)\tCtrl+Alt+Shift+M");
    AppendMenuW(m, Flag(g.split > 0), ID_SPLIT, L"Split compare (raw | Phosphor)\tCtrl+Alt+Shift+S");
    AppendMenuW(m, Flag(!g.characterOff), ID_CHARACTER, L"Glass veil + analog softness\tCtrl+Alt+Shift+G");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)layouts, L"Colour fringe fix\tCtrl+Alt+Shift+L");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPENCFG, L"Edit config...");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload config + shader\tCtrl+Alt+Shift+R");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Quit Phosphor\tCtrl+Alt+Shift+Q");

    POINT pt;
    UINT align = TPM_RIGHTBUTTON;
    if (atCursor)
        GetCursorPos(&pt);
    else
    {
        pt = {(g.disp.rect.left + g.disp.rect.right) / 2, (g.disp.rect.top + g.disp.rect.bottom) / 2};
        align |= TPM_CENTERALIGN | TPM_VCENTERALIGN;
    }
    SetForegroundWindow(g.ctrl);
    TrackPopupMenu(m, align, pt.x, pt.y, 0, g.ctrl, nullptr);
    PostMessageW(g.ctrl, WM_NULL, 0, 0);
    DestroyMenu(m); // also destroys the submenus
}

UINT MenuVirtualKey(const std::wstring& name);

// (Re)claim the menu key; it's a plain key, so other apps stop receiving it.
void RegisterMenuKey()
{
    UINT vk = MenuVirtualKey(g.cfg.menuKey);
    if (vk == g.menuVk || !g.ctrl)
        return;
    if (g.menuVk)
        UnregisterHotKey(g.ctrl, HK_MENU);
    g.menuVk = 0;
    if (vk && RegisterHotKey(g.ctrl, HK_MENU, MOD_NOREPEAT, vk))
        g.menuVk = vk;
    else if (vk)
        Log(L"warning: menu key '%s' is taken by another app", g.cfg.menuKey.c_str());
}

// Virtual-key code for the menu_key setting.
UINT MenuVirtualKey(const std::wstring& name)
{
    std::wstring n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    static const std::pair<const wchar_t*, UINT> keys[] = {
        {L"insert", VK_INSERT}, {L"home", VK_HOME},   {L"end", VK_END},         {L"pageup", VK_PRIOR},
        {L"pagedown", VK_NEXT}, {L"pause", VK_PAUSE}, {L"scrolllock", VK_SCROLL}};
    for (auto& k : keys)
        if (n == k.first)
            return k.second;
    if (n.size() >= 2 && n[0] == L'f')
    {
        int f = _wtoi(n.c_str() + 1);
        if (f >= 1 && f <= 24)
            return VK_F1 + f - 1;
    }
    return 0;
}

void Command(UINT id)
{
    if (id == ID_ENABLE)
        SetEnabled(!g.enabled);
    else if (id == ID_AUTORES)
    {
        g.autoRes = AutoResOn() ? 0 : 1;
        if (!g.autoRes)
            g.autoW = g.autoH = 0;
        g.dirty = true;
        ShowOsd();
    }
    else if (id == ID_MOTION)
    {
        g.motion = MotionOn() ? 0 : 1;
        g.lastField = -1;
        ShowOsd();
    }
    else if (id == ID_CHARACTER)
    {
        g.characterOff = !g.characterOff;
        ShowOsd();
    }
    else if (id >= ID_BEAM0 && id < ID_BEAM0 + _countof(kBeamLevels))
    {
        g.beamLevel = int(id - ID_BEAM0);
        ShowOsd();
    }
    else if (id >= ID_LAYOUT0 && id < ID_LAYOUT0 + _countof(kLayouts))
    {
        g.layout = int(id - ID_LAYOUT0);
        ShowOsd(6000);
    }
    else if (id == ID_SPLIT)
    {
        g.split = g.split > 0 ? 0.0f : 0.5f;
        ShowOsd();
    }
    else if (id == ID_RELOAD)
        ReloadAll();
    else if (id == ID_OPENCFG)
        ShellExecuteW(nullptr, L"open", L"notepad.exe", g.cfgPath.c_str(), nullptr, SW_SHOWNORMAL);
    else if (id == ID_EXIT)
        g.running = false;
    else if (id >= ID_PRESET0 && id < ID_PRESET0 + g.cfg.presets.size())
        NextPreset(int(id - ID_PRESET0));
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK CtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_HOTKEY:
        switch (wp)
        {
        case HK_TOGGLE: Command(ID_ENABLE); break;
        case HK_SPLIT: Command(ID_SPLIT); break;
        case HK_PRESET: NextPreset(); break;
        case HK_MOTION:
            g.motion = MotionOn() ? 0 : 1;
            g.lastField = -1;
            Log(L"motion: %s, K=%d, refresh %.3f ms", g.motion ? L"on" : L"off", MotionK(), g.refreshMs);
            ShowOsd();
            break;
        case HK_DEBLUR:
            g.deblur = DeblurOn() ? 0 : 1;
            Log(L"deblur: %s", g.deblur ? L"on" : L"off");
            ShowOsd();
            break;
        case HK_VECTORS:
            g.vectors = !g.vectors;
            g.dirty = true;
            break;
        case HK_MENU:
            ShowToggleMenu(false);
            break;
        case HK_AUTORES:
            g.autoRes = AutoResOn() ? 0 : 1;
            if (!g.autoRes)
                g.autoW = g.autoH = 0;
            g.dirty = true;
            ShowOsd();
            break;
        case HK_CHARACTER:
            g.characterOff = !g.characterOff;
            ShowOsd();
            break;
        case HK_LAYOUT:
            g.layout = (CurrentLayout() + 1) % int(_countof(kLayouts));
            Log(L"subpixel layout %d: %s", g.layout, kLayouts[g.layout].name);
            ShowOsd(10000);
            break;
        case HK_BEAM:
            g.beamLevel = (g.beamLevel + 1) % int(_countof(kBeamLevels));
            Log(L"beam: %s", kBeamLevels[g.beamLevel].name);
            ShowOsd();
            break;
        case HK_RELOAD: ReloadAll(); break;
        case HK_QUIT: g.running = false; break;
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowToggleMenu(true);
        return 0;
    case WM_COMMAND:
        Command(LOWORD(wp));
        return 0;
    case WM_DISPLAYCHANGE:
        if (g.enabled)
            g.needSetup = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunOverlay(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = L"PhosphorOverlay";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    wc.lpfnWndProc = CtrlProc;
    wc.lpszClassName = L"PhosphorCtrl";
    RegisterClassW(&wc);

    g.ctrl = CreateWindowExW(0, L"PhosphorCtrl", L"Phosphor", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g.overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                    WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
                                L"PhosphorOverlay", L"Phosphor", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!g.ctrl || !g.overlay)
        return 1;
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(g.overlay, WDA_EXCLUDEFROMCAPTURE))
    {
        Log(L"SetWindowDisplayAffinity failed (needs Windows 10 2004+)");
        return 1;
    }

    UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    RegisterHotKey(g.ctrl, HK_TOGGLE, mods, 'C');
    RegisterHotKey(g.ctrl, HK_SPLIT, mods, 'S');
    RegisterHotKey(g.ctrl, HK_PRESET, mods, 'P');
    RegisterHotKey(g.ctrl, HK_RELOAD, mods, 'R');
    RegisterHotKey(g.ctrl, HK_BEAM, mods, 'B');
    RegisterHotKey(g.ctrl, HK_LAYOUT, mods, 'L');
    RegisterHotKey(g.ctrl, HK_MOTION, mods, 'M');
    RegisterHotKey(g.ctrl, HK_CHARACTER, mods, 'G');
    RegisterHotKey(g.ctrl, HK_AUTORES, mods, 'A');
    RegisterMenuKey();
#if PHOSPHOR_DEBLUR
    RegisterHotKey(g.ctrl, HK_DEBLUR, mods, 'D');
    RegisterHotKey(g.ctrl, HK_VECTORS, mods, 'V');
#endif
    if (!RegisterHotKey(g.ctrl, HK_QUIT, mods, 'Q'))
        Log(L"warning: quit hotkey Ctrl+Alt+Shift+Q unavailable");

    g.tray.cbSize = sizeof(g.tray);
    g.tray.hWnd = g.ctrl;
    g.tray.uID = 1;
    g.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.tray.uCallbackMessage = WM_TRAY;
    g.tray.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), 0);
    if (!g.tray.hIcon)
        g.tray.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g.tray.szTip, L"Phosphor");
    Shell_NotifyIconW(NIM_ADD, &g.tray);
    UpdateTrayTip();

    gDetector.Start();
    while (g.running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                g.running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.running)
            break;

        DWORD now = GetTickCount();
        if (g.osdInfo && now >= g.osdUntil)
        {
            RebuildOsd(); // info panel timed out; keep split labels if any
            g.dirty = true;
        }
        if (now - g.lastCheck > 500)
        {
            g.lastCheck = now;
            std::wstring flag = ExeDir() + L"\\dump.flag";
            if (g.haveFrame && GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileW(flag.c_str());
                DumpFrame(ExeDir() + L"\\dump.png");
            }
            gDetector.debug = g.cfg.detectDebug;
            if (FileStamp(g.cfgPath) != g.cfgStamp || FileStamp(g.shaderPath) != g.shaderStamp)
                ReloadAll();
        }

        if (!g.enabled || g.needSetup || !g.dup)
        {
            if (g.enabled && now - g.lastSetupTry > 1000)
            {
                g.lastSetupTry = now;
                if (!Setup())
                    Teardown();
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }

        if (now - g.lastAppCheck > 300)
        {
            g.lastAppCheck = now;
            CheckApp();
        }
        // Resolution detection: probe the raw desktop twice a second while it
        // changes; the analysis runs on a worker thread.
        if (AutoResOn() && g.haveFrame && g.dev)
        {
            std::vector<float> lines;
            UINT pw, ph;
            if (g.renderer.ReadProbe(lines, pw, ph))
                gDetector.Submit(std::move(lines), int(pw), int(ph));
            else if (g.framesSinceProbe && now - g.lastProbe > 500 && !gDetector.Busy())
            {
                g.renderer.Probe(g.frame.Get(), g.w, g.h);
                g.lastProbe = now;
                g.framesSinceProbe = false;
            }
            DetectResult dr;
            if (gDetector.Poll(dr))
            {
                g.autoW = dr.sourceW;
                g.autoH = dr.sourceH;
                Log(L"auto resolution: %s", dr.sourceH ? dr.detail.c_str() : L"native");
                g.dirty = true;
                ShowOsd(4000);
            }
        }
        if (now - g.lastTopmost > 1000)
        {
            g.lastTopmost = now;
            SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // Rolling scan presents every refresh (Present paces the loop), so it
        // only polls for new desktop frames; otherwise wait for one.
        bool motion = MotionOn();
        DXGI_OUTDUPL_FRAME_INFO fi;
        ComPtr<IDXGIResource> res;
        HRESULT hr = g.dup->AcquireNextFrame(motion ? 0 : 8, &fi, &res);
        if (FAILED(hr) && hr != DXGI_ERROR_WAIT_TIMEOUT)
        {
            Log(L"AcquireNextFrame 0x%08x, reinitialising", hr);
            g.needSetup = true;
            continue;
        }
        if (SUCCEEDED(hr))
        {
            if (fi.LastPresentTime.QuadPart != 0 || !g.haveFrame)
            {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(res.As(&tex)))
                {
                    g.ctx->CopyResource(g.frame.Get(), tex.Get());
                    g.haveFrame = true;
                    g.dirty = true;
                    g.newFrame = true;
                    g.motionShown = true;
                    g.framesSinceProbe = true;
                    g.lastCaptureQpc = Qpc();
                }
            }
            g.dup->ReleaseFrame();
        }
        if (!g.haveFrame)
            continue;
        if (motion)
            MotionStep();
        else
        {
            // Content stopped moving: redraw once with zero motion so a paused
            // image is never left pre-sharpened. (Comparing the frame with
            // itself measures no motion.)
            if (DeblurOn() && g.motionShown && !g.newFrame)
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                if (double(Qpc() - g.lastCaptureQpc) * 1000.0 / f.QuadPart > 3.0 * g.refreshMs)
                {
                    g.motionShown = false;
                    g.newFrame = true;
                    g.dirty = true;
                }
            }
            if (g.dirty)
                RenderPresent();
        }
    }

    gDetector.Stop();
    Teardown();
    Shell_NotifyIconW(NIM_DELETE, &g.tray);
    DestroyWindow(g.overlay);
    DestroyWindow(g.ctrl);
    return 0;
}

// Offline: run the same pipeline on an image file.
int RunImage(const std::wstring& in, const std::wstring& out, const std::wstring& preset, float widthMm,
             const std::wstring& source, int bench, int layout, const std::wstring& prevPath, float deblur,
             bool vectors)
{
    std::vector<uint8_t> px;
    UINT w, h;
    if (!LoadImageBGRA(in, px, w, h))
    {
        Log(L"cannot load %s", in.c_str());
        return 1;
    }
    // Use the GPU with the most dedicated memory (the one driving the OLED).
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> best;
    SIZE_T bestMem = 0;
    for (UINT i = 0;; i++)
    {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestMem)
        {
            bestMem = d.DedicatedVideoMemory;
            best = a;
        }
    }
    if (best)
    {
        DXGI_ADAPTER_DESC1 d;
        best->GetDesc1(&d);
        Log(L"gpu: %s", d.Description);
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(best.Get(), best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)))
        return 1;
    if (!g.renderer.Init(g.dev.Get(), g.shaderPath))
        return 1;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> src, dst, staging;
    g.dev->CreateTexture2D(&td, &sd, &src);
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    g.dev->CreateTexture2D(&td, nullptr, &dst);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g.dev->CreateTexture2D(&td, nullptr, &staging);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(dst.Get(), nullptr, &rtv);

    for (size_t i = 0; i < g.cfg.presets.size(); i++)
        if (_wcsicmp(g.cfg.presets[i].name.c_str(), preset.c_str()) == 0)
            g.cfg.presetIndex = int(i);

    FrameParams fp;
    fp.look = g.cfg.look();
    if (!source.empty())
    {
        size_t x = source.find(L'x');
        fp.look.sourceW = (float)_wtof(source.substr(0, x).c_str());
        fp.look.sourceH = x == std::wstring::npos ? 0.0f : (float)_wtof(source.substr(x + 1).c_str());
        fp.look.lines = 0;
    }
    fp.outputMode = 0;
    fp.outputBits = 8;
    if (layout > 0 && layout < int(_countof(kLayouts)))
        for (int c = 0; c < 3; c++)
        {
            fp.subX[c] = kLayouts[layout].x[c];
            fp.subY[c] = kLayouts[layout].y[c];
        }
    fp.peak = 1.0f;
    float mm = widthMm > 0 ? widthMm : (g.cfg.panelWidthMm > 0 ? g.cfg.panelWidthMm : 697.0f);
    fp.pixelMm = mm / float(w); // image spans the full panel width
    Log(L"image: %ux%u preset '%s' pixel %.4f mm", w, h, g.cfg.presets[g.cfg.presetIndex].name.c_str(), fp.pixelMm);
    fp.deblur = deblur;
    fp.motionDebug = vectors;
    fp.peak = deblur > 0 ? 2.0f : 1.0f;
    if (!prevPath.empty())
    {
        // Previous frame first, so the current one is measured against it.
        std::vector<uint8_t> ppx;
        UINT pw, ph;
        if (LoadImageBGRA(prevPath, ppx, pw, ph) && pw == w && ph == h)
        {
            D3D11_TEXTURE2D_DESC pd{};
            src->GetDesc(&pd);
            D3D11_SUBRESOURCE_DATA psd{ppx.data(), w * 4, 0};
            ComPtr<ID3D11Texture2D> prevTex;
            g.dev->CreateTexture2D(&pd, &psd, &prevTex);
            fp.newFrame = true;
            g.renderer.Render(prevTex.Get(), rtv.Get(), w, h, fp);
        }
        else
            Log(L"cannot use --prev %s (missing or different size)", prevPath.c_str());
    }
    fp.newFrame = true;
    g.renderer.ProfileNext();
    g.renderer.Render(src.Get(), rtv.Get(), w, h, fp);
    if (!prevPath.empty())
    {
        // Debug: most common measured vectors among confident blocks.
        std::vector<float> mv;
        UINT mw, mh;
        if (g.renderer.ReadMotion(mv, mw, mh))
        {
            std::map<std::pair<int, int>, int> hist;
            int confident = 0;
            for (size_t i = 0; i < size_t(mw) * mh; i++)
                if (mv[i * 4 + 2] > 0.3f)
                {
                    confident++;
                    hist[{int(std::lround(mv[i * 4])), int(std::lround(mv[i * 4 + 1]))}]++;
                }
            std::vector<std::pair<int, std::pair<int, int>>> top;
            for (auto& e : hist)
                top.push_back({e.second, e.first});
            std::sort(top.rbegin(), top.rend());
            Log(L"motion: %u x %u blocks, %d confident", mw, mh, confident);
            for (size_t i = 0; i < top.size() && i < 6; i++)
                Log(L"  vector (%d, %d): %d blocks", top[i].second.first, top[i].second.second, top[i].first);
        }
    }
    if (bench > 0)
    {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> q;
        g.dev->CreateQuery(&qd, &q);
        g.ctx->End(q.Get());
        while (g.ctx->GetData(q.Get(), nullptr, 0, 0) == S_FALSE) {}
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < bench; i++)
            g.renderer.Render(src.Get(), rtv.Get(), w, h, fp);
        g.ctx->End(q.Get());
        while (g.ctx->GetData(q.Get(), nullptr, 0, 0) == S_FALSE) {}
        QueryPerformanceCounter(&t1);
        double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / bench;
        Log(L"bench: %.3f ms/frame (%.0f fps max)", ms, 1000.0 / ms);
        g.renderer.ProfileNext();
        g.renderer.Render(src.Get(), rtv.Get(), w, h, fp);
    }
    g.ctx->CopyResource(staging.Get(), dst.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return 1;
    bool ok = SavePngBGRA(out, (const uint8_t*)m.pData, w, h, m.RowPitch);
    g.ctx->Unmap(staging.Get(), 0);
    Log(ok ? L"wrote %s" : L"failed to write %s", out.c_str());
    return ok ? 0 : 1;
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    g.cfgPath = FindAsset(L"phosphor.ini");
    g.shaderPath = FindAsset(L"shaders\\crt.hlsl");
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (args[i] == L"--config")
        {
            g.cfgPath = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            break;
        }
    if (!args.empty())
        EnableConsoleLog();
    LoadConfig(g.cfgPath, g.cfg);
    g.cfgStamp = FileStamp(g.cfgPath);
    g.shaderStamp = FileStamp(g.shaderPath);

    if (args.size() >= 3 && args[0] == L"--image")
    {
        std::wstring preset, source;
        float widthMm = 0;
        int bench = 0, layout = 0;
        std::wstring prevPath;
        float deblur = 0;
        bool vectors = false;
        for (size_t i = 3; i + 1 < args.size(); i++)
        {
            if (args[i] == L"--bench") bench = _wtoi(args[i + 1].c_str());
            if (args[i] == L"--layout") layout = _wtoi(args[i + 1].c_str());
            if (args[i] == L"--prev") prevPath = args[i + 1];
            if (args[i] == L"--deblur") deblur = (float)_wtof(args[i + 1].c_str());
            if (args[i] == L"--vectors") vectors = args[i + 1] == L"1";
            if (args[i] == L"--preset") preset = args[i + 1];
            if (args[i] == L"--width-mm") widthMm = (float)_wtof(args[i + 1].c_str());
            if (args[i] == L"--source") source = args[i + 1];
        }
        return RunImage(args[1], args[2], preset, widthMm, source, bench, layout, prevPath, deblur, vectors);
    }
    if (!args.empty() && args[0] == L"--list")
    {
        DisplayTarget d;
        return FindDisplay(g.cfg.monitor, d) ? 0 : 1;
    }
    if (args.size() >= 2 && args[0] == L"--detect")
    {
        // Offline: run the resolution detector on an image.
        std::vector<uint8_t> px;
        UINT w, h;
        if (!LoadImageBGRA(args[1], px, w, h))
            return 1;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)) ||
            !g.renderer.Init(g.dev.Get(), g.shaderPath))
            return 1;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
        ComPtr<ID3D11Texture2D> tex;
        g.dev->CreateTexture2D(&td, &sd, &tex);
        g.renderer.Probe(tex.Get(), w, h);
        std::vector<float> lines;
        UINT lw, lh;
        if (!g.renderer.ReadProbe(lines, lw, lh, true))
            return 1;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        DetectResult r = AnalyzeLines(lines.data(), int(lw), int(lh));
        QueryPerformanceCounter(&t1);
        Log(L"detect %s: %s  (%.1f ms)", args[1].c_str(), r.detail.c_str(),
            double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        return 0;
    }
    if (args.size() >= 2 && args[0] == L"--pattern")
    {
        UINT w = args.size() >= 4 ? _wtoi(args[2].c_str()) : 3840;
        UINT h = args.size() >= 4 ? _wtoi(args[3].c_str()) : 2160;
        bool ok = MakePattern(args[1], w, h);
        Log(ok ? L"wrote %s" : L"failed %s", args[1].c_str());
        return ok ? 0 : 1;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"PhosphorCrtSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    int rc = RunOverlay(inst);
    CloseHandle(mutex);
    return rc;
}
