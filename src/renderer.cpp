#include "renderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
const double kPi = 3.14159265358979;

struct FrameCB
{
    float srcW, srcH, pitchX, pitchY;
    float inputMode, sdrWhiteScale, crtGamma, outputMode;
    float spotMinPx, spotMaxPx, spotPower, spotAspect;
    float spotTail, tailMix, aaX, aaY;
    float radiusH, radiusV, pad1, pad2;
    float diffusion, halation, nearMix, haloRings;
    float maskType, maskStrength, maskPitch, brightness;
    float peak, split, highlightBoost, aplKnee;
    float outputBits, fullW, fullH, pad;
    float gamut[3][4];
    float halo[32][4];
    float subX[4], subY[4];
    float motion0[4], motion1[4];
    float deblur0[4];
    float veil0[4];
};
static_assert(sizeof(FrameCB) % 16 == 0, "cbuffer size");

// ---- colorimetry: phosphor primaries + white point -> sRGB linear matrix

void Mul3(const double a[9], const double b[9], double o[9])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

bool Inv3(const double m[9], double o[9])
{
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-12)
        return false;
    double id = 1.0 / det;
    o[0] = (m[4] * m[8] - m[5] * m[7]) * id;
    o[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    o[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    o[3] = (m[5] * m[6] - m[3] * m[8]) * id;
    o[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    o[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    o[6] = (m[3] * m[7] - m[4] * m[6]) * id;
    o[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    o[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return true;
}

// RGB -> XYZ for given primaries (rx,ry,gx,gy,bx,by) and white (x,y), white Y = 1.
void RgbToXyz(const double prim[6], double wx, double wy, double out[9])
{
    double P[9];
    for (int i = 0; i < 3; i++)
    {
        double x = prim[i * 2], y = prim[i * 2 + 1];
        P[0 + i] = x / y;
        P[3 + i] = 1.0;
        P[6 + i] = (1 - x - y) / y;
    }
    double Pi[9];
    Inv3(P, Pi);
    double W[3] = {wx / wy, 1.0, (1 - wx - wy) / wy};
    double S[3];
    for (int r = 0; r < 3; r++)
        S[r] = Pi[r * 3] * W[0] + Pi[r * 3 + 1] * W[1] + Pi[r * 3 + 2] * W[2];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r * 3 + c] = P[r * 3 + c] * S[c];
}

// CIE daylight locus.
void KelvinToXy(double T, double& x, double& y)
{
    if (std::fabs(T - 6500) < 1)
    {
        x = 0.3127;
        y = 0.3290;
        return;
    }
    T = std::clamp(T, 4000.0, 25000.0);
    if (T <= 7000)
        x = -4.6070e9 / (T * T * T) + 2.9678e6 / (T * T) + 0.09911e3 / T + 0.244063;
    else
        x = -2.0064e9 / (T * T * T) + 1.9018e6 / (T * T) + 0.24748e3 / T + 0.237040;
    y = -3.0 * x * x + 2.870 * x - 0.275;
}

void GamutMatrix(int phosphors, float whiteK, float out[3][4])
{
    static const double sets[4][6] = {
        {0.640, 0.330, 0.300, 0.600, 0.150, 0.060}, // sRGB / Rec.709
        {0.630, 0.340, 0.310, 0.595, 0.155, 0.070}, // P22 / SMPTE-C
        {0.625, 0.340, 0.280, 0.595, 0.155, 0.070}, // Sony Trinitron (FW900 class)
        {0.625, 0.340, 0.285, 0.605, 0.150, 0.065}, // Mitsubishi Diamondtron NF
    };
    double wx, wy;
    KelvinToXy(whiteK, wx, wy);
    double crt[9], srgb[9], srgbInv[9], m[9];
    RgbToXyz(sets[std::clamp(phosphors, 0, 3)], wx, wy, crt);
    RgbToXyz(sets[0], 0.3127, 0.3290, srgb);
    Inv3(srgb, srgbInv);
    Mul3(srgbInv, crt, m);
    // Scale so full white never exceeds 1.0 in any channel (no clipping).
    double mx = 0;
    for (int r = 0; r < 3; r++)
        mx = std::max(mx, m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2]);
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
            out[r][c] = float(m[r * 3 + c] / mx);
        out[r][3] = 0;
    }
}

// Faceplate halation kernel. A Lambertian phosphor emits into glass of
// thickness t (n = 1.54). Rays steeper than the critical angle are totally
// reflected at the front surface and land at r = 2t tan(theta); the areal
// density there is proportional to cos^4(theta). Below the critical angle only
// a residual Fresnel/AR reflection returns. A second bounce off the phosphor
// layer adds a dimmer, wider ring. Returns ring count.
int HaloKernel(float glassPx, float out[32][4])
{
    const int K = 32;
    const double n = 1.54;
    const double thc = std::asin(1.0 / n);
    const double t = std::max(double(glassPx), 1.0);
    const double rMax = 2.0 * t * std::tan(78.0 * kPi / 180.0);
    const double dr = rMax / K;
    double w[K], sum = 0;
    for (int k = 0; k < K; k++)
    {
        double r = (k + 0.5) * dr;
        double th1 = std::atan(r / (2.0 * t));
        double d1 = std::pow(std::cos(th1), 4) * (th1 > thc ? 1.0 : 0.02);
        double th2 = std::atan(r / (4.0 * t));
        double d2 = std::pow(std::cos(th2), 4) * (th2 > thc ? 1.0 : 0.02) * 0.25 * 0.35;
        w[k] = (d1 + d2) * 2.0 * kPi * r * dr;
        sum += w[k];
    }
    for (int k = 0; k < K; k++)
    {
        double r = (k + 0.5) * dr;
        // Pyramid level blurred enough for 24 taps around this ring and for the
        // ring spacing (level 4/5/6 sigma ~ 16/32/64 px).
        double needSigma = std::max(2.0 * kPi * r / 24.0 / 1.5, dr);
        out[k][0] = float(r);
        out[k][1] = float(w[k] / sum / 24.0);
        out[k][2] = needSigma <= 16 ? 0.0f : needSigma <= 32 ? 1.0f : 2.0f;
        out[k][3] = float(k * 2.39996323); // golden angle: no aligned spokes
    }
    return K;
}

ComPtr<ID3DBlob> Compile(const std::string& src, const std::wstring& path, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    std::string name;
    for (wchar_t c : path)
        name += char(c < 128 ? c : '?');
    HRESULT hr = D3DCompile(src.data(), src.size(), name.c_str(), nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        std::string msg = err ? (const char*)err->GetBufferPointer() : "unknown error";
        std::wstring w(msg.begin(), msg.end());
        Log(L"shader %S: %s", entry, w.c_str());
        return nullptr;
    }
    return code;
}
} // namespace

bool Renderer::Init(ID3D11Device* dev, const std::wstring& shaderPath)
{
    dev_ = dev;
    dev_->GetImmediateContext(&ctx_);
    shaderPath_ = shaderPath;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(FrameCB);
    if (FAILED(dev_->CreateBuffer(&bd, nullptr, &frameCB_)))
        return false;
    bd.ByteWidth = sizeof(PassCB);
    if (FAILED(dev_->CreateBuffer(&bd, nullptr, &passCB_)))
        return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev_->CreateSamplerState(&sd, &linear_)))
        return false;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    if (FAILED(dev_->CreateSamplerState(&sd, &linearBorder_))) // border colour defaults to 0
        return false;

    return Reload();
}

bool Renderer::Reload()
{
    std::ifstream in(shaderPath_, std::ios::binary);
    if (!in)
    {
        Log(L"shader: cannot open %s", shaderPath_.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();

    auto vsb = Compile(src, shaderPath_, "VS", "vs_5_0");
    if (!vsb)
        return false;
    ComPtr<ID3D11VertexShader> vs;
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);

    struct Entry { const char* name; ComPtr<ID3D11PixelShader>* dst; };
    Entry entries[] = {{"PS_Decode", &psDecode_}, {"PS_Resample", &psResample_}, {"PS_BeamH", &psBeamH_},
                       {"PS_BeamV", &psBeamV_},   {"PS_Down", &psDown_},         {"PS_Halo", &psHalo_},
                       {"PS_Final", &psFinal_},   {"PS_Scan", &psScan_},         {"PS_Luma", &psLuma_},
                       {"PS_LumaDown", &psLumaDown_}, {"PS_MotionSearch", &psMotionSearch_},
                       {"PS_Deblur", &psDeblur_}, {"PS_Copy", &psCopy_}, {"PS_ChangeMask", &psChangeMask_},
                       {"PS_ProbeLines", &psProbe_}};
    ComPtr<ID3D11PixelShader> built[_countof(entries)];
    for (size_t i = 0; i < _countof(entries); i++)
    {
        auto b = Compile(src, shaderPath_, entries[i].name, "ps_5_0");
        if (!b || FAILED(dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &built[i])))
            return false; // keep previous shaders running
    }
    vs_ = vs;
    for (size_t i = 0; i < _countof(entries); i++)
        *entries[i].dst = built[i];
    Log(L"shader: compiled %s", shaderPath_.c_str());
    return true;
}

void Renderer::SetOsd(const uint8_t* rg, UINT w, UINT h)
{
    osdSrv_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{rg, w * 2, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &sd, &tex)))
        dev_->CreateShaderResourceView(tex.Get(), nullptr, &osdSrv_);
}

bool Renderer::Ensure(Target& t, UINT w, UINT h)
{
    w = std::max(w, 1u);
    h = std::max(h, 1u);
    if (t.tex && t.w == w && t.h == h)
        return true;
    t = Target{};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.tex)) ||
        FAILED(dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)) ||
        FAILED(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)))
        return false;
    t.w = w;
    t.h = h;
    return true;
}

void Renderer::Pass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT dw, UINT dh,
                    std::initializer_list<ID3D11ShaderResourceView*> srvs, UINT texelW, UINT texelH,
                    const float* params)
{
    ID3D11ShaderResourceView* none[11] = {};
    ctx_->PSSetShaderResources(0, 11, none);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{0, 0, float(dw), float(dh), 0, 1};
    ctx_->RSSetViewports(1, &vp);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(passCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        PassCB cb{float(texelW), float(texelH), 1.0f / texelW, 1.0f / texelH,
                  float(dw), float(dh), 1.0f / dw, 1.0f / dh, {}};
        if (params)
            memcpy(cb.params, params, sizeof(cb.params));
        memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(passCB_.Get(), 0);
    }

    UINT n = 0;
    ID3D11ShaderResourceView* list[11] = {};
    for (auto s : srvs)
        list[n++] = s;
    ctx_->PSSetShaderResources(0, n, list);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->Draw(3, 0);
}

void Renderer::Mark(const wchar_t* name)
{
    if (!profile_)
        return;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
    ComPtr<ID3D11Query> q;
    dev_->CreateQuery(&qd, &q);
    ctx_->End(q.Get());
    marks_.push_back({name, q});
}

void Renderer::Render(ID3D11Texture2D* src, ID3D11RenderTargetView* out, UINT w, UINT h, const FrameParams& fp,
                      int firstStage, int lastStage)
{
    ComPtr<ID3D11Query> disjoint;
    if (profile_)
    {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &disjoint);
        ctx_->Begin(disjoint.Get());
        marks_.clear();
        Mark(L"start");
    }
    if (!psFinal_)
        return;
    if (srcTex_.Get() != src)
    {
        srcSrv_.Reset();
        srcTex_ = src;
        dev_->CreateShaderResourceView(src, nullptr, &srcSrv_);
    }
    const Look& L = fp.look;

    // Content resolution the virtual tube scans.
    UINT sw = w, sh = h;
    if (L.sourceW >= 1 && L.sourceH >= 1)
    {
        sw = std::min(UINT(L.sourceW + 0.5f), w);
        sh = std::min(UINT(L.sourceH + 0.5f), h);
    }
    else if (L.lines >= 1)
        sh = std::min(UINT(L.lines + 0.5f), h);
    bool resample = sw != w || sh != h;

    if (!Ensure(lin_, w, h) || (resample && !Ensure(source_, sw, sh)) || !Ensure(beamH_, w, sh) || !Ensure(beam_, w, h))
        return;
    UINT lw = w, lh = h;
    for (int i = 1; i <= kLevels; i++)
    {
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
        if (!Ensure(down_[i], lw, lh))
            return;
    }
    if (!Ensure(halo_, down_[kHaloLevel].w, down_[kHaloLevel].h))
        return;

    // ---- constants
    FrameCB cb{};
    cb.srcW = float(sw);
    cb.srcH = float(sh);
    cb.pitchX = float(w) / sw;
    cb.pitchY = float(h) / sh;
    cb.inputMode = float(fp.inputMode);
    cb.sdrWhiteScale = fp.sdrWhiteScale;
    cb.crtGamma = L.crtGamma;
    cb.outputMode = float(fp.outputMode);
    cb.spotMinPx = std::max(L.spotMinMm / fp.pixelMm, 0.05f);
    cb.spotMaxPx = std::max(L.spotMaxMm / fp.pixelMm, cb.spotMinPx);
    cb.spotPower = std::max(L.spotPower, 0.01f);
    cb.spotAspect = std::max(L.spotAspect, 0.1f);
    cb.spotTail = std::clamp(L.spotTail, 0.0f, 0.5f);
    // When scanlines/columns don't land on whole pixels, pre-smooth just enough
    // that the structure can't beat against the panel grid (moire).
    cb.aaX = cb.pitchX > 1.01f ? 0.3f : 0.0f;
    cb.aaY = cb.pitchY > 1.01f ? 0.45f : 0.0f;

    // Gather radius covers 3 sigma of the widest core, half a source pixel and
    // the largest subpixel offset.
    float subMaxX = 0, subMaxY = 0;
    for (int c = 0; c < 3; c++)
    {
        cb.subX[c] = std::clamp(fp.subX[c], -0.5f, 0.5f);
        cb.subY[c] = std::clamp(fp.subY[c], -0.5f, 0.5f);
        subMaxX = std::max(subMaxX, std::fabs(cb.subX[c]));
        subMaxY = std::max(subMaxY, std::fabs(cb.subY[c]));
    }
    float analogPx = std::max(L.analogSoftMm, 0.0f) / fp.pixelMm;
    float coreH = std::sqrt(std::pow(cb.spotMaxPx * cb.spotAspect, 2.0f) + cb.aaX * cb.aaX + analogPx * analogPx);
    cb.radiusH =
        std::clamp(std::ceil((3.0f * coreH + 0.5f * cb.pitchX + subMaxX) / cb.pitchX), 1.0f, 16.0f);
    float coreV = std::sqrt(cb.spotMaxPx * cb.spotMaxPx + cb.aaY * cb.aaY);
    cb.radiusV = std::clamp(std::ceil((3.0f * coreV + subMaxY) / cb.pitchY + 0.5f), 1.0f, 16.0f);
    // The skirt is a wide, current-independent blur: take it from the pyramid.
    float tailLvl = std::clamp(std::log2(std::max(cb.spotMaxPx * std::max(L.spotTailScale, 1.0f), 2.0f)), 1.0f,
                               float(kLevels - 1) - 0.001f);
    int tailLevel = int(tailLvl);
    cb.tailMix = tailLvl - tailLevel;

    cb.diffusion = std::clamp(L.diffusion, 0.0f, 1.0f);
    cb.halation = std::clamp(L.halation, 0.0f, 1.0f - cb.diffusion);
    // Diffusion radius -> pyramid levels (level k ~ sigma 2^k px).
    float lvl = std::clamp(std::log2(std::max(L.diffusionMm / fp.pixelMm, 2.0f)), 1.0f, float(kLevels - 1) - 0.001f);
    int nearLevel = int(lvl);
    cb.nearMix = lvl - nearLevel;
    cb.haloRings = float(HaloKernel(L.glassMm / fp.pixelMm, cb.halo));
    // Veiling glow takes whatever energy diffusion and halation leave.
    cb.veil0[0] = std::clamp(L.veil, 0.0f, 1.0f - cb.diffusion - cb.halation);
    float veilLvl = std::clamp(std::log2(std::max(L.veilMm / fp.pixelMm, 2.0f)), 1.0f, float(kLevels - 1) - 0.001f);
    int veilLevel = int(veilLvl);
    cb.veil0[1] = veilLvl - veilLevel;
    cb.veil0[2] = analogPx;

    cb.maskType = float(L.mask);
    cb.maskStrength = std::clamp(L.maskStrength, 0.0f, 1.0f);
    cb.maskPitch = std::max(L.maskPitchPx, 1.0f);
    cb.brightness = L.brightness;
    cb.peak = fp.peak;
    cb.split = fp.split;
    cb.highlightBoost = std::max(L.highlightBoost, 1.0f);
    cb.aplKnee = 0.5f;
    cb.outputBits = float(fp.outputBits);
    cb.fullW = float(w);
    cb.fullH = float(h);
    GamutMatrix(L.phosphors, L.whiteK, cb.gamut);

    // Motion pre-deblur: Final composes into a linear buffer, PS_Deblur encodes.
    bool deblur = fp.deblur > 0 && fp.outputMode != 3 && motionFine_.tex && lumaValid_;
    cb.deblur0[0] = fp.deblur;
    cb.deblur0[1] = fp.motionDebug ? 1.0f : 0.0f;
    cb.deblur0[2] = std::max(fp.deblurMaxPx, 1.0f);
    cb.deblur0[3] = deblur ? 1.0f : 0.0f;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(frameCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(frameCB_.Get(), 0);
    }

    // ---- passes
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {frameCB_.Get(), passCB_.Get()};
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* samplers[2] = {linear_.Get(), linearBorder_.Get()};
    ctx_->PSSetSamplers(0, 2, samplers);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->RSSetState(nullptr);

    if (firstStage == 0)
    {
        bool measure = fp.deblur > 0 && fp.newFrame && fp.outputMode != 3;
        bool mask = fp.motionMode == 1 && fp.outputMode == 3; // field render = new content
        if ((measure || mask) && BuildLuma(w, h))
        {
            if (measure)
                EstimateMotion();
            if (mask)
                UpdateChangeMask(true, fp);
        }
    }

    // Stage 0: decode + horizontal beam. Stage 1: vertical beam + glow.
    // Stage 2: final composite. The rolling scan runs one stage per refresh.
    if (firstStage <= 0)
    {
        Pass(psDecode_.Get(), lin_.rtv.Get(), w, h, {srcSrv_.Get()}, w, h);
        Mark(L"decode");

        ID3D11ShaderResourceView* beamSrc = lin_.srv.Get();
        if (resample)
        {
            Pass(psResample_.Get(), source_.rtv.Get(), sw, sh, {lin_.srv.Get()}, w, h);
            beamSrc = source_.srv.Get();
            Mark(L"resample");
        }
        Pass(psBeamH_.Get(), beamH_.rtv.Get(), w, sh, {beamSrc}, sw, sh);
        Mark(L"beamH");
    }
    if (firstStage <= 1 && lastStage >= 1)
    {
        Pass(psBeamV_.Get(), beam_.rtv.Get(), w, h, {beamH_.srv.Get()}, w, sh);
        Mark(L"beamV");

        Target* prev = &beam_;
        for (int i = 1; i <= kLevels; i++)
        {
            Pass(psDown_.Get(), down_[i].rtv.Get(), down_[i].w, down_[i].h, {prev->srv.Get()}, prev->w, prev->h);
            prev = &down_[i];
        }
        Mark(L"pyramid");
        Pass(psHalo_.Get(), halo_.rtv.Get(), halo_.w, halo_.h,
             {down_[kHaloLevel].srv.Get(), down_[kHaloLevel + 1].srv.Get(), down_[kHaloLevel + 2].srv.Get()},
             down_[kHaloLevel].w, down_[kHaloLevel].h);
        Mark(L"halo");
    }
    if (lastStage >= 2)
    {
        ID3D11RenderTargetView* finalOut = out;
        if (deblur && Ensure(composed_, w, h))
            finalOut = composed_.rtv.Get();
        else
            deblur = false;
        Pass(psFinal_.Get(), finalOut, w, h,
             {beam_.srv.Get(), down_[nearLevel].srv.Get(), down_[nearLevel + 1].srv.Get(), halo_.srv.Get(),
              down_[kLevels].srv.Get(), srcSrv_.Get(), down_[tailLevel].srv.Get(), down_[tailLevel + 1].srv.Get(),
              osdSrv_.Get(), down_[veilLevel].srv.Get(), down_[veilLevel + 1].srv.Get()},
             w, h);
        Mark(L"final");
        if (deblur)
        {
            Pass(psDeblur_.Get(), out, w, h,
                 {composed_.srv.Get(), motionFine_.srv.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  osdSrv_.Get()},
                 w, h);
            Mark(L"deblur");
        }
    }

    ID3D11ShaderResourceView* none[11] = {};
    ctx_->PSSetShaderResources(0, 11, none);

    if (profile_)
    {
        profile_ = false;
        ctx_->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (ctx_->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}
        std::wstring line = L"profile:";
        UINT64 prevT = 0;
        for (size_t i = 0; i < marks_.size(); i++)
        {
            UINT64 t = 0;
            while (ctx_->GetData(marks_[i].second.Get(), &t, sizeof(t), 0) == S_FALSE) {}
            if (i > 0)
            {
                wchar_t buf[64];
                swprintf_s(buf, L" %s %.3fms", marks_[i].first, double(t - prevT) * 1000.0 / dj.Frequency);
                line += buf;
            }
            prevT = t;
        }
        Log(L"%s%s", line.c_str(), dj.Disjoint ? L" (disjoint)" : L"");
    }
}

bool Renderer::BuildLuma(UINT w, UINT h)
{
    if (!psLuma_)
        return false;
    Target* nw = LumaOld(); // overwrite the older pyramid
    UINT lw = w, lh = h;
    for (int i = 0; i < 3; i++)
    {
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
        if (!Ensure(nw[i], lw, lh))
            return false;
    }
    Pass(psLuma_.Get(), nw[0].rtv.Get(), nw[0].w, nw[0].h, {srcSrv_.Get()}, w, h);
    Pass(psLumaDown_.Get(), nw[1].rtv.Get(), nw[1].w, nw[1].h, {nw[0].srv.Get()}, nw[0].w, nw[0].h);
    Pass(psLumaDown_.Get(), nw[2].rtv.Get(), nw[2].w, nw[2].h, {nw[1].srv.Get()}, nw[1].w, nw[1].h);
    lumaFlip_ = !lumaFlip_;
    Target* old = LumaOld();
    bool comparable = lumaValid_ && old[2].w == nw[2].w && old[2].h == nw[2].h;
    lumaValid_ = true;
    return comparable;
}

void Renderer::UpdateChangeMask(bool fresh, const FrameParams& fp)
{
    if (!psChangeMask_ || !LumaNew()[2].tex)
        return;
    Target& l3 = LumaNew()[2];
    int out = mask_ < 0 ? 0 : 1 - mask_;
    if (!Ensure(masks_[0], l3.w, l3.h) || !Ensure(masks_[1], l3.w, l3.h))
        return;
    if (mask_ < 0)
    {
        float zero[4] = {};
        ctx_->ClearRenderTargetView(masks_[1].rtv.Get(), zero);
        mask_ = 1;
        out = 0;
    }
    // Nothing new: compare the latest luma with itself, so the mask only fades.
    ID3D11ShaderResourceView* older = fresh ? LumaOld()[2].srv.Get() : l3.srv.Get();
    const float params[4] = {fp.changeThreshold, fp.maskDecay, 0, 0};
    Pass(psChangeMask_.Get(), masks_[out].rtv.Get(), l3.w, l3.h, {l3.srv.Get(), older, masks_[mask_].srv.Get()},
         l3.w, l3.h, params);
    mask_ = out;
}

void Renderer::EstimateMotion()
{
    if (!psMotionSearch_)
        return;
    Target* nw = LumaNew();
    Target* old = LumaOld();
    // Coarse: 8x8 blocks at 1/8 res (64 px), +-8 there (+-64 px per frame).
    // Mid: 8x8 at 1/4 (32 px), +-2 around it. Fine: 16x16 at 1/2, +-1, sub-pixel.
    UINT cw = (nw[2].w + 7) / 8, ch = (nw[2].h + 7) / 8;
    UINT mw = (nw[1].w + 7) / 8, mh = (nw[1].h + 7) / 8;
    if (!Ensure(motionCoarse_, cw, ch) || !Ensure(motionMid_, mw, mh) || !Ensure(motionFine_, mw, mh))
        return;
    // {footprint, radius, full px per level px, samples per side (+100 = sub-pixel)}
    const float coarse[4] = {8, 8, 8, 4}, mid[4] = {8, 2, 4, 4}, fine[4] = {16, 1, 2, 108};
    Pass(psMotionSearch_.Get(), motionCoarse_.rtv.Get(), cw, ch, {nw[2].srv.Get(), old[2].srv.Get(), nullptr},
         nw[2].w, nw[2].h, coarse);
    Pass(psMotionSearch_.Get(), motionMid_.rtv.Get(), mw, mh,
         {nw[1].srv.Get(), old[1].srv.Get(), motionCoarse_.srv.Get()}, nw[1].w, nw[1].h, mid);
    Pass(psMotionSearch_.Get(), motionFine_.rtv.Get(), mw, mh,
         {nw[0].srv.Get(), old[0].srv.Get(), motionMid_.srv.Get()}, nw[0].w, nw[0].h, fine);
    Mark(L"motion");
}

void Renderer::Probe(ID3D11Texture2D* src, UINT w, UINT h)
{
    if (!psProbe_ || probePending_)
        return;
    if (!probeTex_ || probeW_ != w)
    {
        probeTex_.Reset();
        probeStaging_.Reset();
        probeRtv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = 64;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &probeTex_)) ||
            FAILED(dev_->CreateRenderTargetView(probeTex_.Get(), nullptr, &probeRtv_)))
            return;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &probeStaging_)))
            return;
    }
    if (probeSrc_ != src)
    {
        probeSrcSrv_.Reset();
        probeSrc_ = src;
        dev_->CreateShaderResourceView(src, nullptr, &probeSrcSrv_);
    }
    probeW_ = w;
    probeH_ = h;
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {frameCB_.Get(), passCB_.Get()};
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    Pass(psProbe_.Get(), probeRtv_.Get(), w, 64, {probeSrcSrv_.Get()}, w, h);
    ctx_->CopyResource(probeStaging_.Get(), probeTex_.Get());
    probePending_ = true;
}

bool Renderer::ReadProbe(std::vector<float>& lines, UINT& w, UINT& h, bool wait)
{
    if (!probePending_)
        return false;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ctx_->Map(probeStaging_.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (FAILED(hr))
        return false; // still in flight
    w = probeW_;
    h = probeH_;
    lines.resize(size_t(w) * 64);
    for (UINT y = 0; y < 64; y++)
        memcpy(&lines[size_t(y) * w], (const uint8_t*)m.pData + size_t(y) * m.RowPitch, w * 4);
    ctx_->Unmap(probeStaging_.Get(), 0);
    probePending_ = false;
    return true;
}

bool Renderer::ReadMotion(std::vector<float>& rgba, UINT& w, UINT& h)
{
    if (!motionFine_.tex)
        return false;
    D3D11_TEXTURE2D_DESC td;
    motionFine_.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    // Convert through a float target so the CPU reads plain floats.
    ComPtr<ID3D11Texture2D> f32, staging;
    D3D11_TEXTURE2D_DESC fd = td;
    fd.Usage = D3D11_USAGE_DEFAULT;
    fd.BindFlags = D3D11_BIND_RENDER_TARGET;
    fd.CPUAccessFlags = 0;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateTexture2D(&fd, nullptr, &f32)) || FAILED(dev_->CreateRenderTargetView(f32.Get(), nullptr, &rtv)) ||
        FAILED(dev_->CreateTexture2D(&td, nullptr, &staging)))
        return false;
    Pass(psCopy_.Get(), rtv.Get(), td.Width, td.Height, {motionFine_.srv.Get()}, td.Width,
         td.Height);
    ctx_->CopyResource(staging.Get(), f32.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    w = td.Width;
    h = td.Height;
    rgba.resize(size_t(w) * h * 4);
    for (UINT y = 0; y < h; y++)
        memcpy(&rgba[size_t(y) * w * 4], (const uint8_t*)m.pData + size_t(y) * m.RowPitch, w * 16);
    ctx_->Unmap(staging.Get(), 0);
    return true;
}

void Renderer::RenderField(ID3D11Texture2D* src, UINT w, UINT h, const FrameParams& fp, int stage)
{
    for (Target& f : fields_)
        if (!Ensure(f, w, h))
            return;
    FrameParams linear = fp;
    linear.outputMode = 3; // PS_Final returns linear light; PS_Scan encodes
    Render(src, fields_[next_].rtv.Get(), w, h, linear, stage < 0 ? 0 : stage, stage < 0 ? 2 : stage);
}

void Renderer::LatchField(bool fresh)
{
    if (!fields_[next_].tex)
        return;
    bool first = cur_ < 0;
    if (first)
    {
        cur_ = 1;
        prev_ = 2;
    }
    if (fresh || first)
    {
        // Rotate: the field that just fell off the end becomes the next target,
        // so the three buffers stay distinct.
        int old = prev_;
        prev_ = cur_;
        cur_ = next_;
        next_ = old;
    }
    if (!fresh || first)
    {
        // Static content (or no history yet): the previous field's light equals
        // the current one.
        ctx_->CopyResource(fields_[prev_].tex.Get(), fields_[cur_].tex.Get());
    }
    if (!fresh)
        maskDecayPending_ = true; // nothing changed this field: let the mask fade
}

void Renderer::RenderScan(ID3D11RenderTargetView* out, UINT w, UINT h, const FrameParams& fp, int refreshInField)
{
    if (!psScan_ || cur_ < 0)
        return;
    FrameCB cb{};
    cb.inputMode = float(fp.inputMode);
    cb.sdrWhiteScale = fp.sdrWhiteScale;
    cb.outputMode = float(fp.outputMode);
    cb.outputBits = float(fp.outputBits);
    cb.peak = fp.peak;
    cb.split = fp.split;
    cb.motion0[0] = float(std::max(fp.motionK, 1));
    cb.motion0[1] = std::max(fp.motionPeak, 0.05f);
    cb.motion0[2] = std::max(fp.motionGain, 0.01f);
    cb.motion0[3] = std::max(fp.afterglow, 0.0f);
    cb.motion1[0] = float(refreshInField);
    cb.motion1[2] = float(fp.motionMode);
    cb.motion1[1] = std::clamp(fp.scanSoftness, 0.0f, 2.0f);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(frameCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(frameCB_.Get(), 0);
    }
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {frameCB_.Get(), passCB_.Get()};
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* samplers[2] = {linear_.Get(), linearBorder_.Get()};
    ctx_->PSSetSamplers(0, 2, samplers);
    if (fp.motionMode == 1 && maskDecayPending_)
    {
        UpdateChangeMask(false, fp);
        maskDecayPending_ = false;
    }
    ID3D11ShaderResourceView* mask = fp.motionMode == 1 && mask_ >= 0 ? masks_[mask_].srv.Get() : nullptr;
    Pass(psScan_.Get(), out, w, h,
         {fields_[cur_].srv.Get(), fields_[prev_].srv.Get(), mask, nullptr, nullptr, nullptr, nullptr, nullptr,
          osdSrv_.Get()},
         w, h);
    ID3D11ShaderResourceView* none[11] = {};
    ctx_->PSSetShaderResources(0, 11, none);
}
