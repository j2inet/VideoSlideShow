// VideoSlideShow.cpp
// A Direct3D 11 slideshow application.
//   - Reads a playlist of image and video filenames from "playlist.txt"
//   - Renders two full-screen planes; the second starts off-screen to the right
//   - On left-click: pauses the current video, loads the next item onto the
//     off-screen plane, and slides the two planes into/out of view
//   - Loops back to the first item after the last one
//
// Technologies used:
//   Direct3D 11       – rendering
//   WIC               – image loading (JPEG, PNG, BMP, …)
//   Windows Media Foundation – video frame decoding

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfobjects.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <cwctype>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

#ifndef MFVideoFormat_BGRA32
const GUID MFVideoFormat_BGRA32 = { 0x00000016, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#endif

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
static const wchar_t* APP_TITLE          = L"VideoSlideShow";
static const wchar_t* PLAYLIST_FILE      = L"playlist.txt";
static const int      WINDOW_WIDTH       = 2160;
static const int      WINDOW_HEIGHT      = 3840;
static const float    TRANSITION_SECONDS = 0.6f;

// ---------------------------------------------------------------------------
//  Embedded HLSL shaders
// ---------------------------------------------------------------------------
static const char* VS_SRC = R"hlsl(
cbuffer TransformCB : register(b0)
{
    row_major float4x4 g_Transform;
};
cbuffer UVTransformCB : register(b1)
{
    row_major float4x4 g_UVTransform;
};
struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT main(VS_IN v)
{
    VS_OUT o;
    o.pos = mul(float4(v.pos, 0.0f, 1.0f), g_Transform);
    float4 uv4 = mul(float4(v.uv, 0.0f, 1.0f), g_UVTransform);
    o.uv = uv4.xy;
    return o;
}
)hlsl";

static const char* PS_SRC = R"hlsl(
Texture2D    g_Tex  : register(t0);
SamplerState g_Samp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return g_Tex.Sample(g_Samp, uv);
}
)hlsl";

// ---------------------------------------------------------------------------
//  Geometry – full-screen quad
// ---------------------------------------------------------------------------
struct Vertex { float x, y, u, v; };

static const Vertex QUAD_VERTS[] =
{
    { -1.0f,  1.0f,  0.0f, 0.0f },
    {  1.0f,  1.0f,  1.0f, 0.0f },
    { -1.0f, -1.0f,  0.0f, 1.0f },
    {  1.0f, -1.0f,  1.0f, 1.0f },
};
static const UINT QUAD_IDX[] = { 0, 1, 2, 1, 3, 2 };

// ---------------------------------------------------------------------------
//  Media type detection
// ---------------------------------------------------------------------------
enum class MediaType { Image, Video };

static MediaType DetectType(const std::wstring& path)
{
    auto dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return MediaType::Image;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    if (ext == L"mp4" || ext == L"wmv" || ext == L"avi" || ext == L"mov" ||
        ext == L"mkv" || ext == L"m4v" || ext == L"mpg" || ext == L"mpeg" ||
        ext == L"webm")
    {
        return MediaType::Video;
    }
    return MediaType::Image;
}

struct MediaItem { std::wstring path; MediaType type; };

// ---------------------------------------------------------------------------
//  VideoState – background thread that decodes frames via Media Foundation
// ---------------------------------------------------------------------------
struct VideoState
{
    // Set before calling Start():
    std::wstring filename;

    // Written by decode thread, read by render thread:
    std::mutex           frameMutex;
    std::vector<BYTE>    framePixels;
    UINT32               frameW  = 0;
    UINT32               frameH  = 0;
    std::atomic<bool>    newFrame{ false };
    std::atomic<int>     rotationDeg{ 0 };  // 0, 90, 180, or 270

    // Control flags:
    std::atomic<bool>    paused       { false };
    std::atomic<bool>    stopRequested{ false };

    std::thread thread;

    void Start();
    void Stop();

private:
    void ThreadProc();
};

void VideoState::Start()
{
    stopRequested = false;
    paused        = false;
    newFrame      = false;
    thread        = std::thread(&VideoState::ThreadProc, this);
}

void VideoState::Stop()
{
    stopRequested = true;
    if (thread.joinable())
        thread.join();
}

void VideoState::ThreadProc()
{
    // Each decode thread owns its own COM apartment.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Open source reader with video-processing enabled (handles format conversion).
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(filename.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) { CoUninitialize(); return; }

    // Request BGRA output so pixels can be uploaded directly to a BGRA texture.
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_BGRA32);
    hr = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outType.Get());
    if (FAILED(hr)) { CoUninitialize(); return; }

    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,         FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,  TRUE);

    // Retrieve the actual output frame size after conversion is configured.
    ComPtr<IMFMediaType> curType;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curType);
    UINT32 videoW = 0, videoH = 0;
    MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &videoW, &videoH);

    // Read rotation metadata; default to 0 if not present.
    {
        UINT32 rot = 0;
        curType->GetUINT32(MF_MT_VIDEO_ROTATION, &rot);
        if (rot == 90 || rot == 180 || rot == 270)
            rotationDeg = static_cast<int>(rot);
        else
            rotationDeg = 0;  // treat 0 and any unexpected value as no rotation
    }

    // Retrieve frame rate to pace playback accurately.
    UINT32 rateNum = 30, rateDen = 1;
    MFGetAttributeRatio(curType.Get(), MF_MT_FRAME_RATE, &rateNum, &rateDen);
    if (rateNum == 0) rateNum = 30;
    DWORD frameSleepMs = static_cast<DWORD>(
        (static_cast<double>(rateDen) / rateNum) * 1000.0 + 0.5);
    if (frameSleepMs < 1)  frameSleepMs = 1;
    if (frameSleepMs > 100) frameSleepMs = 100;

    while (!stopRequested)
    {
        if (paused) { Sleep(16); continue; }

        DWORD  streamIndex = 0, streamFlags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        hr = reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0, &streamIndex, &streamFlags, &timestamp, &sample);

        if (FAILED(hr) || !sample)
        {
            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                // Loop: seek back to the beginning.
                PROPVARIANT varStart;
                PropVariantInit(&varStart);
                varStart.vt           = VT_I8;
                varStart.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, varStart);
                PropVariantClear(&varStart);
            }
            Sleep(1);
            continue;
        }

        // Flatten multi-buffer samples into one contiguous buffer.
        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);

        BYTE* pData   = nullptr;
        DWORD cbData  = 0;
        hr = buf->Lock(&pData, nullptr, &cbData);
        if (SUCCEEDED(hr))
        {
            std::lock_guard<std::mutex> lk(frameMutex);
            frameW = videoW;
            frameH = videoH;
            framePixels.assign(pData, pData + cbData);
            newFrame = true;
            buf->Unlock();
        }

        Sleep(frameSleepMs);
    }

    reader.Reset();
    CoUninitialize();
}

// ---------------------------------------------------------------------------
//  Application
// ---------------------------------------------------------------------------
class Application
{
public:
    Application()  = default;
    ~Application() { Cleanup(); }

    bool Initialize(HWND hwnd, int w, int h);
    void OnClick();
    void Update();
    void Render();
    void Cleanup();

private:
    // --- D3D11 objects -------------------------------------------------------
    ComPtr<ID3D11Device>            m_dev;
    ComPtr<ID3D11DeviceContext>     m_ctx;
    ComPtr<IDXGISwapChain>          m_sc;
    ComPtr<ID3D11RenderTargetView>  m_rtv;
    ComPtr<ID3D11VertexShader>      m_vs;
    ComPtr<ID3D11PixelShader>       m_ps;
    ComPtr<ID3D11InputLayout>       m_il;
    ComPtr<ID3D11Buffer>            m_vb;
    ComPtr<ID3D11Buffer>            m_ib;
    ComPtr<ID3D11Buffer>            m_cb[2];   // per-plane constant buffer (b0: position)
    ComPtr<ID3D11Buffer>            m_uvcb[2]; // per-plane UV transform buffer (b1: UV rotation)
    ComPtr<ID3D11SamplerState>      m_samp;
    ComPtr<ID3D11RasterizerState>   m_rast;
    ComPtr<ID3D11BlendState>        m_blend;

    // --- WIC factory ---------------------------------------------------------
    ComPtr<IWICImagingFactory>      m_wic;

    // --- Per-plane state -----------------------------------------------------
    struct Plane
    {
        float                            offsetX    = 0.0f;
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT                             texW       = 0;
        UINT                             texH       = 0;
        bool                             hasContent = false;
        std::unique_ptr<VideoState>      video;
    };
    Plane m_plane[2];

    // --- Playlist & navigation -----------------------------------------------
    std::vector<MediaItem> m_playlist;
    int m_currentIdx  = 0;   // media index shown on the active (visible) plane
    int m_activePlane = 0;   // which plane is currently at offsetX = 0

    // --- Transition animation ------------------------------------------------
    bool  m_transitioning  = false;
    float m_transProgress  = 0.0f;
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_transStart;

    int m_width  = 0;
    int m_height = 0;

    // --- Helpers -------------------------------------------------------------
    bool CreateDeviceAndSwapChain(HWND hwnd);
    bool CreateShadersAndLayout();
    bool CreateGeometryBuffers();
    bool CreateConstantBuffers();
    bool CreateRenderStates();

    bool LoadMedia(int planeIdx, int mediaIdx);
    bool LoadImage(int planeIdx, const std::wstring& path);
    bool LoadVideo(int planeIdx, const std::wstring& path);

    void EnsureTexture(int planeIdx, UINT w, UINT h);
    void UploadPixels(int planeIdx, const BYTE* pixels, UINT w, UINT h);
    void UpdateCB(int planeIdx);
    void UpdateUVCB(int planeIdx);

    static float EaseInOut(float t) { return t * t * (3.0f - 2.0f * t); }
};

// ---------------------------------------------------------------------------
bool Application::Initialize(HWND hwnd, int w, int h)
{
    m_width  = w;
    m_height = h;

    if (FAILED(MFStartup(MF_VERSION))) return false;

    // WIC factory (STA-compatible; used only on main thread).
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wic))))
        return false;

    if (!CreateDeviceAndSwapChain(hwnd)) return false;
    if (!CreateShadersAndLayout())       return false;
    if (!CreateGeometryBuffers())        return false;
    if (!CreateConstantBuffers())        return false;
    if (!CreateRenderStates())           return false;

    // Parse playlist.txt (one file path per line, UTF-16 or UTF-8 encoded).
    {
        std::wifstream f(PLAYLIST_FILE);
        if (!f.is_open())
        {
            MessageBoxW(hwnd, L"Could not open playlist.txt", APP_TITLE, MB_ICONERROR);
            return false;
        }
        std::wstring line;
        while (std::getline(f, line))
        {
            while (!line.empty() && std::iswspace(line.back()))  line.pop_back();
            while (!line.empty() && std::iswspace(line.front())) line.erase(line.begin());
            if (!line.empty() && line.front() != L'#')
            {
                MediaItem item{ line, DetectType(line) };
                m_playlist.push_back(std::move(item));
            }
        }
    }

    if (m_playlist.empty())
    {
        MessageBoxW(hwnd, L"playlist.txt contains no entries.", APP_TITLE, MB_ICONERROR);
        return false;
    }

    // Initial layout: plane 0 visible, plane 1 off-screen right.
    m_activePlane      = 0;
    m_plane[0].offsetX = 0.0f;
    m_plane[1].offsetX = 2.0f;

    m_currentIdx = 0;
    LoadMedia(0, 0);

    UpdateCB(0);
    UpdateCB(1);
    UpdateUVCB(0);
    UpdateUVCB(1);
    return true;
}

// ---------------------------------------------------------------------------
bool Application::CreateDeviceAndSwapChain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount             = 1;
    scd.BufferDesc.Width        = static_cast<UINT>(m_width);
    scd.BufferDesc.Height       = static_cast<UINT>(m_height);
    scd.BufferDesc.Format       = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate  = { 60, 1 };
    scd.BufferUsage             = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow            = hwnd;
    scd.SampleDesc.Count        = 1;
    scd.Windowed                = TRUE;
    scd.SwapEffect              = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &scd,
        &m_sc, &m_dev, &fl, &m_ctx);
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Texture2D> bb;
    hr = m_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) return false;

    hr = m_dev->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
bool Application::CreateShadersAndLayout()
{
    // Vertex shader
    ComPtr<ID3DBlob> vsBlob, errBlob;
    HRESULT hr = D3DCompile(VS_SRC, strlen(VS_SRC), nullptr, nullptr, nullptr,
                            "main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
        return false;
    }
    hr = m_dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                                   vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) return false;

    // Input layout
    const D3D11_INPUT_ELEMENT_DESC ied[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = m_dev->CreateInputLayout(ied, 2,
                                  vsBlob->GetBufferPointer(),
                                  vsBlob->GetBufferSize(), &m_il);
    if (FAILED(hr)) return false;

    // Pixel shader
    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(PS_SRC, strlen(PS_SRC), nullptr, nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
        return false;
    }
    hr = m_dev->CreatePixelShader(psBlob->GetBufferPointer(),
                                  psBlob->GetBufferSize(), nullptr, &m_ps);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
bool Application::CreateGeometryBuffers()
{
    // Vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth  = sizeof(QUAD_VERTS);
    vbd.Usage      = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags  = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = { QUAD_VERTS };
    if (FAILED(m_dev->CreateBuffer(&vbd, &vsd, &m_vb))) return false;

    // Index buffer
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth  = sizeof(QUAD_IDX);
    ibd.Usage      = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags  = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = { QUAD_IDX };
    return SUCCEEDED(m_dev->CreateBuffer(&ibd, &isd, &m_ib));
}

// ---------------------------------------------------------------------------
bool Application::CreateConstantBuffers()
{
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(XMFLOAT4X4);   // 64 bytes – multiple of 16
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    for (int i = 0; i < 2; ++i)
        if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_cb[i]))) return false;

    for (int i = 0; i < 2; ++i)
        if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_uvcb[i]))) return false;
    return true;
}

// ---------------------------------------------------------------------------
bool Application::CreateRenderStates()
{
    // Linear-clamp sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(m_dev->CreateSamplerState(&sd, &m_samp))) return false;

    // No back-face culling (quad is always front-facing but be safe)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(m_dev->CreateRasterizerState(&rd, &m_rast))) return false;

    // Opaque blend
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(m_dev->CreateBlendState(&bd, &m_blend));
}

// ---------------------------------------------------------------------------
void Application::EnsureTexture(int idx, UINT w, UINT h)
{
    auto& p = m_plane[idx];
    if (p.texW == w && p.texH == h && p.tex) return;

    p.tex.Reset();
    p.srv.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &p.tex))) return;
    if (FAILED(m_dev->CreateShaderResourceView(p.tex.Get(), nullptr, &p.srv)))
    {
        p.tex.Reset();
        return;
    }
    p.texW = w;
    p.texH = h;
}

// ---------------------------------------------------------------------------
void Application::UploadPixels(int idx, const BYTE* pixels, UINT w, UINT h)
{
    EnsureTexture(idx, w, h);
    auto& p = m_plane[idx];
    if (!p.tex) return;

    m_ctx->UpdateSubresource(p.tex.Get(), 0, nullptr, pixels, w * 4, 0);
    p.hasContent = true;
}

// ---------------------------------------------------------------------------
bool Application::LoadImage(int idx, const std::wstring& path)
{
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   conv;

    HRESULT hr = m_wic->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return false;

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    hr = m_wic->CreateFormatConverter(&conv);
    if (FAILED(hr)) return false;

    // Convert any source format to 32-bit BGRA.
    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);

    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4,
                          static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return false;

    UploadPixels(idx, pixels.data(), w, h);
    return true;
}

// ---------------------------------------------------------------------------
bool Application::LoadVideo(int idx, const std::wstring& path)
{
    auto& p = m_plane[idx];

    // Stop and release any previous video on this plane.
    if (p.video) { p.video->Stop(); p.video.reset(); }

    auto vs = std::make_unique<VideoState>();
    vs->filename = path;
    vs->Start();
    p.video = std::move(vs);
    // The texture is created lazily when the first frame arrives in Update().
    return true;
}

// ---------------------------------------------------------------------------
bool Application::LoadMedia(int planeIdx, int mediaIdx)
{
    if (m_playlist.empty()) return false;

    const auto& item = m_playlist[mediaIdx % static_cast<int>(m_playlist.size())];

    auto& p = m_plane[planeIdx];
    if (p.video) { p.video->Stop(); p.video.reset(); }
    p.hasContent = false;

    if (item.type == MediaType::Image)
        return LoadImage(planeIdx, item.path);
    else
        return LoadVideo(planeIdx, item.path);
}

// ---------------------------------------------------------------------------
void Application::UpdateCB(int idx)
{
    XMMATRIX   t   = XMMatrixTranslation(m_plane[idx].offsetX, 0.0f, 0.0f);
    XMFLOAT4X4 mat;
    XMStoreFloat4x4(&mat, t);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(m_ctx->Map(m_cb[idx].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        memcpy(ms.pData, &mat, sizeof(mat));
        m_ctx->Unmap(m_cb[idx].Get(), 0);
    }
}

// ---------------------------------------------------------------------------
void Application::UpdateUVCB(int idx)
{
    int rot = 0;
    if (m_plane[idx].video)
        rot = m_plane[idx].video->rotationDeg.load();

    XMMATRIX m = XMMatrixIdentity();
    if (rot == 90 || rot == 180 || rot == 270)
    {
        // Rotate around UV center (0.5, 0.5).
        // Negate angle: UV Y-axis points down, so a positive rotation in
        // degrees maps to a negative Z-rotation in the XMMatrix convention.
        const XMMATRIX T0 = XMMatrixTranslation(-0.5f, -0.5f, 0.0f);
        const XMMATRIX T1 = XMMatrixTranslation( 0.5f,  0.5f, 0.0f);
        float angle = static_cast<float>(rot) * (XM_PI / 180.0f);
        m = T0 * XMMatrixRotationZ(-angle) * T1;
    }

    XMFLOAT4X4 mat;
    XMStoreFloat4x4(&mat, m);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(m_ctx->Map(m_uvcb[idx].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        memcpy(ms.pData, &mat, sizeof(mat));
        m_ctx->Unmap(m_uvcb[idx].Get(), 0);
    }
}

// ---------------------------------------------------------------------------
void Application::OnClick()
{
    if (m_transitioning) return;

    const int active  = m_activePlane;
    const int standby = 1 - active;

    // Pause video on the currently visible plane.
    if (m_plane[active].video)
        m_plane[active].video->paused = true;

    // Compute the next media index (with wraparound).
    int nextIdx = (m_currentIdx + 1) % static_cast<int>(m_playlist.size());

    // Position the standby plane off-screen right, then load the next item.
    m_plane[standby].offsetX = 2.0f;
    UpdateCB(standby);
    LoadMedia(standby, nextIdx);

    // Commit the new current index and start the slide animation.
    m_currentIdx   = nextIdx;
    m_transitioning = true;
    m_transProgress = 0.0f;
    m_transStart    = Clock::now();
}

// ---------------------------------------------------------------------------
void Application::Update()
{
    // --- Drive the slide animation -------------------------------------------
    if (m_transitioning)
    {
        auto   now     = Clock::now();
        float  elapsed = std::chrono::duration<float>(now - m_transStart).count();
        m_transProgress = elapsed / TRANSITION_SECONDS;

        if (m_transProgress >= 1.0f)
        {
            // Animation complete: snap planes to final positions.
            m_transProgress = 1.0f;
            m_transitioning = false;

            const int newActive = 1 - m_activePlane;
            const int oldActive = m_activePlane;

            m_plane[newActive].offsetX = 0.0f;
            m_plane[oldActive].offsetX = 2.0f;   // reset off-screen right
            m_activePlane = newActive;
        }
        else
        {
            float t = EaseInOut(m_transProgress);
            const int active  = m_activePlane;
            const int standby = 1 - active;

            // Active slides from 0 → -2 (exits left).
            m_plane[active].offsetX  = 0.0f  + t * (-2.0f);
            // Standby slides from 2 → 0 (enters from right).
            m_plane[standby].offsetX = 2.0f  + t * (-2.0f);
        }

        UpdateCB(0);
        UpdateCB(1);
    }

    // --- Pull new video frames into textures ---------------------------------
    for (int i = 0; i < 2; ++i)
    {
        auto& p = m_plane[i];
        if (!p.video || !p.video->newFrame.load()) continue;

        std::lock_guard<std::mutex> lk(p.video->frameMutex);
        if (p.video->newFrame && p.video->frameW > 0 && p.video->frameH > 0)
        {
            UploadPixels(i, p.video->framePixels.data(),
                         p.video->frameW, p.video->frameH);
            p.video->newFrame = false;
        }
    }

    // --- Update UV rotation transforms ---------------------------------------
    UpdateUVCB(0);
    UpdateUVCB(1);
}

// ---------------------------------------------------------------------------
void Application::Render()
{
    const float black[] = { 0.05f, 0.05f, 0.05f, 1.0f };
    m_ctx->ClearRenderTargetView(m_rtv.Get(), black);

    m_ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(m_width);
    vp.Height   = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    m_ctx->RSSetViewports(1, &vp);

    // Fixed pipeline state
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(m_il.Get());

    UINT stride = sizeof(Vertex), offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);

    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_samp.GetAddressOf());
    m_ctx->RSSetState(m_rast.Get());

    const float bf[4] = {};
    m_ctx->OMSetBlendState(m_blend.Get(), bf, 0xFFFFFFFF);

    // Draw planes that have content (back to front: active first so the
    // incoming plane is always on top at the seam).
    const int drawOrder[2] = { m_activePlane, 1 - m_activePlane };
    for (int i : drawOrder)
    {
        auto& p = m_plane[i];
        if (!p.hasContent || !p.srv) continue;

        ID3D11Buffer* cbs[2] = { m_cb[i].Get(), m_uvcb[i].Get() };
        m_ctx->VSSetConstantBuffers(0, 2, cbs);
        m_ctx->PSSetShaderResources(0, 1, p.srv.GetAddressOf());
        m_ctx->DrawIndexed(6, 0, 0);
    }

    m_sc->Present(1, 0);
}

// ---------------------------------------------------------------------------
void Application::Cleanup()
{
    for (int i = 0; i < 2; ++i)
    {
        if (m_plane[i].video)
        {
            m_plane[i].video->Stop();
            m_plane[i].video.reset();
        }
    }
    MFShutdown();
}

// ===========================================================================
//  Win32 plumbing
// ===========================================================================
static Application* g_App = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        if (g_App) g_App->OnClick();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // Main thread uses a single-threaded apartment (required by WIC).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = APP_TITLE;
    RegisterClassExW(&wc);

    // Create a fixed-size window
    RECT rc = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, APP_TITLE, APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { CoUninitialize(); return -1; }

    Application app;
    g_App = &app;

    if (!app.Initialize(hwnd, WINDOW_WIDTH, WINDOW_HEIGHT))
    {
        g_App = nullptr;
        CoUninitialize();
        return -1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Render loop
    MSG msg = {};
    for (;;)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto done;
        }
        app.Update();
        app.Render();
    }
done:
    g_App = nullptr;
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
