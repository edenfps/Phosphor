#include "config.h"
#include "common.h"
#include <algorithm>
#include <fstream>
#include <sstream>

static std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos)
        return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// "native" -> 0x0, "2560x1440" -> 2560x1440
static void ParseSource(const std::wstring& v, float& w, float& h)
{
    w = h = 0;
    if (Lower(v) == L"native")
        return;
    size_t x = Lower(v).find(L'x');
    if (x == std::wstring::npos)
        return;
    w = (float)_wtof(v.substr(0, x).c_str());
    h = (float)_wtof(v.substr(x + 1).c_str());
}

static void SetLookKey(Look& l, const std::wstring& k, const std::wstring& v)
{
    float f = (float)_wtof(v.c_str());
    if (k == L"gamma") l.crtGamma = f;
    else if (k == L"phosphors")
    {
        std::wstring p = Lower(v);
        l.phosphors = p == L"srgb" ? 0 : p == L"p22" ? 1 : p == L"trinitron" ? 2 : 3;
    }
    else if (k == L"white_k") l.whiteK = f;
    else if (k == L"source") ParseSource(v, l.sourceW, l.sourceH);
    else if (k == L"lines") l.lines = f;
    else if (k == L"spot_min_mm") l.spotMinMm = f;
    else if (k == L"spot_max_mm") l.spotMaxMm = f;
    else if (k == L"spot_power") l.spotPower = f;
    else if (k == L"spot_aspect") l.spotAspect = f;
    else if (k == L"spot_tail") l.spotTail = f;
    else if (k == L"spot_tail_scale") l.spotTailScale = f;
    else if (k == L"diffusion") l.diffusion = f;
    else if (k == L"diffusion_mm") l.diffusionMm = f;
    else if (k == L"halation") l.halation = f;
    else if (k == L"glass_mm") l.glassMm = f;
    else if (k == L"veil") l.veil = f;
    else if (k == L"veil_mm") l.veilMm = f;
    else if (k == L"analog_softness_mm") l.analogSoftMm = f;
    else if (k == L"highlight_boost") l.highlightBoost = f;
    else if (k == L"mask") l.mask = Lower(v) == L"grille" ? 1 : 0;
    else if (k == L"mask_strength") l.maskStrength = f;
    else if (k == L"mask_pitch_px") l.maskPitchPx = f;
    else if (k == L"brightness") l.brightness = f;
    else Log(L"config: unknown preset key '%s'", k.c_str());
}

int Config::FindPreset(const std::wstring& name) const
{
    for (size_t i = 0; i < presets.size(); i++)
        if (Lower(presets[i].name) == Lower(name))
            return int(i);
    return -1;
}

bool LoadConfig(const std::wstring& path, Config& cfg)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        Log(L"config: cannot open %s, using defaults", path.c_str());
        cfg = Config{};
        cfg.presets.push_back({L"Default", Look{}});
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::wstring text = Widen(ss.str());

    Config c;
    std::wstring startPreset;
    Preset* cur = nullptr;
    AppProfile* app = nullptr;
    bool general = false;
    std::wstringstream lines(text);
    std::wstring line;
    while (std::getline(lines, line))
    {
        size_t hash = line.find_first_of(L";#");
        if (hash != std::wstring::npos)
            line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty())
            continue;
        if (line.front() == L'[' && line.back() == L']')
        {
            std::wstring sec = Trim(line.substr(1, line.size() - 2));
            std::wstring low = Lower(sec);
            general = low == L"general";
            cur = nullptr;
            app = nullptr;
            if (low.rfind(L"preset", 0) == 0)
            {
                // Each preset starts from the defaults in config.h.
                c.presets.push_back({Trim(sec.substr(6)), Look{}, L""});
                cur = &c.presets.back();
            }
            else if (low.rfind(L"app", 0) == 0)
            {
                c.apps.push_back({Lower(Trim(sec.substr(3)))});
                app = &c.apps.back();
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;
        std::wstring k = Lower(Trim(line.substr(0, eq)));
        std::wstring v = Trim(line.substr(eq + 1));
        if (cur && k == L"description")
            cur->description = v;
        else if (cur)
            SetLookKey(cur->look, k, v);
        else if (app)
        {
            if (k == L"preset") app->preset = v;
            else if (k == L"source") ParseSource(v, app->sourceW, app->sourceH);
            else Log(L"config: unknown app key '%s'", k.c_str());
        }
        else if (general)
        {
            if (k == L"monitor") c.monitor = v;
            else if (k == L"panel_width_mm") c.panelWidthMm = (float)_wtof(v.c_str());
            else if (k == L"sdr_white_nits") c.sdrWhiteNits = (float)_wtof(v.c_str());
            else if (k == L"peak_nits") c.peakNits = (float)_wtof(v.c_str());
            else if (k == L"sdr_output") c.sdrOutput = Lower(v) == L"gamma22" ? 1 : 0;
            else if (k == L"preset") startPreset = v;
            else if (k == L"subpixel_layout") c.subpixelLayout = _wtoi(v.c_str());
            else if (k == L"auto_resolution") c.autoResolution = Lower(v) == L"on" || v == L"1";
            else if (k == L"detect_debug") c.detectDebug = Lower(v) == L"on" || v == L"1";
            else if (k == L"motion") c.motion = Lower(v) == L"on" || v == L"1";
            else if (k == L"crt_hz") c.crtHz = (float)_wtof(v.c_str());
            else if (k == L"motion_gain") c.motionGain = (float)_wtof(v.c_str());
            else if (k == L"afterglow_ms") c.afterglowMs = (float)_wtof(v.c_str());
            else if (k == L"scan_softness") c.scanSoftness = (float)_wtof(v.c_str());
            else if (k == L"motion_mode") c.motionMode = Lower(v) == L"rolling" ? 0 : 1;
            else if (k == L"change_threshold") c.changeThreshold = (float)_wtof(v.c_str());
            else if (k == L"mask_decay") c.maskDecay = (float)_wtof(v.c_str());
            else if (k == L"deblur") c.deblur = Lower(v) == L"on" || v == L"1";
            else if (k == L"deblur_strength") c.deblurStrength = (float)_wtof(v.c_str());
            else if (k == L"deblur_max_px") c.deblurMaxPx = (float)_wtof(v.c_str());
            else Log(L"config: unknown general key '%s'", k.c_str());
        }
    }
    if (c.presets.empty())
        c.presets.push_back({L"Default", Look{}});
    c.presetIndex = std::max(0, c.FindPreset(startPreset));

    // Keep the user's current preset across hot reloads.
    if (!cfg.presets.empty())
    {
        int keep = c.FindPreset(cfg.presets[cfg.presetIndex].name);
        if (keep >= 0)
            c.presetIndex = keep;
    }
    cfg = c;
    Log(L"config: loaded %zu presets, %zu app profiles, active '%s'", cfg.presets.size(), cfg.apps.size(),
        cfg.presets[cfg.presetIndex].name.c_str());
    return true;
}
