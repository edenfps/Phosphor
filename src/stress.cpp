// Test harness: a fake GPU-heavy borderless fullscreen game, so overlay pacing
// under load can be measured without a real game. Draws moving bars (to judge
// smoothness by eye) and burns a tunable amount of GPU per pixel, presenting
// as fast as it can like an uncapped game.

#include "common.h"
#include "display.h"
#include <d3dcompiler.h>

namespace
{
const char* kStressHlsl = R"(
cbuffer C : register(b0) { float t; float iters; float2 pad; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target
{
    float v = 0;
    float2 p = pos.xy * 0.001;
    [loop] for (int i = 0; i < (int)iters; i++)
        v = frac(sin(dot(p + v, float2(12.9898, 78.233))) * 43758.5453);
    float bar = frac((pos.x + t * 1500.0) / 480.0) < 0.08 ? 0.85 : 0.08;
    return float4(bar.xxx + v * 0.02, 1);
}
)";

LRESULT CALLBACK StressProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_ESCAPE)
        PostQuitMessage(0);
    if (m == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}
} // namespace

int RunStress(const std::wstring& monitor, int iters, int seconds, int capFps)
{
    DisplayTarget d;
    if (!FindDisplay(monitor, d))
        return 1;
    char cls[8] = {};
    if (GetEnvironmentVariableA("STRESS_CLASS", cls, sizeof(cls)))
    {
        // Test: run the fake game at another GPU scheduling class.
        using SetClass = LONG(APIENTRY*)(HANDLE, int);
        auto fn = (SetClass)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass");
        if (fn)
            Log(L"stress: scheduling class %d -> %ld", atoi(cls), fn(GetCurrentProcess(), atoi(cls)));
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(d.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &dev, nullptr, &ctx)))
        return 1;

    WNDCLASSW wc{};
    wc.lpfnWndProc = StressProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhosphorStress";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const RECT& r = d.rect;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Phosphor stress", WS_POPUP | WS_VISIBLE, r.left, r.top,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(hwnd);
    UINT w = r.right - r.left, h = r.bottom - r.top;

    ComPtr<IDXGIFactory2> factory;
    d.adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 3;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &scd, nullptr, nullptr, &sc)))
        return 1;
    ComPtr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kStressHlsl, strlen(kStressHlsl), "stress", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kStressHlsl, strlen(kStressHlsl), "stress", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err)))
        return 1;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    ComPtr<ID3D11Buffer> cb;
    dev->CreateBuffer(&bd, nullptr, &cb);

    LARGE_INTEGER f, t0, tl;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    tl = t0;
    int frames = 0;
    Log(L"stress: %ux%u, %d iterations per pixel, %d s", w, h, iters, seconds);
    for (;;)
    {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                quit = true;
            DispatchMessageW(&msg);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = double(now.QuadPart - t0.QuadPart) / f.QuadPart;
        if (quit || t > seconds)
            break;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float c[4] = {float(t), float(iters), 0, 0};
            memcpy(m.pData, c, sizeof(c));
            ctx->Unmap(cb.Get(), 0);
        }
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->Draw(3, 0);
        sc->Present(0, 0);
        frames++;
        if (capFps > 0)
        {
            // Frame limiter like an in-game cap: wait out the rest of the frame.
            static LONGLONG next = 0;
            LONGLONG period = f.QuadPart / capFps;
            if (!next)
                next = now.QuadPart;
            next += period;
            LARGE_INTEGER c;
            for (QueryPerformanceCounter(&c); c.QuadPart < next; QueryPerformanceCounter(&c))
                if (next - c.QuadPart > f.QuadPart / 500)
                    Sleep(1);
            if (c.QuadPart - next > period)
                next = c.QuadPart;
        }
        if (double(now.QuadPart - tl.QuadPart) / f.QuadPart >= 1.0)
        {
            Log(L"stress: %d fps", frames);
            frames = 0;
            tl = now;
        }
    }
    DestroyWindow(hwnd);
    return 0;
}
