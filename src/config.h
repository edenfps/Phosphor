#pragma once
#include <string>
#include <vector>

// Physical description of the virtual tube. Sizes are millimetres on the
// glass so the look matches a real CRT at the same viewing distance,
// independent of the panel's pixel density.
struct Look
{
    float crtGamma = 2.4f;        // tube response
    int phosphors = 3;            // 0 sRGB/Rec.709, 1 P22, 2 Trinitron, 3 Diamondtron
    float whiteK = 6500.0f;       // white point colour temperature
    float sourceW = 0.0f;         // content resolution the tube scans; 0 = native
    float sourceH = 0.0f;
    float lines = 0.0f;           // shorthand: scanline count at native horizontal res
    float spotMinMm = 0.03f;      // beam spot core sigma at black
    float spotMaxMm = 0.06f;      // beam spot core sigma at full drive
    float spotPower = 1.0f;
    float spotAspect = 1.0f;      // horizontal / vertical
    float spotTail = 0.02f;       // energy in the spot's skirt
    float spotTailScale = 3.5f;   // skirt width / core width
    float diffusion = 0.008f;     // short range scatter (phosphor layer + glass)
    float diffusionMm = 0.6f;     // its radius (sigma)
    float halation = 0.008f;      // light returned by the faceplate (ring)
    float glassMm = 13.0f;        // faceplate thickness
    float veil = 0.0f;            // glass veiling glow: very wide faceplate scatter (energy fraction)
    float veilMm = 40.0f;         // its radius (sigma)
    float analogSoftMm = 0.0f;    // extra horizontal softness of the analog video path (sigma)
    float highlightBoost = 1.0f;  // HDR: small-area peak vs full-field white
    int mask = 0;                 // 0 none, 1 aperture grille
    float maskStrength = 0.5f;
    float maskPitchPx = 3.0f;
    float brightness = 1.0f;
};

struct Preset
{
    std::wstring name;
    Look look;
    std::wstring description; // shown on screen when the preset is selected
};

// Applied while the named executable has focus.
struct AppProfile
{
    std::wstring exe;      // lower case, e.g. "game.exe"
    std::wstring preset;   // "" = keep current
    float sourceW = -1.0f; // -1 = keep preset's
    float sourceH = -1.0f;
};

struct Config
{
    std::wstring monitor;        // "" = auto (largest), index, or name substring
    float panelWidthMm = 0;      // 0 = from EDID
    float sdrWhiteNits = 0;      // 0 = Windows SDR content brightness
    float peakNits = 0;          // 0 = from display
    int sdrOutput = 0;           // SDR desktop: 0 sRGB panel, 1 gamma 2.2 panel
    int subpixelLayout = 0;      // index into the panel layout table (main.cpp), 0 = off
    bool autoResolution = false; // detect stretched lower-res content and redraw it at its real resolution
    bool detectDebug = false;    // log every resolution analysis
    std::wstring menuKey = L"Insert"; // opens the toggles menu
    // Rolling scan (CRT motion clarity)
    bool motion = false;         // start with it on
    float crtHz = 80.0f;         // virtual CRT refresh; panel Hz / crtHz refreshes per field
    float motionGain = 1.0f;     // < 1 trades brightness for less blur on bright pixels
    float afterglowMs = 1.0f;    // phosphor decay time constant
    float scanSoftness = 0.75f;  // slice-edge blend width, in refreshes (0 = hard slices)
    int motionMode = 0;          // 0 rolling scan, 1 motion-adaptive flash (only changing areas strobe)
    float changeThreshold = 0.02f; // luma change (0-1) that counts as motion
    float maskDecay = 0.5f;      // how fast regions return to steady once they stop changing
    // Motion pre-deblur
    bool deblur = false;         // start with it on
    float deblurStrength = 1.0f; // 1 = first-order inverse of the eye's smear
    float deblurMaxPx = 48.0f;   // largest per-frame motion compensated
    std::vector<Preset> presets;
    std::vector<AppProfile> apps;
    int presetIndex = 0;

    const Look& look() const { return presets[presetIndex].look; }
    int FindPreset(const std::wstring& name) const;
};

bool LoadConfig(const std::wstring& path, Config& cfg);
