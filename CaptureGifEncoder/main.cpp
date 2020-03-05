﻿#include "pch.h"
#include "WindowInfo.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Storage;
    using namespace Windows::System;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace util
{
    using namespace robmikh::common::desktop;
    using namespace robmikh::common::uwp;
}

winrt::IAsyncAction MainAsync(std::vector<std::wstring> const& args)
{
    if (args.size() <= 0)
    {
        wprintf(L"Invalid input!\n");
        co_return;
    }
    auto windowQuery = args[0];

    auto matchedWindows = FindWindowsByTitle(windowQuery);
    if (matchedWindows.size() <= 0)
    {
        wprintf(L"Couldn't find a window that contains '%s'!\n", windowQuery.c_str());
        co_return;
    }
    auto window = matchedWindows[0];
    wprintf(L"Using '%s'\n", window.Title.c_str());

    auto d3dDevice = util::CreateD3DDevice();
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    d3dDevice->GetImmediateContext(d3dContext.put());
    auto device = CreateDirect3DDevice(d3dDevice.as<IDXGIDevice>().get());
    auto d2dFactory = util::CreateD2DFactory();
    auto d2dDevice = util::CreateD2DDevice(d2dFactory, d3dDevice);
    winrt::com_ptr<ID2D1DeviceContext> d2dContext;
    winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext.put()));
    auto wicFactory = util::CreateWICFactory();

    auto currentPath = std::filesystem::current_path();
    auto folder = co_await winrt::StorageFolder::GetFolderFromPathAsync(currentPath.wstring());
    auto file = co_await folder.CreateFileAsync(L"test.gif", winrt::CreationCollisionOption::ReplaceExisting);

    auto stream = co_await file.OpenAsync(winrt::FileAccessMode::ReadWrite);
    auto abiStream = util::CreateStreamFromRandomAccessStream(stream);

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatGif, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(abiStream.get(), WICBitmapEncoderNoCache));

    winrt::com_ptr<IWICImageEncoder> imageEncoder;
    winrt::check_hresult(wicFactory->CreateImageEncoder(d2dDevice.get(), imageEncoder.put()));

    auto item = util::CreateCaptureItemForWindow(window.WindowHandle);
    auto itemSize = item.Size();
    auto framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        1,
        itemSize);
    auto session = framePool.CreateCaptureSession(item);

    winrt::com_ptr<ID3D11Texture2D> blankTexture;
    winrt::com_ptr<ID3D11Texture2D> gifTexture;
    {
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = itemSize.Width;
        description.Height = itemSize.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.CPUAccessFlags = 0;
        winrt::check_hresult(d3dDevice->CreateTexture2D(&description, nullptr, blankTexture.put()));
        winrt::com_ptr<ID3D11RenderTargetView> renderTargetView;
        winrt::check_hresult(d3dDevice->CreateRenderTargetView(blankTexture.get(), nullptr, renderTargetView.put()));
        d3dContext->ClearRenderTargetView(renderTargetView.get(), new float[0.0f, 0.0f, 0.0f, 1.0f]);

        winrt::check_hresult(d3dDevice->CreateTexture2D(&description, nullptr, gifTexture.put()));
    }

    auto lastTimeStamp = winrt::TimeSpan{ 0 };
    framePool.FrameArrived([=, &lastTimeStamp](auto& framePool, auto&)
    {
        auto frame = framePool.TryGetNextFrame();
        auto contentSize = frame.ContentSize();
        auto timeStamp = frame.SystemRelativeTime();
        auto frameTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

        if (lastTimeStamp.count() == 0)
        {
            lastTimeStamp = timeStamp;
        }
        auto timeStampDelta = timeStamp - lastTimeStamp;
        lastTimeStamp = timeStamp;
        auto millisconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeStampDelta);
        // Use 10ms units
        auto frameDelay = millisconds.count() / 10;

        auto width = std::clamp(contentSize.Width, 0, itemSize.Width);
        auto height = std::clamp(contentSize.Height, 0, itemSize.Height);

        D3D11_BOX region = {};
        region.left = 0;
        region.right = width;
        region.top = 0;
        region.bottom = height;
        region.back = 1;

        d3dContext->CopyResource(gifTexture.get(), blankTexture.get());
        d3dContext->CopySubresourceRegion(
            gifTexture.get(),
            0,
            0, 0, 0,
            frameTexture.get(),
            0,
            &region);

        winrt::com_ptr<ID2D1Bitmap1> d2dBitmap;
        winrt::check_hresult(d2dContext->CreateBitmapFromDxgiSurface(gifTexture.as<IDXGISurface>().get(), nullptr, d2dBitmap.put()));

        winrt::com_ptr<IWICBitmapFrameEncode> wicFrame;
        winrt::check_hresult(encoder->CreateNewFrame(wicFrame.put(), nullptr));
        winrt::check_hresult(wicFrame->Initialize(nullptr));
        auto wicPixelFormat = GUID_WICPixelFormat32bppBGRA;
        winrt::check_hresult(wicFrame->SetPixelFormat(&wicPixelFormat));

        winrt::com_ptr<IWICMetadataQueryWriter> metadata;
        winrt::check_hresult(wicFrame->GetMetadataQueryWriter(metadata.put()));
        wil::unique_prop_variant delayValue;
        delayValue.vt = VT_UI2;
        delayValue.uiVal = frameDelay;
        winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Delay", &delayValue));

        winrt::check_hresult(imageEncoder->WriteFrame(d2dBitmap.get(), wicFrame.get(), nullptr));
        winrt::check_hresult(wicFrame->Commit());
    });

    session.StartCapture();
    co_await std::chrono::seconds(5);
    session.Close();
    framePool.Close();

    winrt::check_hresult(encoder->Commit());
    co_await winrt::Launcher::LaunchFileAsync(file);
}

int wmain(int argc, wchar_t* argv[])
{
    winrt::init_apartment();
    
    std::vector<std::wstring> args(argv + 1, argv + argc);

    MainAsync(args).get();
}
