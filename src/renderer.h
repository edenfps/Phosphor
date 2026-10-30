#pragma once
#include "common.h"
#include "config.h"
#include <initializer_list>

struct FrameParams
{
    Look look;
    int inputMode = 0;          // 0 sRGB 8-bit, 1 scRGB fp16
    float sdrWhiteScale = 1.0f; // scRGB units per SDR white
    int outputMode = 0;         // 0 sRGB, 1 gamma 2.2, 2 scRGB
    int outputBits = 8;         // SDR output precision, for dithering
    float peak = 1.0f;          // in SDR-white units
    float pixelMm = 0.18f;      // physical size of one output pixel
    float split = 0.0f;
    float subX[3] = {}, subY[3] = {}; // panel emitter offsets per colour (px)
    // Rolling scan
    int motionK = 4;            // panel refreshes per CRT field
    float motionPeak = 1.0f;    // max light per refresh (SDR-white units)
    float motionGain = 1.0f;
    float afterglow = 0.0f;     // phosphor time constant, in refreshes
    float scanSoftness = 0.0f;  // slice-edge blend width, in refreshes
    int motionMode = 0;         // 0 rolling scan, 1 motion-adaptive flash
    float changeThreshold = 0.02f; // luma change that counts as motion
    float maskDecay = 0.5f;     // per field, so regions fade out of strobing
    // Motion pre-deblur
    bool newFrame = false;      // src is a newly captured frame: measure motion against the last one
    float deblur = 0.0f;        // strength, 0 = off
    float deblurMaxPx = 48.0f;  // largest smear compensated
    bool motionDebug = false;   // tint by motion vectors
};

class Renderer
{
public:
    bool Init(ID3D11Device* dev, const std::wstring& shaderPath);
    bool Reload();
    // src: desktop texture (BGRA8 sRGB or FP16 scRGB) with SHADER_RESOURCE binding.
    // Stages 0..2 can be run separately (same inputs) to spread the cost.
    void Render(ID3D11Texture2D* src, ID3D11RenderTargetView* out, UINT w, UINT h, const FrameParams& fp,
                int firstStage = 0, int lastStage = 2);
    // Rolling scan: RenderField runs the full CRT model into a linear field
    // buffer, LatchField makes it current when the beam returns to the top
    // (fresh = new content; otherwise the current field repeats), RenderScan
    // draws one panel refresh of the scan.
    // stage -1 = whole field at once, else one of the three stages.
    void RenderField(ID3D11Texture2D* src, UINT w, UINT h, const FrameParams& fp, int stage = -1);
    static const int kFieldStages = 3;
    void LatchField(bool fresh);
    void RenderScan(ID3D11RenderTargetView* out, UINT w, UINT h, const FrameParams& fp, int refreshInField);
    bool HasField() const { return cur_ >= 0; }
    // Times every pass of the next Render() with GPU timestamps and logs them.
    void ProfileNext() { profile_ = true; }
    // On-screen text, drawn on top of the CRT image. rg: w*h pairs of
    // (text coverage, panel opacity), output-sized.
    void SetOsd(const uint8_t* rg, UINT w, UINT h);
    void ClearOsd() { osdSrv_.Reset(); }
    // Resolution probe: Probe() samples 32 rows + 32 columns of src luma on
    // the GPU; ReadProbe() returns the previous probe without waiting (false
    // if the GPU isn't done yet). lines = 64 x w floats, columns use the
    // first h entries of their line.
    void Probe(ID3D11Texture2D* src, UINT w, UINT h);
    bool ReadProbe(std::vector<float>& lines, UINT& w, UINT& h, bool wait = false);
    // Debug: the last measured motion field (x, y, confidence, match error).
    bool ReadMotion(std::vector<float>& rgba, UINT& w, UINT& h);

private:
    struct Target
    {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT w = 0, h = 0;
    };
    struct PassCB
    {
        float srcW, srcH, srcTexelW, srcTexelH;
        float dstW, dstH, dstTexelW, dstTexelH;
        float params[4];
    };

    bool Ensure(Target& t, UINT w, UINT h);
    void Pass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT dw, UINT dh,
              std::initializer_list<ID3D11ShaderResourceView*> srvs, UINT texelW, UINT texelH,
              const float* params = nullptr);
    // Luma pyramid of the new frame (the previous one is kept for comparison).
    bool BuildLuma(UINT w, UINT h);
    Target* LumaNew() { return lumaFlip_ ? lumaB_ : lumaA_; }
    Target* LumaOld() { return lumaFlip_ ? lumaA_ : lumaB_; }
    // Coarse-to-fine block matching of the two pyramids into motionFine_.
    void EstimateMotion();
    // Change mask for the motion-adaptive flash: fresh = compare the two
    // pyramids, otherwise just fade the mask (nothing new arrived).
    void UpdateChangeMask(bool fresh, const FrameParams& fp);

    // Glow pyramid: level k is ~gaussian sigma 2^k px. 4..6 feed halation,
    // 8 is the local average for highlight punch.
    static const int kLevels = 8;
    static const int kHaloLevel = 4;

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    std::wstring shaderPath_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psDecode_, psResample_, psBeamH_, psBeamV_, psDown_, psHalo_, psFinal_;
    ComPtr<ID3D11Buffer> frameCB_, passCB_;
    ComPtr<ID3D11SamplerState> linear_, linearBorder_;
    ComPtr<ID3D11Texture2D> srcTex_;
    ComPtr<ID3D11ShaderResourceView> srcSrv_;
    ComPtr<ID3D11ShaderResourceView> osdSrv_;

    bool profile_ = false;
    std::vector<std::pair<const wchar_t*, ComPtr<ID3D11Query>>> marks_;
    void Mark(const wchar_t* name);

    ComPtr<ID3D11PixelShader> psScan_;
    Target fields_[3];
    int next_ = 0, cur_ = -1, prev_ = -1; // indices into fields_, always distinct once latched

    ComPtr<ID3D11PixelShader> psLuma_, psLumaDown_, psMotionSearch_, psDeblur_, psCopy_;
    Target lumaA_[3], lumaB_[3]; // 1/2, 1/4, 1/8 resolution; alternate between frames
    bool lumaFlip_ = false, lumaValid_ = false;
    Target motionCoarse_, motionMid_, motionFine_, composed_;
    ComPtr<ID3D11PixelShader> psChangeMask_, psProbe_;
    ComPtr<ID3D11Texture2D> probeTex_, probeStaging_;
    ComPtr<ID3D11RenderTargetView> probeRtv_;
    ComPtr<ID3D11ShaderResourceView> probeSrcSrv_;
    ID3D11Texture2D* probeSrc_ = nullptr;
    UINT probeW_ = 0, probeH_ = 0;
    bool probePending_ = false;
    Target masks_[2];
    int mask_ = -1; // latest mask index, -1 = none yet
    bool maskDecayPending_ = false;
    FrameParams lastScanParams_;

    Target lin_, source_, beamH_, beam_, halo_;
    Target down_[kLevels + 1];
};
