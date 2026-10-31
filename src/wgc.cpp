#include "wgc.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

struct WindowCapture::Impl
{
    HWND hwnd = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived;
    wgc::GraphicsCaptureItem::Closed_revoker closed;
    wgc::Direct3D11CaptureFrame held{nullptr}; // keeps the last acquired surface alive
    std::atomic<bool> active = false;
    std::atomic<LONGLONG> arrivedQpc = 0;
};

WindowCapture::WindowCapture() : p_(std::make_unique<Impl>()) {}

WindowCapture::~WindowCapture()
{
    Stop();
    CloseHandle(p_->event);
}

bool WindowCapture::Start(ID3D11Device* dev, HWND hwnd, bool hdr)
{
    Stop();
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...)
    {
        // already initialised on this thread
    }
    try
    {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                       winrt::put_abi(item)));

        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(dev->QueryInterface(IID_PPV_ARGS(&dxgi)));
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()));
        auto d3d = insp.as<wdx::Direct3D11::IDirect3DDevice>();

        auto fmt = hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        // Free-threaded: frames arrive on a system thread; we only signal.
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d, fmt, 3, item.Size());
        auto session = pool.CreateCaptureSession(item);
        try
        {
            session.IsCursorCaptureEnabled(false);
        }
        catch (...)
        {
        }
        try
        {
            session.IsBorderRequired(false); // Windows 11: no yellow frame
        }
        catch (...)
        {
        }
        try
        {
            // Deliver every frame the game presents, not just one per refresh.
            session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{0});
        }
        catch (...)
        {
        }

        Impl* p = p_.get();
        p->arrived = pool.FrameArrived(winrt::auto_revoke, [p](auto&&, auto&&) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            p->arrivedQpc = t.QuadPart;
            SetEvent(p->event);
        });
        p->closed = item.Closed(winrt::auto_revoke, [p](auto&&, auto&&) {
            p->active = false;
            SetEvent(p->event);
        });
        p->hwnd = hwnd;
        p->item = item;
        p->pool = pool;
        p->session = session;
        p->active = true;
        session.StartCapture();
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        Log(L"window capture failed: 0x%08x %s", uint32_t(e.code()), e.message().c_str());
    }
    catch (...)
    {
        Log(L"window capture failed");
    }
    Stop();
    return false;
}

void WindowCapture::Stop()
{
    Impl* p = p_.get();
    p->active = false;
    p->arrived.revoke();
    p->closed.revoke();
    p->held = nullptr;
    try
    {
        if (p->session)
            p->session.Close();
        if (p->pool)
            p->pool.Close();
    }
    catch (...)
    {
    }
    p->session = nullptr;
    p->pool = nullptr;
    p->item = nullptr;
    p->hwnd = nullptr;
}

bool WindowCapture::Active() const { return p_->active; }
HWND WindowCapture::Target() const { return p_->hwnd; }
HANDLE WindowCapture::Event() const { return p_->event; }

bool WindowCapture::Acquire(ComPtr<ID3D11Texture2D>& tex, LONGLONG& qpc, int& skipped)
{
    Impl* p = p_.get();
    skipped = 0;
    if (!p->active || !p->pool)
        return false;
    try
    {
        // Drain to the newest frame.
        wgc::Direct3D11CaptureFrame newest{nullptr};
        for (;;)
        {
            auto f = p->pool.TryGetNextFrame();
            if (!f)
                break;
            if (newest)
            {
                skipped++;
                newest.Close();
            }
            newest = f;
        }
        if (!newest)
            return false;
        auto access = newest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> t;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&t)));
        if (p->held)
            p->held.Close();
        p->held = newest;
        tex = t;
        qpc = p->arrivedQpc;
        return true;
    }
    catch (...)
    {
        p->active = false;
        return false;
    }
}
