#include "Renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <pix3.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr uint32_t ConstantStride = 512;

    D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES result{};
        result.Type = type;
        result.CreationNodeMask = 1;
        result.VisibleNodeMask = 1;
        return result;
    }

    D3D12_RESOURCE_DESC BufferDescription(uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_RESOURCE_DESC result{};
        result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        result.Width = size;
        result.Height = 1;
        result.DepthOrArraySize = 1;
        result.MipLevels = 1;
        result.SampleDesc.Count = 1;
        result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result.Flags = flags;
        return result;
    }

    bool Failed(HRESULT hr) { return FAILED(hr); }
}

Renderer::~Renderer()
{
    Shutdown();
}

void Renderer::SetError(const std::string& message)
{
    m_lastError = message;
    OutputDebugStringA((message + "\n").c_str());
}

bool Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_hwnd = hwnd;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);

    wchar_t executablePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    const std::filesystem::path volumePath =
        std::filesystem::path(executablePath).parent_path() / L"Assets" / L"Altocumlus.vdb";
    if (!LoadVdbVolume(volumePath, m_volumeData, m_lastError))
        return false;

    if (!CreateDeviceAndSwapchain(hwnd) || !CreateCoreResources() ||
        !CreateSizeDependentResources() || !CreateVolumeResources(64) ||
        !CreateVdbResources() || !CreatePipelines())
    {
        Shutdown();
        return false;
    }

    if (!ImGui_ImplWin32_Init(hwnd) ||
        !ImGui_ImplDX12_Init(m_device.Get(), FrameCount, BackBufferFormat, m_srvHeap.Get(),
            CpuHandle(ImGuiFont), GpuHandle(ImGuiFont)))
    {
        SetError("Failed to initialize Dear ImGui backends.");
        Shutdown();
        return false;
    }

    m_initialized = true;
    return true;
}

bool Renderer::CreateDeviceAndSwapchain(HWND hwnd)
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif

    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (Failed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
    {
        SetError("CreateDXGIFactory2 failed.");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;
         SUCCEEDED(m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)));
         ++index)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
            break;
        adapter.Reset();
    }

    if (!m_device)
    {
        SetError("No Direct3D 12 capable hardware adapter was found.");
        return false;
    }

    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,
            &options1, sizeof(options1))) && options1.WaveLaneCountMin > 0)
        {
            m_waveLaneCount = options1.WaveLaneCountMin;
        }
        else
        {
            m_waveLaneCount = 32;
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (Failed(m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_queue))))
    {
        SetError("CreateCommandQueue failed.");
        return false;
    }
    m_queue->GetTimestampFrequency(&m_timestampFrequency);

    DXGI_SWAP_CHAIN_DESC1 swapchainDescription{};
    swapchainDescription.BufferCount = FrameCount;
    swapchainDescription.Width = m_width;
    swapchainDescription.Height = m_height;
    swapchainDescription.Format = BackBufferFormat;
    swapchainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDescription.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapchain;
    if (Failed(m_factory->CreateSwapChainForHwnd(
        m_queue.Get(), hwnd, &swapchainDescription, nullptr, nullptr, &swapchain)) ||
        Failed(swapchain.As(&m_swapchain)))
    {
        SetError("CreateSwapChainForHwnd failed.");
        return false;
    }
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
    return true;
}

bool Renderer::CreateCoreResources()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = FrameCount;
    if (Failed(m_device->CreateDescriptorHeap(&rtvDescription, IID_PPV_ARGS(&m_rtvHeap))))
    {
        SetError("Failed to create RTV descriptor heap.");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
    srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDescription.NumDescriptors = DescriptorCount;
    srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (Failed(m_device->CreateDescriptorHeap(&srvDescription, IID_PPV_ARGS(&m_srvHeap))))
    {
        SetError("Failed to create shader-visible descriptor heap.");
        return false;
    }
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (FrameContext& frame : m_frames)
    {
        if (Failed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))))
        {
            SetError("CreateCommandAllocator failed.");
            return false;
        }
    }
    if (Failed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList))))
    {
        SetError("CreateCommandList failed.");
        return false;
    }
    m_commandList->Close();

    if (Failed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
    {
        SetError("CreateFence failed.");
        return false;
    }
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
    {
        SetError("CreateEvent failed.");
        return false;
    }

    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Count = FrameCount * TimestampCount;
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (Failed(m_device->CreateQueryHeap(&queryDescription, IID_PPV_ARGS(&m_queryHeap))))
    {
        SetError("CreateQueryHeap failed.");
        return false;
    }

    auto readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    for (auto& readback : m_timestampReadbacks)
    {
        auto description = BufferDescription(TimestampCount * sizeof(uint64_t));
        if (Failed(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        {
            SetError("Failed to create timestamp readback buffer.");
            return false;
        }
    }

    auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    // Three CBV slots per frame: scene (b0 for the render pass), build (b0 for
    // the primary cache-build dispatch, may hold a predictive-target light dir),
    // and seedB-build (b0 for the second cache-build dispatch on sub-Nyquist
    // toggle-on, holding the look-ahead direction to populate physical B).
    auto constantsDescription = BufferDescription(FrameCount * ConstantStride * 3);
    if (Failed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &constantsDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer))) ||
        Failed(m_constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_constantData))))
    {
        SetError("Failed to create constant upload buffer.");
        return false;
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::CpuHandle(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::GpuHandle(uint32_t index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::GetOutputSrv() const
{
    return GpuHandle(OutputSrv);
}

bool Renderer::CreateSizeDependentResources()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FrameCount; ++i)
    {
        if (Failed(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]))))
        {
            SetError("Failed to acquire a swapchain back buffer.");
            return false;
        }
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvDescriptorSize;
    }

    D3D12_RESOURCE_DESC textureDescription{};
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = m_width;
    textureDescription.Height = m_height;
    textureDescription.DepthOrArraySize = 1;
    textureDescription.MipLevels = 1;
    textureDescription.Format = BackBufferFormat;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_outputTexture))))
    {
        SetError("Failed to create the raymarch output texture.");
        return false;
    }
    m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = BackBufferFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_outputTexture.Get(), nullptr, &uav, CpuHandle(OutputUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = BackBufferFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_outputTexture.Get(), &srv, CpuHandle(OutputSrv));

    // Tile classification: one uint per 8x8 tile
    const uint32_t tileCountX = (m_width + 7) / 8;
    const uint32_t tileCountY = (m_height + 7) / 8;
    const uint32_t tileCount = tileCountX * tileCountY;
    auto tileDescription = BufferDescription(tileCount * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &tileDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_tileClassificationBuffer))))
    {
        SetError("Failed to create tile classification buffer.");
        return false;
    }
    m_tileClassificationState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC tileUav{};
        tileUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        tileUav.Format = DXGI_FORMAT_UNKNOWN;
        tileUav.Buffer.NumElements = tileCount;
        tileUav.Buffer.StructureByteStride = sizeof(uint32_t);
        m_device->CreateUnorderedAccessView(m_tileClassificationBuffer.Get(), nullptr, &tileUav,
            CpuHandle(TileClassificationUav));
        D3D12_SHADER_RESOURCE_VIEW_DESC tileSrv{};
        tileSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        tileSrv.Format = DXGI_FORMAT_UNKNOWN;
        tileSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tileSrv.Buffer.NumElements = tileCount;
        tileSrv.Buffer.StructureByteStride = sizeof(uint32_t);
        m_device->CreateShaderResourceView(m_tileClassificationBuffer.Get(), &tileSrv,
            CpuHandle(TileClassificationSrv));
    }

    // Hit-interval buffer for the split raymarch pipeline (option 1 A/B experiment):
    // one float2 per pixel storing (firstHitT, lastHitT) — the DDA-derived non-empty
    // slab of the ray produced by FindHitIntervalCS and consumed by SplitCachedRenderCS.
    const uint32_t pixelCount = m_width * m_height;
    const uint32_t hitStride = sizeof(float) * 2;
    auto hitIntervalDescription = BufferDescription(
        static_cast<uint64_t>(pixelCount) * hitStride,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &hitIntervalDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_hitIntervalBuffer))))
    {
        SetError("Failed to create hit-interval buffer.");
        return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC hitUav{};
    hitUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    hitUav.Format = DXGI_FORMAT_UNKNOWN;
    hitUav.Buffer.NumElements = pixelCount;
    hitUav.Buffer.StructureByteStride = hitStride;
    m_device->CreateUnorderedAccessView(m_hitIntervalBuffer.Get(), nullptr, &hitUav,
        CpuHandle(HitIntervalUav));

    // Option 2 (tile-variant dispatch) resources: one compacted non-sky tile list
    // and a small indirect-args buffer holding a single D3D12_DISPATCH_ARGUMENTS.
    const uint32_t tileGridX = (m_width + 7) / 8;
    const uint32_t tileGridY = (m_height + 7) / 8;
    const uint32_t tileGridCount = tileGridX * tileGridY;
    const uint32_t tileListStride = sizeof(uint32_t);
    auto tileListDescription = BufferDescription(
        static_cast<uint64_t>(tileGridCount) * tileListStride,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &tileListDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(m_tileList.ReleaseAndGetAddressOf()))))
    {
        SetError("Failed to create tile list buffer.");
        return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC tileListUavDesc{};
    tileListUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    tileListUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    tileListUavDesc.Buffer.NumElements = tileGridCount;
    tileListUavDesc.Buffer.StructureByteStride = tileListStride;
    m_device->CreateUnorderedAccessView(m_tileList.Get(), nullptr, &tileListUavDesc,
        CpuHandle(TileListUav));

    // 3 uints = 1 x D3D12_DISPATCH_ARGUMENTS. Kept in UAV state between frames and
    // transitioned to INDIRECT_ARGUMENT immediately before ExecuteIndirect.
    const uint32_t argCount = 3;
    auto argsDescription = BufferDescription(
        static_cast<uint64_t>(argCount) * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &argsDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_indirectDispatchArgs))))
    {
        SetError("Failed to create indirect dispatch args buffer.");
        return false;
    }
    m_indirectDispatchArgsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_UNORDERED_ACCESS_VIEW_DESC argsUavDesc{};
    argsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    argsUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    argsUavDesc.Buffer.NumElements = argCount;
    argsUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    m_device->CreateUnorderedAccessView(m_indirectDispatchArgs.Get(), nullptr, &argsUavDesc,
        CpuHandle(IndirectDispatchArgsUav));

    // Per-pixel DenseDensityAt sample-count texture (R32_UINT). Sized to render dims;
    // populated by *_CountCS variants and consumed by HeatmapDisplayCS.
    D3D12_RESOURCE_DESC sampleCountDescription = textureDescription;
    sampleCountDescription.Format = DXGI_FORMAT_R32_UINT;
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &sampleCountDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(m_sampleCountTexture.ReleaseAndGetAddressOf()))))
    {
        SetError("Failed to create sample-count texture.");
        return false;
    }
    m_sampleCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_UNORDERED_ACCESS_VIEW_DESC sampleCountUav{};
    sampleCountUav.Format = DXGI_FORMAT_R32_UINT;
    sampleCountUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_sampleCountTexture.Get(), nullptr, &sampleCountUav,
        CpuHandle(SampleCountUav));

    // Temporal reuse: R8_UINT per-pixel reuse marker (1 = pixel filled from history,
    // 0 = pixel needs marching this frame). Cleared to 0 each frame; TemporalReprojectCS
    // fills valid pixels and writes 1 here + colour into Output.
    D3D12_RESOURCE_DESC reuseDescription = textureDescription;
    reuseDescription.Format = DXGI_FORMAT_R8_UINT;
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &reuseDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(m_reuseValidTexture.ReleaseAndGetAddressOf()))))
    {
        SetError("Failed to create reuse-validity texture.");
        return false;
    }
    m_reuseValidState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_UNORDERED_ACCESS_VIEW_DESC reuseUav{};
    reuseUav.Format = DXGI_FORMAT_R8_UINT;
    reuseUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_reuseValidTexture.Get(), nullptr, &reuseUav,
        CpuHandle(ReuseValidUav));

    // Temporal reuse: history colour target. Matches Output format so we can CopyResource
    // the two directly. Starts in COPY_DEST; transitioned to NON_PIXEL_SHADER_RESOURCE for
    // sampling by TemporalReprojectCS.
    D3D12_RESOURCE_DESC historyDescription = textureDescription;
    historyDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &historyDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(m_historyColorTexture.ReleaseAndGetAddressOf()))))
    {
        SetError("Failed to create history colour texture.");
        return false;
    }
    m_historyColorState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_SHADER_RESOURCE_VIEW_DESC historySrv{};
    historySrv.Format = historyDescription.Format;
    historySrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    historySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    historySrv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_historyColorTexture.Get(), &historySrv,
        CpuHandle(HistoryColorSrv));
    m_temporalHistoryValid = false;

    return true;
}

bool Renderer::CreateVolumeResources(uint32_t resolution)
{
    if (m_densityBuffer)
        WaitForGpu();

    m_gridResolution = std::clamp(resolution, 16u, 256u);
    const uint64_t columnCount = static_cast<uint64_t>(m_gridResolution) * m_gridResolution;
    const uint64_t cellCount = columnCount * m_gridResolution;
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);

    auto createUavBuffer = [&](uint64_t bytes, ComPtr<ID3D12Resource>& resource) -> bool
    {
        auto description = BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)));
    };

    if (!createUavBuffer(sizeof(uint32_t) * 2, m_densityBuffer) ||
        !createUavBuffer(sizeof(uint32_t), m_linksBuffer) ||
        !createUavBuffer(sizeof(uint32_t) * 2, m_statsBuffer))
    {
        SetError("Failed to create volume cache buffers.");
        return false;
    }

    D3D12_RESOURCE_DESC coarseDepthDescription{};
    coarseDepthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    coarseDepthDescription.Width = m_gridResolution;
    coarseDepthDescription.Height = m_gridResolution;
    coarseDepthDescription.DepthOrArraySize = static_cast<UINT16>(m_gridResolution);
    coarseDepthDescription.MipLevels = 1;
    coarseDepthDescription.Format = DXGI_FORMAT_R16_FLOAT;
    coarseDepthDescription.SampleDesc.Count = 1;
    coarseDepthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    coarseDepthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &coarseDepthDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_transmittanceBuffer))))
    {
        SetError("Failed to create cached depth texture.");
        return false;
    }
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &coarseDepthDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_transmittanceBufferB))))
    {
        SetError("Failed to create cached depth texture B.");
        return false;
    }
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &coarseDepthDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_transmittanceBufferC))))
    {
        SetError("Failed to create cached depth texture C.");
        return false;
    }

    auto readbackDescription = BufferDescription(sizeof(uint32_t) * 2);
    for (auto& readback : m_statsReadbacks)
    {
        if (Failed(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        {
            SetError("Failed to create volume statistics readback.");
            return false;
        }
    }

    auto createUav = [&](ID3D12Resource* resource, uint32_t index, uint32_t elements, uint32_t stride)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
        description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.Buffer.NumElements = elements;
        description.Buffer.StructureByteStride = stride;
        m_device->CreateUnorderedAccessView(resource, nullptr, &description, CpuHandle(index));
    };
    auto createSrv = [&](ID3D12Resource* resource, uint32_t index, uint32_t elements, uint32_t stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Buffer.NumElements = elements;
        description.Buffer.StructureByteStride = stride;
        m_device->CreateShaderResourceView(resource, &description, CpuHandle(index));
    };

    const uint32_t cells = static_cast<uint32_t>(cellCount);
    createUav(m_densityBuffer.Get(), DensityUav, 1, sizeof(uint32_t) * 2);
    createUav(m_linksBuffer.Get(), LinksUav, 1, sizeof(uint32_t));
    createUav(m_statsBuffer.Get(), StatsUav, 2, sizeof(uint32_t));
    createSrv(m_densityBuffer.Get(), DensitySrv, 1, sizeof(uint32_t) * 2);
    createSrv(m_linksBuffer.Get(), LinksSrv, 1, sizeof(uint32_t));

    D3D12_UNORDERED_ACCESS_VIEW_DESC coarseUav{};
    coarseUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    coarseUav.Format = DXGI_FORMAT_R16_FLOAT;
    coarseUav.Texture3D.WSize = static_cast<UINT>(m_gridResolution);
    m_device->CreateUnorderedAccessView(m_transmittanceBuffer.Get(), nullptr, &coarseUav,
        CpuHandle(TransmittanceUav));
    D3D12_SHADER_RESOURCE_VIEW_DESC coarseSrv{};
    coarseSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    coarseSrv.Format = DXGI_FORMAT_R16_FLOAT;
    coarseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coarseSrv.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_transmittanceBuffer.Get(), &coarseSrv,
        CpuHandle(TransmittanceSrv));
    // Buffer B SRV slot (t11 in the shader). Feature-off default: point at A so
    // shader lerp(A, B, 0) is bit-exact identical to reading A alone. When
    // sub-Nyquist is enabled and B has been populated, this descriptor is
    // rewritten to point at m_transmittanceBufferB.
    m_device->CreateShaderResourceView(m_transmittanceBuffer.Get(), &coarseSrv,
        CpuHandle(TransmittanceSrvB));

    m_densityState = m_linksState = m_transmittanceState = m_transmittanceStateB =
        m_transmittanceStateC =
        m_statsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_cacheBothPopulated = false;
    m_cacheHasValidB = false;
    m_cacheOldPhysical = 0;
    m_cacheBlend = 0.0f;
    ResetAmortizedCacheState();
    m_subNyquistWasEnabled = false;
    m_stats.totalCells = cells;
    m_stats.cacheBytes = cellCount * sizeof(uint16_t) * 3ull +
        m_volumeData.denseHalf.size() * sizeof(uint16_t) +
        m_volumeData.pageTable.size() * sizeof(uint32_t) +
        m_volumeData.brickPool.size() * sizeof(float);
    m_cacheValid = false;
    return true;
}

bool Renderer::CreateVdbResources()
{
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    std::vector<ComPtr<ID3D12Resource>> uploads;

    auto createTexture = [&](uint32_t width, uint32_t height, uint32_t depth, DXGI_FORMAT format,
        const void* source, uint32_t sourceRowBytes, uint32_t sourceSliceBytes,
        ComPtr<ID3D12Resource>& texture) -> bool
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = static_cast<UINT16>(depth);
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
            return false;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 uploadBytes = 0;
        m_device->GetCopyableFootprints(&description, 0, 1, 0,
            &footprint, &rowCount, &rowSize, &uploadBytes);
        ComPtr<ID3D12Resource> upload;
        auto uploadDescription = BufferDescription(uploadBytes);
        if (Failed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
            return false;

        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        const uint8_t* sourceBytes = static_cast<const uint8_t*>(source);
        for (uint32_t z = 0; z < depth; ++z)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                std::memcpy(mapped + footprint.Offset +
                    static_cast<size_t>(z) * footprint.Footprint.RowPitch * height +
                    static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    sourceBytes + static_cast<size_t>(z) * sourceSliceBytes +
                    static_cast<size_t>(y) * sourceRowBytes, sourceRowBytes);
            }
        }
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = upload.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        uploads.push_back(upload);
        return true;
    };

    m_frames[0].allocator->Reset();
    m_commandList->Reset(m_frames[0].allocator.Get(), nullptr);

    if (!createTexture(m_volumeData.width, m_volumeData.height, m_volumeData.depth,
        DXGI_FORMAT_R16_FLOAT, m_volumeData.denseHalf.data(),
        m_volumeData.width * sizeof(uint16_t),
        m_volumeData.width * m_volumeData.height * sizeof(uint16_t), m_denseVolumeTexture) ||
        !createTexture(m_volumeData.pageWidth, m_volumeData.pageHeight, m_volumeData.pageDepth,
        DXGI_FORMAT_R32_UINT, m_volumeData.pageTable.data(),
        m_volumeData.pageWidth * sizeof(uint32_t),
        m_volumeData.pageWidth * m_volumeData.pageHeight * sizeof(uint32_t), m_sparsePageTexture))
    {
        SetError("Failed to create VDB 3D textures.");
        return false;
    }

    auto createUavTexture3D = [&](uint32_t width, uint32_t height, uint32_t depth,
        DXGI_FORMAT format, ComPtr<ID3D12Resource>& resource) -> bool
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = static_cast<UINT16>(depth);
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&resource)));
    };
    auto createUavTexture3DMipped = [&](uint32_t width, uint32_t height, uint32_t depth,
        uint32_t mipLevels, DXGI_FORMAT format, ComPtr<ID3D12Resource>& resource) -> bool
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = static_cast<UINT16>(depth);
        description.MipLevels = static_cast<UINT16>(mipLevels);
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&resource)));
    };
    // RemappedVolume gets 4 mip levels: mip0 = full res, mip3 = 1/8 res used as coarse occupancy.
    if (!createUavTexture3DMipped(m_volumeData.width, m_volumeData.height, m_volumeData.depth,
            4, DXGI_FORMAT_R16_FLOAT, m_remappedVolumeTexture))
    {
        SetError("Failed to create remapped volume.");
        return false;
    }
    m_remappedVolumeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_bakeVolumesPending = true;
    m_bakedDensityThreshold = std::numeric_limits<float>::quiet_NaN();
    m_bakedDensityGamma = std::numeric_limits<float>::quiet_NaN();
    m_bakedDensityScale = std::numeric_limits<float>::quiet_NaN();

    // Normal LUT: R8G8B8A8_SNORM raw normals sampled during trace. Sized via CreateNormalLUT.
    if (!CreateNormalLUT(std::max<uint32_t>(m_gridResolution, 128u)))
    {
        return false;
    }

    // Empty-space distance field: two 128^3 R8_UNORM textures for ping-pong propagation.
    // The stored range only uses [0, 16/128 = 0.125] normalised distances, so we scale
    // by DF_ENCODE_SCALE (=8.0) at bake time to use the full R8_UNORM range and get
    // ~1/(255*DF_DECODE_SCALE) = ~0.5-cell precision, which is well above the DDA's
    // floor()'d cell-count granularity. Halves texture bandwidth vs R16_FLOAT and
    // doubles L1TEX cache density — meaningful on this texture-latency-bound shader.
    const uint32_t dfRes = 128;
    const DXGI_FORMAT dfFormat = DXGI_FORMAT_R8_UNORM;
    if (!createUavTexture3D(dfRes, dfRes, dfRes, dfFormat, m_distanceFieldTextureA) ||
        !createUavTexture3D(dfRes, dfRes, dfRes, dfFormat, m_distanceFieldTextureB))
    {
        SetError("Failed to create distance-field textures.");
        return false;
    }
    m_distanceFieldAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_distanceFieldBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC dfUav{};
        dfUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        dfUav.Format = dfFormat;
        dfUav.Texture3D.MipSlice = 0;
        dfUav.Texture3D.WSize = dfRes;
        m_device->CreateUnorderedAccessView(m_distanceFieldTextureA.Get(), nullptr, &dfUav,
            CpuHandle(DistanceFieldAUav));
        m_device->CreateUnorderedAccessView(m_distanceFieldTextureB.Get(), nullptr, &dfUav,
            CpuHandle(DistanceFieldBUav));

        D3D12_SHADER_RESOURCE_VIEW_DESC dfSrv{};
        dfSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dfSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        dfSrv.Texture3D.MipLevels = 1;
        dfSrv.Format = dfFormat;
        m_device->CreateShaderResourceView(m_distanceFieldTextureA.Get(), &dfSrv,
            CpuHandle(DistanceFieldSrv));
    }

    const uint64_t brickBytes = m_volumeData.brickPool.size() * sizeof(float);
    auto brickDescription = BufferDescription(brickBytes);
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &brickDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_sparseBrickBuffer))))
    {
        SetError("Failed to create sparse VDB brick pool.");
        return false;
    }
    ComPtr<ID3D12Resource> brickUpload;
    if (Failed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &brickDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&brickUpload))))
    {
        SetError("Failed to create sparse VDB upload buffer.");
        return false;
    }
    void* mappedBricks = nullptr;
    brickUpload->Map(0, nullptr, &mappedBricks);
    std::memcpy(mappedBricks, m_volumeData.brickPool.data(), static_cast<size_t>(brickBytes));
    brickUpload->Unmap(0, nullptr);
    m_commandList->CopyBufferRegion(m_sparseBrickBuffer.Get(), 0, brickUpload.Get(), 0, brickBytes);
    uploads.push_back(brickUpload);

    D3D12_RESOURCE_BARRIER barriers[3]{};
    ID3D12Resource* resources[3] = {
        m_denseVolumeTexture.Get(), m_sparsePageTexture.Get(), m_sparseBrickBuffer.Get()
    };
    for (uint32_t i = 0; i < 3; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = resources[i];
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    m_commandList->ResourceBarrier(3, barriers);
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{};
    textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    textureSrv.Texture3D.MipLevels = 1;
    textureSrv.Format = DXGI_FORMAT_R16_FLOAT;
    m_device->CreateShaderResourceView(m_denseVolumeTexture.Get(), &textureSrv,
        CpuHandle(DenseVolumeSrv));
    textureSrv.Format = DXGI_FORMAT_R32_UINT;
    m_device->CreateShaderResourceView(m_sparsePageTexture.Get(), &textureSrv,
        CpuHandle(SparsePageSrv));

    D3D12_SHADER_RESOURCE_VIEW_DESC brickSrv{};
    brickSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    brickSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    brickSrv.Format = DXGI_FORMAT_UNKNOWN;
    brickSrv.Buffer.NumElements = static_cast<UINT>(m_volumeData.brickPool.size());
    brickSrv.Buffer.StructureByteStride = sizeof(float);
    m_device->CreateShaderResourceView(m_sparseBrickBuffer.Get(), &brickSrv,
        CpuHandle(SparseBrickSrv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC bakedUav{};
    bakedUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    bakedUav.Format = DXGI_FORMAT_R16_FLOAT;
    bakedUav.Texture3D.MipSlice = 0;
    bakedUav.Texture3D.WSize = m_volumeData.depth;
    m_device->CreateUnorderedAccessView(m_remappedVolumeTexture.Get(), nullptr, &bakedUav,
        CpuHandle(RemappedVolumeUav));
    bakedUav.Texture3D.MipSlice = 3;
    bakedUav.Texture3D.WSize = std::max<UINT>(m_volumeData.depth >> 3, 1);
    m_device->CreateUnorderedAccessView(m_remappedVolumeTexture.Get(), nullptr, &bakedUav,
        CpuHandle(RemappedVolumeMip3Uav));

    D3D12_SHADER_RESOURCE_VIEW_DESC bakedSrv{};
    bakedSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bakedSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    bakedSrv.Texture3D.MipLevels = 4;
    bakedSrv.Format = DXGI_FORMAT_R16_FLOAT;
    m_device->CreateShaderResourceView(m_remappedVolumeTexture.Get(), &bakedSrv,
        CpuHandle(RemappedVolumeSrv));

    m_stats.volumeWidth = m_volumeData.width;
    m_stats.volumeHeight = m_volumeData.height;
    m_stats.volumeDepth = m_volumeData.depth;
    m_stats.activeBricks = m_volumeData.ActiveBrickCount();
    m_stats.sourceActiveVoxels = m_volumeData.sourceActiveVoxels;
    m_stats.denseVolumeBytes = m_volumeData.denseHalf.size() * sizeof(uint16_t);
    m_stats.sparseVolumeBytes = m_volumeData.pageTable.size() * sizeof(uint32_t) +
        m_volumeData.brickPool.size() * sizeof(float);
    return true;
}

bool Renderer::CreateNormalLUT(uint32_t resolution)
{
    const uint32_t clamped = std::clamp(resolution, 64u, 512u);
    if (m_normalLUTTexture && m_normalLUTResolution == clamped)
        return true;
    if (m_normalLUTTexture)
        WaitForGpu();

    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = clamped;
    description.Height = clamped;
    description.DepthOrArraySize = static_cast<UINT16>(clamped);
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    m_normalLUTTexture.Reset();
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_normalLUTTexture))))
    {
        SetError("Failed to create normal LUT texture.");
        return false;
    }
    m_normalLUTTextureB.Reset();
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_normalLUTTextureB))))
    {
        SetError("Failed to create normal LUT B texture.");
        return false;
    }
    m_normalLUTTextureC.Reset();
    if (Failed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_normalLUTTextureC))))
    {
        SetError("Failed to create normal LUT C texture.");
        return false;
    }
    m_normalLUTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_normalLUTStateB = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_normalLUTStateC = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_normalLUTResolution = clamped;
    m_normalLUTBaked = false;
    m_normalLUTBBaked = false;
    m_normalLUTBValid = false;
    m_normalLUTBakedSlots = {};
    m_stats.normalLUTBytes =
        static_cast<uint64_t>(clamped) * clamped * clamped * 4ull * 3ull; // A + B + C

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    uav.Texture3D.MipSlice = 0;
    uav.Texture3D.WSize = clamped;
    m_device->CreateUnorderedAccessView(m_normalLUTTexture.Get(), nullptr, &uav,
        CpuHandle(NormalLUTUav));
    m_device->CreateUnorderedAccessView(m_normalLUTTextureB.Get(), nullptr, &uav,
        CpuHandle(NormalLUTBUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = 1;
    srv.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    m_device->CreateShaderResourceView(m_normalLUTTexture.Get(), &srv,
        CpuHandle(NormalLUTSrv));
    // LUT_B SRV initially aliases LUT_A (identical descriptor) so that runtime
    // sampling of NormalLUTB with blend weight=0 returns the same as LUT_A. On
    // the first bake into LUT_B (sub-Nyquist cross-fade), we recreate this SRV
    // to point at the real B resource — matches the CachedDepthReadB pattern.
    m_device->CreateShaderResourceView(m_normalLUTTexture.Get(), &srv,
        CpuHandle(NormalLUTSrvB));
    return true;
}

bool Renderer::CreatePipelines()
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 17;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 13;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 0;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants.ShaderRegister = 1;
    parameters[3].Constants.Num32BitValues = 4;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = _countof(parameters);
    rootDescription.pParameters = parameters;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    rootDescription.NumStaticSamplers = 1;
    rootDescription.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized, errors;
    if (Failed(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors)) ||
        Failed(m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature))))
    {
        SetError(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Root signature creation failed.");
        return false;
    }

    wchar_t executablePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    const std::filesystem::path shaderDirectory =
        std::filesystem::path(executablePath).parent_path() / L"shaders";

    struct PipelineEntry { const char* entry; ComPtr<ID3D12PipelineState>* target; };
    PipelineEntry entries[] = {
        { "ResolveColumnsCS", &m_resolvePso },
        { "NaiveRenderCS", &m_naivePso }, { "CachedRenderCS", &m_cachedPso },
        { "CachedRenderLutCS", &m_cachedLutPso },
        { "CachedRenderDFCS", &m_cachedDfPso },
        { "CachedRenderDFLutCS", &m_cachedDfLutPso },
        { "CachedRenderNoSubNCS", &m_cachedNoSubNPso },
        { "CachedRenderLutNoSubNCS", &m_cachedLutNoSubNPso },
        { "CachedRenderDFNoSubNCS", &m_cachedDfNoSubNPso },
        { "CachedRenderDFLutNoSubNCS", &m_cachedDfLutNoSubNPso },
        { "CachedRenderNoSNCS", &m_cachedNoSNPso },
        { "CachedRenderLutNoSNCS", &m_cachedLutNoSNPso },
        { "CachedRenderDFNoSNCS", &m_cachedDfNoSNPso },
        { "CachedRenderDFLutNoSNCS", &m_cachedDfLutNoSNPso },
        { "CachedRenderDebugCS", &m_cachedDebugPso },
        { "CachedRenderReducedTapCS", &m_cachedReducedTapPso },
        { "CachedRenderDFReducedTapCS", &m_cachedDfReducedTapPso },
        { "CachedRenderCountCS", &m_cachedCountPso },
        { "CachedRenderLutCountCS", &m_cachedLutCountPso },
        { "CachedRenderDFCountCS", &m_cachedDfCountPso },
        { "CachedRenderDFLutCountCS", &m_cachedDfLutCountPso },
        { "CachedRenderNoSNCountCS", &m_cachedNoSNCountPso },
        { "CachedRenderLutNoSNCountCS", &m_cachedLutNoSNCountPso },
        { "CachedRenderDFNoSNCountCS", &m_cachedDfNoSNCountPso },
        { "CachedRenderDFLutNoSNCountCS", &m_cachedDfLutNoSNCountPso },
        { "CachedRenderReducedTapCountCS", &m_cachedReducedTapCountPso },
        { "CachedRenderDFReducedTapCountCS", &m_cachedDfReducedTapCountPso },
        { "NaiveRenderCountCS", &m_naiveCountPso },
        { "HeatmapDisplayCS", &m_heatmapDisplayPso },
        { "TemporalReprojectCS", &m_temporalReprojectPso },
        { "TemporalReuseDebugCS", &m_temporalReuseDebugPso },
        { "BakeRemappedVolumeCS", &m_bakeRemappedPso },
        { "BakeOccupancyCS", &m_bakeOccupancyPso },
        { "ClassifyTilesCS", &m_classifyTilesPso },
        { "DebugTileOverlayCS", &m_debugTileOverlayPso },
        { "BakeNormalLUTCS", &m_bakeNormalLUTPso },
        { "BakeNormalLUTBCS", &m_bakeNormalLUTBPso },
        { "BakeDistanceFieldInitCS", &m_bakeDistanceFieldInitPso },
        { "BakeDistanceFieldPropagateCS", &m_bakeDistanceFieldPropagatePso },
        { "DebugDistanceFieldCS", &m_debugDistanceFieldPso },
        { "FindHitIntervalCS", &m_findHitIntervalPso },
        { "SplitCachedRenderCS", &m_splitCachedPso },
        { "ClearTileDispatchCS", &m_clearTileDispatchPso },
        { "SplitCachedRenderIndirectCS", &m_splitCachedIndirectPso },
        { "SplitCachedSkyCS", &m_splitCachedSkyPso }
    };
    for (PipelineEntry& pipeline : entries)
    {
        const std::filesystem::path shaderPath = shaderDirectory /
            (std::string(pipeline.entry) + ".cso");
        std::ifstream stream(shaderPath, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            SetError("Failed to open DXC shader blob: " + shaderPath.string());
            return false;
        }
        const std::streamsize size = stream.tellg();
        stream.seekg(0, std::ios::beg);
        std::vector<uint8_t> shader(static_cast<size_t>(size));
        if (!stream.read(reinterpret_cast<char*>(shader.data()), size))
        {
            SetError("Failed to read DXC shader blob: " + shaderPath.string());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_rootSignature.Get();
        description.CS = { shader.data(), shader.size() };
        if (Failed(m_device->CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.target->ReleaseAndGetAddressOf()))))
        {
            SetError(std::string("Failed to create compute pipeline: ") + pipeline.entry);
            return false;
        }
    }

    // Command signature for ExecuteIndirect(Dispatch) used by the tile-variant path.
    D3D12_INDIRECT_ARGUMENT_DESC indirectArg{};
    indirectArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
    sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &indirectArg;
    if (Failed(m_device->CreateCommandSignature(&sigDesc, nullptr,
        IID_PPV_ARGS(&m_dispatchSignature))))
    {
        SetError("Failed to create dispatch command signature.");
        return false;
    }
    return true;
}

void Renderer::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current,
    D3D12_RESOURCE_STATES next)
{
    if (current == next)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = current;
    barrier.Transition.StateAfter = next;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);
    current = next;
}

void Renderer::UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    m_commandList->ResourceBarrier(1, &barrier);
}

ID3D12Resource* Renderer::PhysicalCache(uint32_t slot) const
{
    switch (slot)
    {
    case 1: return m_transmittanceBufferB.Get();
    case 2: return m_transmittanceBufferC.Get();
    default: return m_transmittanceBuffer.Get();
    }
}

ID3D12Resource* Renderer::PhysicalNormalLUT(uint32_t slot) const
{
    switch (slot)
    {
    case 1: return m_normalLUTTextureB.Get();
    case 2: return m_normalLUTTextureC.Get();
    default: return m_normalLUTTexture.Get();
    }
}

D3D12_RESOURCE_STATES& Renderer::PhysicalCacheState(uint32_t slot)
{
    switch (slot)
    {
    case 1: return m_transmittanceStateB;
    case 2: return m_transmittanceStateC;
    default: return m_transmittanceState;
    }
}

D3D12_RESOURCE_STATES& Renderer::PhysicalNormalLUTState(uint32_t slot)
{
    switch (slot)
    {
    case 1: return m_normalLUTStateB;
    case 2: return m_normalLUTStateC;
    default: return m_normalLUTState;
    }
}

DirectX::XMFLOAT3 Renderer::RotateLightByThreshold(const DirectX::XMFLOAT3& lightDir,
    float thresholdRad, const RenderSettings& settings) const
{
    if (!settings.rotateLight)
        return lightDir;
    const float direction = settings.lightRotationSpeed >= 0.0f ? 1.0f : -1.0f;
    const float rotAngle = thresholdRad * direction;
    const float c = std::cos(rotAngle);
    const float s = std::sin(rotAngle);
    return {
        lightDir.x * c + lightDir.z * s,
        lightDir.y,
        -lightDir.x * s + lightDir.z * c
    };
}

void Renderer::ResetAmortizedCacheState()
{
    m_slotDisplayA = 0;
    m_slotDisplayB = 1;
    m_slotBuild = 2;
    m_buildColumnCursor = 0;
    m_buildColumnsPerFrame = 0;
    m_buildActive = false;
    m_buildComplete = false;
    m_pendingProgressiveBuild = false;
    m_pendingCacheSwap = false;
}

void Renderer::RebindCacheDescriptors()
{
    if (!m_device || !m_transmittanceBuffer || !m_normalLUTTexture)
        return;

    const uint32_t displayASlot = static_cast<uint32_t>(std::clamp(m_slotDisplayA, 0, 2));
    const uint32_t displayBSlot = m_cacheBothPopulated
        ? static_cast<uint32_t>(std::clamp(m_slotDisplayB, 0, 2))
        : displayASlot;
    const uint32_t buildSlot = static_cast<uint32_t>(std::clamp(m_slotBuild, 0, 2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC coarseUav{};
    coarseUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    coarseUav.Format = DXGI_FORMAT_R16_FLOAT;
    coarseUav.Texture3D.WSize = static_cast<UINT>(m_gridResolution);
    m_device->CreateUnorderedAccessView(PhysicalCache(buildSlot), nullptr, &coarseUav,
        CpuHandle(TransmittanceUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC coarseSrv{};
    coarseSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    coarseSrv.Format = DXGI_FORMAT_R16_FLOAT;
    coarseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coarseSrv.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(PhysicalCache(displayASlot), &coarseSrv,
        CpuHandle(TransmittanceSrv));
    m_device->CreateShaderResourceView(PhysicalCache(displayBSlot), &coarseSrv,
        CpuHandle(TransmittanceSrvB));

    D3D12_UNORDERED_ACCESS_VIEW_DESC lutUav{};
    lutUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    lutUav.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    lutUav.Texture3D.MipSlice = 0;
    lutUav.Texture3D.WSize = m_normalLUTResolution;
    m_device->CreateUnorderedAccessView(PhysicalNormalLUT(displayASlot), nullptr, &lutUav,
        CpuHandle(NormalLUTUav));
    m_device->CreateUnorderedAccessView(PhysicalNormalLUT(displayBSlot), nullptr, &lutUav,
        CpuHandle(NormalLUTBUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv{};
    lutSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lutSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    lutSrv.Texture3D.MipLevels = 1;
    lutSrv.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    m_device->CreateShaderResourceView(PhysicalNormalLUT(displayASlot), &lutSrv,
        CpuHandle(NormalLUTSrv));
    m_device->CreateShaderResourceView(PhysicalNormalLUT(displayBSlot), &lutSrv,
        CpuHandle(NormalLUTSrvB));
}

void Renderer::FillLightConstants(ShaderConstants& constants,
                                  const DirectX::XMFLOAT3& lightDir) const
{
    constants.lightDirectionGrid = { lightDir.x, lightDir.y, lightDir.z,
        static_cast<float>(m_gridResolution) };

    DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(lightDir.x, lightDir.y, lightDir.z, 0.0f));
    DirectX::XMFLOAT3 fwdF; DirectX::XMStoreFloat3(&fwdF, fwd);
    DirectX::XMVECTOR reference = (std::fabs(fwdF.y) < 0.99f)
        ? DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
        : DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    DirectX::XMVECTOR right = DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(reference, fwd));
    DirectX::XMVECTOR up = DirectX::XMVector3Cross(fwd, right);

    DirectX::XMFLOAT3 rightF, upF;
    DirectX::XMStoreFloat3(&rightF, right);
    DirectX::XMStoreFloat3(&upF, up);

    const float halfX = (m_volumeData.boundsMax.x - m_volumeData.boundsMin.x) * 0.5f;
    const float halfY = (m_volumeData.boundsMax.y - m_volumeData.boundsMin.y) * 0.5f;
    const float halfZ = (m_volumeData.boundsMax.z - m_volumeData.boundsMin.z) * 0.5f;
    const float centerX = (m_volumeData.boundsMin.x + m_volumeData.boundsMax.x) * 0.5f;
    const float centerY = (m_volumeData.boundsMin.y + m_volumeData.boundsMax.y) * 0.5f;
    const float centerZ = (m_volumeData.boundsMin.z + m_volumeData.boundsMax.z) * 0.5f;

    auto pack = [&](const DirectX::XMFLOAT3& axis, DirectX::XMFLOAT4& scaled,
                    DirectX::XMFLOAT4& plain)
    {
        const float invExtent = 1.0f / std::max(2.0f * (std::fabs(axis.x) * halfX
            + std::fabs(axis.y) * halfY + std::fabs(axis.z) * halfZ), 1e-8f);
        const float centerDot = axis.x * centerX + axis.y * centerY + axis.z * centerZ;
        scaled = { axis.x * invExtent, axis.y * invExtent, axis.z * invExtent,
            0.5f - centerDot * invExtent };
        plain  = { axis.x, axis.y, axis.z, 0.0f };
    };
    pack(rightF, constants.lightCacheRightScaled,   constants.lightCacheRightAxis);
    pack(upF,    constants.lightCacheUpScaled,      constants.lightCacheUpAxis);
    pack(fwdF,   constants.lightCacheForwardScaled, constants.lightCacheForwardAxis);

    // Default B-axes mirror A. Sub-Nyquist path overrides via
    // FillLightCacheAxesB when A and B were baked at different directions.
    constants.lightCacheRightScaledB   = constants.lightCacheRightScaled;
    constants.lightCacheUpScaledB      = constants.lightCacheUpScaled;
    constants.lightCacheForwardScaledB = constants.lightCacheForwardScaled;
    constants.lightCacheRightAxisB     = constants.lightCacheRightAxis;
    constants.lightCacheUpAxisB        = constants.lightCacheUpAxis;
    constants.lightCacheForwardAxisB   = constants.lightCacheForwardAxis;
}

void Renderer::FillLightCacheAxesB(ShaderConstants& constants,
                                   const DirectX::XMFLOAT3& lightDirB) const
{
    DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(lightDirB.x, lightDirB.y, lightDirB.z, 0.0f));
    DirectX::XMFLOAT3 fwdF; DirectX::XMStoreFloat3(&fwdF, fwd);
    DirectX::XMVECTOR reference = (std::fabs(fwdF.y) < 0.99f)
        ? DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
        : DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    DirectX::XMVECTOR right = DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(reference, fwd));
    DirectX::XMVECTOR up = DirectX::XMVector3Cross(fwd, right);

    DirectX::XMFLOAT3 rightF, upF;
    DirectX::XMStoreFloat3(&rightF, right);
    DirectX::XMStoreFloat3(&upF, up);

    const float halfX = (m_volumeData.boundsMax.x - m_volumeData.boundsMin.x) * 0.5f;
    const float halfY = (m_volumeData.boundsMax.y - m_volumeData.boundsMin.y) * 0.5f;
    const float halfZ = (m_volumeData.boundsMax.z - m_volumeData.boundsMin.z) * 0.5f;
    const float centerX = (m_volumeData.boundsMin.x + m_volumeData.boundsMax.x) * 0.5f;
    const float centerY = (m_volumeData.boundsMin.y + m_volumeData.boundsMax.y) * 0.5f;
    const float centerZ = (m_volumeData.boundsMin.z + m_volumeData.boundsMax.z) * 0.5f;

    auto pack = [&](const DirectX::XMFLOAT3& axis, DirectX::XMFLOAT4& scaled)
    {
        const float invExtent = 1.0f / std::max(2.0f * (std::fabs(axis.x) * halfX
            + std::fabs(axis.y) * halfY + std::fabs(axis.z) * halfZ), 1e-8f);
        const float centerDot = axis.x * centerX + axis.y * centerY + axis.z * centerZ;
        scaled = { axis.x * invExtent, axis.y * invExtent, axis.z * invExtent,
            0.5f - centerDot * invExtent };
    };
    pack(rightF, constants.lightCacheRightScaledB);
    pack(upF,    constants.lightCacheUpScaledB);
    pack(fwdF,   constants.lightCacheForwardScaledB);
    constants.lightCacheRightAxisB   = { rightF.x, rightF.y, rightF.z, 0.0f };
    constants.lightCacheUpAxisB      = { upF.x,    upF.y,    upF.z,    0.0f };
    constants.lightCacheForwardAxisB = { fwdF.x,   fwdF.y,   fwdF.z,   0.0f };
}

void Renderer::PopulateConstants(RenderSettings& settings, float timeSeconds)
{
    // Advance per-toggle motion accumulators by the real delta since the last frame,
    // gated on the corresponding tickbox. This makes the effective angle continuous
    // across enable/disable — no visible jump when the user re-enables auto-orbit or
    // light rotation after it's been off for a while.
    float dt = 0.0f;
    if (m_hasLastFrameTime)
        dt = std::max(0.0f, timeSeconds - m_lastFrameTimeSeconds);
    m_lastFrameTimeSeconds = timeSeconds;
    m_hasLastFrameTime = true;
    if (settings.autoOrbit)
        m_orbitAccumulator += dt;
    // Light rotation drives the yaw of `settings.lightDirection` directly, so
    // the ImGui yaw slider tracks the current position and un-ticking rotate
    // simply leaves the direction where it landed (no jump, no snap-back).
    if (settings.rotateLight && dt > 0.0f)
    {
        const float angle = dt * settings.lightRotationSpeed;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float nx =  settings.lightDirection.x * c + settings.lightDirection.z * s;
        const float nz = -settings.lightDirection.x * s + settings.lightDirection.z * c;
        settings.lightDirection.x = nx;
        settings.lightDirection.z = nz;
    }

    float yaw = settings.cameraYaw + (settings.autoOrbit ? m_orbitAccumulator * 0.18f : 0.0f);
    float cp = std::cos(settings.cameraPitch);
    const float effectiveDistance = std::max(settings.cameraDistance, 0.001f);
    DirectX::XMFLOAT3 camera{
        std::sin(yaw) * cp * effectiveDistance,
        std::sin(settings.cameraPitch) * effectiveDistance,
        std::cos(yaw) * cp * effectiveDistance
    };

    DirectX::XMVECTOR light = DirectX::XMLoadFloat3(&settings.lightDirection);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(light)) < 1e-5f)
        light = DirectX::XMVectorSet(0.45f, 0.8f, -0.35f, 0.0f);
    light = DirectX::XMVector3Normalize(light);
    DirectX::XMFLOAT3 normalizedLight;
    DirectX::XMStoreFloat3(&normalizedLight, light);
    m_effectiveLightDirection = normalizedLight;

    ShaderConstants constants{};
    constants.cameraPositionTime = { camera.x, camera.y, camera.z, timeSeconds };
    constants.cameraTargetMode = { 0.0f, 0.0f, 0.0f, settings.debugVoxels ? 1.0f : 0.0f };
    // Motion-adaptive step scaling. When enabled and any camera/light motion is
    // detected this frame, use fewer, coarser steps. stepScale is bumped in inverse
    // proportion so per-frame volume coverage stays the same. Motion perceptually
    // masks the coarser sampling.
    int effectivePrimarySteps = settings.primarySteps;
    int effectiveShadowSteps = settings.shadowSteps;
    float effectiveStepScale = settings.stepScale;
    if (settings.motionAdaptiveSteps)
    {
        float camDelta = 0.0f;
        float lightDelta = 0.0f;
        if (m_motionHasReference)
        {
            camDelta = std::fabs(settings.cameraYaw - m_motionPrevYaw)
                     + std::fabs(settings.cameraPitch - m_motionPrevPitch)
                     + std::fabs(settings.cameraDistance - m_motionPrevDistance) * 0.5f;
            const DirectX::XMVECTOR l0 = DirectX::XMLoadFloat3(&m_motionPrevLightDir);
            const DirectX::XMVECTOR l1 = DirectX::XMLoadFloat3(&normalizedLight);
            const float dot01 = std::clamp(
                DirectX::XMVectorGetX(DirectX::XMVector3Dot(l0, l1)), -1.0f, 1.0f);
            lightDelta = 1.0f - dot01;
        }
        const bool inMotion = camDelta > 0.001f || lightDelta > 0.0001f
            || settings.autoOrbit || settings.rotateLight;
        if (inMotion)
        {
            constexpr float scale = 0.7f;
            effectivePrimarySteps = std::max(1,
                static_cast<int>(std::round(settings.primarySteps * scale)));
            effectiveShadowSteps = std::max(1,
                static_cast<int>(std::round(settings.shadowSteps * scale)));
            effectiveStepScale = settings.stepScale *
                static_cast<float>(settings.primarySteps) /
                static_cast<float>(effectivePrimarySteps);
        }
        m_motionPrevYaw = settings.cameraYaw;
        m_motionPrevPitch = settings.cameraPitch;
        m_motionPrevDistance = settings.cameraDistance;
        m_motionPrevLightDir = normalizedLight;
        m_motionHasReference = true;
    }
    else
    {
        m_motionHasReference = false;
    }
    constants.resolutionSteps = { static_cast<float>(m_width), static_cast<float>(m_height),
        static_cast<float>(effectivePrimarySteps), static_cast<float>(effectiveShadowSteps) };
    constants.volumeParameters = { settings.densityScale, settings.extinction,
        settings.scattering, effectiveStepScale };
    // lightDirectionGrid + lightCache*Scaled/Axis are populated at end of function
    // via FillLightConstants (once for scene, optionally again for predictive build).
    constants.volumeMin = { m_volumeData.boundsMin.x, m_volumeData.boundsMin.y,
        m_volumeData.boundsMin.z, 0.0f };
    constants.volumeMax = { m_volumeData.boundsMax.x, m_volumeData.boundsMax.y,
        m_volumeData.boundsMax.z, 0.0f };
    constants.cloudParameters = { settings.powderStrength, settings.ambientLight,
        settings.exposure, settings.contrast };
    constants.volumeGridParameters = { static_cast<float>(m_volumeData.width),
        static_cast<float>(m_volumeData.height), static_cast<float>(m_volumeData.depth),
        static_cast<float>(VolumeData::BrickSize) };
    constants.sparseGridParameters = { static_cast<float>(m_volumeData.pageWidth),
        static_cast<float>(m_volumeData.pageHeight), static_cast<float>(m_volumeData.pageDepth),
        static_cast<float>(m_volumeData.ActiveBrickCount()) };
    constants.densityTransferParameters = { settings.densityThreshold, settings.densityGamma,
        settings.shadowDensityScale, settings.displayGamma };
    constants.lightingParameters = { settings.phaseAnisotropy, settings.directLightIntensity,
        settings.multipleScattering, settings.backgroundIntensity };
    constants.lightColorParameters = { settings.lightColor.x, settings.lightColor.y,
        settings.lightColor.z, settings.lightingSkipEpsilon };
    constants.ambientColorParameters = { settings.ambientColor.x, settings.ambientColor.y,
        settings.ambientColor.z, settings.enableSurfaceNormals ? 1.0f : 0.0f };
    const float sizeX = std::max(m_volumeData.boundsMax.x - m_volumeData.boundsMin.x, 1e-6f);
    const float sizeY = std::max(m_volumeData.boundsMax.y - m_volumeData.boundsMin.y, 1e-6f);
    const float sizeZ = std::max(m_volumeData.boundsMax.z - m_volumeData.boundsMin.z, 1e-6f);
    constants.cacheParameters = { 1.0f / sizeX, 1.0f / sizeY, 1.0f / sizeZ, m_cacheBlend };

    // Scene-frame light constants (based on current effective light direction).
    FillLightConstants(constants, normalizedLight);

    // Temporal reuse: prev-frame camera pose feeds TemporalReprojectCS. On first frame
    // (m_temporalHistoryValid==false), set to current pose so reprojection is identity;
    // the CPU-side history-valid gate ensures the reproject dispatch is skipped anyway.
    if (m_temporalHistoryValid)
    {
        constants.prevCameraPositionTime = m_prevCameraPositionTime;
        constants.prevCameraTargetMode = m_prevCameraTargetMode;
    }
    else
    {
        constants.prevCameraPositionTime = constants.cameraPositionTime;
        constants.prevCameraTargetMode = constants.cameraTargetMode;
    }
    // Cache the current pose for next frame.
    m_prevCameraPositionTime = constants.cameraPositionTime;
    m_prevCameraTargetMode = constants.cameraTargetMode;

    // ---- Sub-Nyquist light-cache scheduler ------------------------------------
    // Decides whether this frame needs a visible cache handoff/rebuild and, when
    // enabled, schedules one hidden partial build into the third physical cache.
    {
        using namespace DirectX;
        const bool subN = settings.useLightCacheSubNyquist;
        const bool subNJustToggled = (subN != m_subNyquistWasEnabled);
        m_subNyquistWasEnabled = subN;
        if (m_amortizedCacheEnabled != settings.useAmortizedCache)
        {
            m_amortizedCacheEnabled = settings.useAmortizedCache;
            ResetAmortizedCacheState();
        }
        const bool amortized = subN && m_amortizedCacheEnabled;

        const bool lutJustToggledOn = settings.useNormalLUT && !m_prevUseNormalLUT;
        m_prevUseNormalLUT = settings.useNormalLUT;
        if (lutJustToggledOn)
        {
            m_normalLUTBaked = false;
            m_normalLUTBakedSlots = {};
            if (m_cacheHasValidB)
                m_forceBakeLUTABBoth = true;
        }

        const XMVECTOR curL = XMLoadFloat3(&m_effectiveLightDirection);
        const uint32_t totalColumns = m_gridResolution * m_gridResolution;
        const uint32_t fullRangeEnd = std::max(totalColumns, 1u);

        m_pendingCacheRebuild = false;
        m_pendingCacheWriteSlot = 0;
        m_pendingFirstBWrite = false;
        m_pendingPredictiveBuild = false;
        m_pendingProgressiveBuild = false;
        m_pendingCacheSwap = false;
        m_pendingCacheBuildRangeStart = 0;
        m_pendingCacheBuildRangeEnd = fullRangeEnd;
        m_cachePredictedLightDir = m_effectiveLightDirection;

        if (!subN)
        {
            ResetAmortizedCacheState();
            m_cacheBlend = 0.0f;
            if (subNJustToggled)
            {
                m_cacheValid = false;
                m_cacheOldPhysical = 0;
                m_cacheBothPopulated = false;
                m_cacheHasValidB = false;
            }
            if (!m_cacheValid || settings.rebuildCache || settings.rotateLight)
            {
                m_pendingCacheRebuild = true;
                m_pendingCacheWriteSlot = 0;
                m_cacheLightOld = m_effectiveLightDirection;
                m_cacheLightNew = m_effectiveLightDirection;
                m_lastCacheRebuildPhysicalSlot = 0;
                m_normalLUTBakedSlots[0] = false;
            }
        }
        else
        {
            const float thresholdRad =
                std::max(settings.lightCacheAngleThresholdDeg, 0.1f) *
                3.14159265358979323846f / 180.0f;
            const XMVECTOR newL = XMLoadFloat3(&m_cacheLightNew);
            const XMVECTOR oldL = XMLoadFloat3(&m_cacheLightOld);
            const float dotOld = std::clamp(XMVectorGetX(XMVector3Dot(curL, oldL)), -1.0f, 1.0f);
            const float dotOldNew = std::clamp(XMVectorGetX(XMVector3Dot(oldL, newL)), -1.0f, 1.0f);
            const float angleFromOld = std::acos(dotOld);
            const float angleOldNew  = std::acos(dotOldNew);
            const bool crossedWindow = angleOldNew > 1e-4f
                ? (angleFromOld > angleOldNew)
                : (angleFromOld > thresholdRad);

            if (!amortized)
                ResetAmortizedCacheState();

            if (subNJustToggled || !m_cacheValid || settings.rebuildCache)
            {
                m_slotDisplayA = 0;
                m_slotDisplayB = 1;
                m_slotBuild = 2;
                m_pendingCacheRebuild = true;
                m_pendingCacheWriteSlot = 0;
                m_cacheLightOld = m_effectiveLightDirection;
                m_cacheLightNew = RotateLightByThreshold(m_effectiveLightDirection,
                    thresholdRad, settings);
                m_cachePredictedLightDir = m_effectiveLightDirection;
                m_pendingPredictiveBuild = false;
                m_pendingSubNSeedDualBuild = true;
                m_pendingFirstBWrite = true;
                m_cacheOldPhysical = 0;
                m_cacheBothPopulated = true;
                m_cacheHasValidB = true;
                m_lastCacheRebuildPhysicalSlot = 0;
                m_normalLUTBakedSlots[0] = false;
                m_normalLUTBakedSlots[1] = false;
                m_forceBakeLUTABBoth = true;
                m_buildActive = false;
                m_buildComplete = false;
                m_buildColumnCursor = 0;
            }
            else if (crossedWindow)
            {
                const uint32_t outgoingPhysical = m_cacheOldPhysical;
                const uint32_t previousNewPhysical =
                    (static_cast<uint32_t>(m_slotDisplayA) == outgoingPhysical)
                        ? static_cast<uint32_t>(m_slotDisplayB)
                        : static_cast<uint32_t>(m_slotDisplayA);

                if (amortized && m_buildActive && m_buildComplete)
                {
                    const uint32_t newPhysical = static_cast<uint32_t>(m_slotBuild);
                    if (static_cast<uint32_t>(m_slotDisplayA) == outgoingPhysical)
                        m_slotDisplayA = static_cast<int>(newPhysical);
                    else
                        m_slotDisplayB = static_cast<int>(newPhysical);
                    m_slotBuild = static_cast<int>(outgoingPhysical);

                    m_cacheLightOld = m_cacheLightNew;
                    m_cacheLightNew = m_buildTargetDir;
                    m_cacheOldPhysical = previousNewPhysical;
                    m_cacheBothPopulated = true;
                    m_cacheHasValidB = true;
                    m_pendingCacheSwap = true;
                    m_lastCacheRebuildPhysicalSlot = newPhysical;
                    m_normalLUTBakedSlots[newPhysical] = false;
                    m_buildActive = false;
                    m_buildComplete = false;
                    m_buildColumnCursor = 0;
                }
                else
                {
                    const XMFLOAT3 buildTarget = RotateLightByThreshold(
                        m_effectiveLightDirection, thresholdRad, settings);
                    m_pendingCacheRebuild = true;
                    m_pendingCacheWriteSlot = outgoingPhysical;
                    m_pendingPredictiveBuild = settings.rotateLight;
                    m_cachePredictedLightDir = buildTarget;
                    m_cacheLightOld = m_cacheLightNew;
                    m_cacheLightNew = buildTarget;
                    m_cacheOldPhysical = previousNewPhysical;
                    m_cacheBothPopulated = true;
                    m_cacheHasValidB = true;
                    m_lastCacheRebuildPhysicalSlot = outgoingPhysical;
                    m_normalLUTBakedSlots[outgoingPhysical] = false;
                    m_buildActive = false;
                    m_buildComplete = false;
                    m_buildColumnCursor = 0;
                }
            }

            if (amortized && !m_pendingCacheRebuild && !m_pendingSubNSeedDualBuild)
            {
                const XMFLOAT3 progressiveTarget = RotateLightByThreshold(
                    m_cacheLightNew, thresholdRad, settings);
                bool resetBuild = !m_buildActive;
                if (m_buildActive)
                {
                    const XMVECTOR oldTarget = XMLoadFloat3(&m_buildTargetDir);
                    const XMVECTOR newTarget = XMLoadFloat3(&progressiveTarget);
                    const float targetDot = std::clamp(
                        XMVectorGetX(XMVector3Dot(oldTarget, newTarget)), -1.0f, 1.0f);
                    resetBuild = std::acos(targetDot) > (2.0f * 3.14159265358979323846f / 180.0f);
                }
                if (resetBuild)
                {
                    m_buildTargetDir = progressiveTarget;
                    m_buildColumnCursor = 0;
                    m_buildActive = true;
                    m_buildComplete = false;
                }

                if (!m_buildComplete)
                {
                    const float radiansPerFrame = (settings.rotateLight && dt > 0.0f)
                        ? std::fabs(settings.lightRotationSpeed) * dt
                        : 0.0f;
                    const float framesBetween = radiansPerFrame > 1e-6f
                        ? std::max(thresholdRad / radiansPerFrame, 1.0f)
                        : 1.0f;
                    // C must be complete BEFORE the crossedWindow frame arrives.
                    // We only advance on non-rebuild frames, so given `framesBetween`
                    // between rebuilds, we get at most (framesBetween - 1) build
                    // opportunities. Subtract a small headroom (1 frame) so C is
                    // fully baked with margin, otherwise crossedWindow falls back to
                    // the sync path and the spike we're trying to hide reappears.
                    const float availableFrames =
                        std::max(std::floor(framesBetween) - 1.0f, 1.0f);
                    m_buildColumnsPerFrame = std::max(1u,
                        static_cast<uint32_t>(std::ceil(
                            static_cast<float>(totalColumns) / availableFrames)));
                    m_buildColumnsPerFrame = std::min(m_buildColumnsPerFrame, fullRangeEnd);

                    const uint32_t startColumn = std::min(
                        static_cast<uint32_t>(std::max(m_buildColumnCursor, 0)), fullRangeEnd);
                    const uint32_t endColumn = std::min(startColumn + m_buildColumnsPerFrame, fullRangeEnd);
                    if (endColumn > startColumn)
                    {
                        m_pendingProgressiveBuild = true;
                        m_pendingCacheWriteSlot = static_cast<uint32_t>(m_slotBuild);
                        m_cachePredictedLightDir = m_buildTargetDir;
                        m_pendingPredictiveBuild = true;
                        m_pendingCacheBuildRangeStart = startColumn;
                        m_pendingCacheBuildRangeEnd = endColumn;
                    }
                    else
                    {
                        m_buildComplete = true;
                    }
                }
            }

            if (m_cacheBothPopulated)
            {
                const XMVECTOR oldL2 = XMLoadFloat3(&m_cacheLightOld);
                const XMVECTOR newL2 = XMLoadFloat3(&m_cacheLightNew);
                const float dotOldCur =
                    std::clamp(XMVectorGetX(XMVector3Dot(oldL2, curL)), -1.0f, 1.0f);
                const float dotOldNew2 =
                    std::clamp(XMVectorGetX(XMVector3Dot(oldL2, newL2)), -1.0f, 1.0f);
                const float angFromOld = std::acos(dotOldCur);
                const float angOldNew2 = std::acos(dotOldNew2);
                const float t = angOldNew2 > 1e-4f
                    ? std::clamp(angFromOld / angOldNew2, 0.0f, 1.0f)
                    : 0.0f;
                m_cacheBlend = (static_cast<uint32_t>(m_slotDisplayA) == m_cacheOldPhysical)
                    ? t : (1.0f - t);
                m_cacheLogicalT = t;
            }
            else
            {
                m_cacheBlend = 0.0f;
                m_cacheLogicalT = 0.0f;
            }
        }
        constants.cacheParameters.w = m_cacheBlend;
        constants.cacheBuildRange = { 0.0f, static_cast<float>(fullRangeEnd), 0.0f, 0.0f };

        if (subN && m_cacheBothPopulated)
        {
            const DirectX::XMFLOAT3& dirBakedInA =
                (static_cast<uint32_t>(m_slotDisplayA) == m_cacheOldPhysical)
                    ? m_cacheLightOld : m_cacheLightNew;
            const DirectX::XMFLOAT3& dirBakedInB =
                (static_cast<uint32_t>(m_slotDisplayB) == m_cacheOldPhysical)
                    ? m_cacheLightOld : m_cacheLightNew;
            FillLightConstants(constants, dirBakedInA);
            FillLightCacheAxesB(constants, dirBakedInB);

            const XMVECTOR oldV = XMLoadFloat3(&m_cacheLightOld);
            const XMVECTOR newV = XMLoadFloat3(&m_cacheLightNew);
            const XMVECTOR interp = XMVector3Normalize(XMVectorLerp(oldV, newV, m_cacheLogicalT));
            DirectX::XMFLOAT3 interpDir;
            XMStoreFloat3(&interpDir, interp);
            constants.lightDirectionGrid = { interpDir.x, interpDir.y, interpDir.z,
                static_cast<float>(m_gridResolution) };
        }
    }
    // Scene constants → scene CBV slot (offset 0 within this frame's 3-slot triplet).
    const uint32_t sceneOffset = m_frameIndex * ConstantStride * 3;
    const uint32_t buildOffset = sceneOffset + ConstantStride;
    const uint32_t seedBBuildOffset = sceneOffset + 2u * ConstantStride;
    std::memcpy(m_constantData + sceneOffset, &constants, sizeof(constants));

    // Build constants: identical to scene EXCEPT the light frame is set to the
    // predictive target dir. When we're not rebuilding, we still write valid
    // constants at the build slot so any bound CBV is defined; when we're
    // rebuilding but not predictive (manual light change), buildTarget == current
    // → constants are effectively identical to scene. Predictive rotating light
    // is where this actually diverges from scene constants.
    ShaderConstants buildConstants = constants;
    FillLightConstants(buildConstants, m_cachePredictedLightDir);
    buildConstants.cacheBuildRange = {
        static_cast<float>(m_pendingCacheBuildRangeStart),
        static_cast<float>(m_pendingCacheBuildRangeEnd),
        0.0f, 0.0f
    };
    std::memcpy(m_constantData + buildOffset, &buildConstants, sizeof(buildConstants));

    // Seed-B build constants: used only on the sub-Nyquist toggle-on frame to
    // populate physical B in a second RecordCacheBuild pass with a look-ahead
    // direction (m_cacheLightNew). Always write valid data at this slot so any
    // bound CBV is defined even on non-seed frames.
    ShaderConstants seedBBuildConstants = constants;
    FillLightConstants(seedBBuildConstants, m_cacheLightNew);
    seedBBuildConstants.cacheBuildRange = { 0.0f,
        static_cast<float>(std::max(m_gridResolution * m_gridResolution, 1u)), 0.0f, 0.0f };
    std::memcpy(m_constantData + seedBBuildOffset, &seedBBuildConstants,
        sizeof(seedBBuildConstants));
}

void Renderer::BeginFrame()
{
    FrameContext& frame = m_frames[m_frameIndex];
    if (frame.fenceValue != 0 && m_fence->GetCompletedValue() < frame.fenceValue)
    {
        m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    ReadCompletedTimings(m_frameIndex);
    frame.allocator->Reset();
    m_commandList->Reset(frame.allocator.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetComputeRootSignature(m_rootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(0, GpuHandle(OutputUav));
    m_commandList->SetComputeRootDescriptorTable(1, GpuHandle(DensitySrv));
    m_commandList->SetComputeRootConstantBufferView(2,
        m_constantBuffer->GetGPUVirtualAddress() + m_frameIndex * ConstantStride * 3);
    const uint32_t zeroConstants[4]{};
    m_commandList->SetComputeRoot32BitConstants(3, 4, zeroConstants, 0);
}

void Renderer::RecordBakeVolumes(const RenderSettings& settings)
{
    PIXScopedEvent(m_commandList.Get(), PIX_COLOR(240, 200, 80), "BakeVolumes");
    Transition(m_remappedVolumeTexture.Get(), m_remappedVolumeState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(240, 210, 100), "BakeRemap");
        m_commandList->SetPipelineState(m_bakeRemappedPso.Get());
        const uint32_t gx = (m_volumeData.width + 3) / 4;
        const uint32_t gy = (m_volumeData.height + 3) / 4;
        const uint32_t gz = (m_volumeData.depth + 3) / 4;
        m_commandList->Dispatch(gx, gy, gz);
    }
    UavBarrier(m_remappedVolumeTexture.Get());
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(240, 220, 120), "BakeRemapMip3");
        m_commandList->SetPipelineState(m_bakeOccupancyPso.Get());
        const uint32_t mip3Res = std::max<uint32_t>(m_volumeData.depth >> 3, 1);
        const uint32_t groups = (mip3Res + 3) / 4;
        m_commandList->Dispatch(groups, groups, groups);
    }
    UavBarrier(m_remappedVolumeTexture.Get());
    Transition(m_remappedVolumeTexture.Get(), m_remappedVolumeState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Distance field: init at DF texture A, then ping-pong propagate 16 times.
    // Result ends in A when the number of propagate passes is even.
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(100, 180, 240), "BakeDistanceField");
        Transition(m_distanceFieldTextureA.Get(), m_distanceFieldAState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(m_distanceFieldTextureB.Get(), m_distanceFieldBState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->SetPipelineState(m_bakeDistanceFieldInitPso.Get());
        m_commandList->Dispatch(32, 32, 32); // 128/4
        UavBarrier(m_distanceFieldTextureA.Get());
        m_commandList->SetPipelineState(m_bakeDistanceFieldPropagatePso.Get());
        const uint32_t propagationPasses = 16;
        for (uint32_t pass = 0; pass < propagationPasses; ++pass)
        {
            const uint32_t direction = pass & 1u; // 0: A->B, 1: B->A
            const uint32_t dispatchParams[4] = { 0u, 0u, 0u, direction };
            m_commandList->SetComputeRoot32BitConstants(3, 4, dispatchParams, 0);
            m_commandList->Dispatch(32, 32, 32);
            UavBarrier(m_distanceFieldTextureA.Get());
            UavBarrier(m_distanceFieldTextureB.Get());
        }
        Transition(m_distanceFieldTextureA.Get(), m_distanceFieldAState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(m_distanceFieldTextureB.Get(), m_distanceFieldBState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    m_bakedDensityThreshold = settings.densityThreshold;
    m_bakedDensityGamma = settings.densityGamma;
    m_bakedDensityScale = settings.densityScale;
    m_bakeVolumesPending = false;
    m_normalLUTBakedSlots = {};
    m_normalLUTBaked = false;
    m_normalLUTBBaked = false;
}

void Renderer::RecordCacheBuild(const RenderSettings& settings, uint32_t writeSlot)
{
    PIXScopedEvent(m_commandList.Get(), PIX_COLOR(80, 180, 220), "CacheBuild");
    const uint32_t queryBase = m_frameIndex * TimestampCount;
    // Point the u3 (CachedDepthWrite) descriptor at the physical buffer we want to
    // populate this rebuild. Rewrite is CPU-side; safe because BeginFrame WaitForGpu
    // guarantees no in-flight dispatch is reading this heap slot.
    ID3D12Resource* writeTarget = PhysicalCache(writeSlot);
    D3D12_RESOURCE_STATES& writeTargetState = PhysicalCacheState(writeSlot);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC coarseUav{};
        coarseUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        coarseUav.Format = DXGI_FORMAT_R16_FLOAT;
        coarseUav.Texture3D.WSize = static_cast<UINT>(m_gridResolution);
        m_device->CreateUnorderedAccessView(writeTarget, nullptr, &coarseUav,
            CpuHandle(TransmittanceUav));
    }
    Transition(m_densityBuffer.Get(), m_densityState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(m_linksBuffer.Get(), m_linksState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(writeTarget, writeTargetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(m_statsBuffer.Get(), m_statsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(120, 120, 120), "ClearCacheUAVs");
        const UINT clearValues[4]{};
        m_commandList->ClearUnorderedAccessViewUint(
            GpuHandle(LinksUav), CpuHandle(LinksUav), m_linksBuffer.Get(), clearValues, 0, nullptr);
        m_commandList->ClearUnorderedAccessViewUint(
            GpuHandle(StatsUav), CpuHandle(StatsUav), m_statsBuffer.Get(), clearValues, 0, nullptr);
        UavBarrier(m_linksBuffer.Get());
        UavBarrier(m_statsBuffer.Get());
    }

    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 2);
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(60, 200, 120), "ResolveColumns");
        m_commandList->SetPipelineState(m_resolvePso.Get());
        const uint32_t columnCount = m_gridResolution * m_gridResolution;
        const uint32_t startColumn = std::min(m_pendingCacheBuildRangeStart, columnCount);
        const uint32_t endColumn = std::min(
            m_pendingCacheBuildRangeEnd == 0 ? columnCount : m_pendingCacheBuildRangeEnd,
            columnCount);
        const uint32_t dispatchColumns = endColumn > startColumn
            ? endColumn - startColumn
            : columnCount;
        const uint32_t wavesPerGroup = 256u / m_waveLaneCount;
        m_commandList->Dispatch((dispatchColumns + wavesPerGroup - 1) / wavesPerGroup, 1, 1);
    }
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 3);
    UavBarrier(m_densityBuffer.Get());
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 4);
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 5);
    UavBarrier(m_linksBuffer.Get());
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 6);
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 7);
    Transition(m_densityBuffer.Get(), m_densityState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(m_linksBuffer.Get(), m_linksState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(writeTarget, writeTargetState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Transition(m_statsBuffer.Get(), m_statsState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->CopyBufferRegion(m_statsReadbacks[m_frameIndex].Get(), 0,
        m_statsBuffer.Get(), 0, sizeof(uint32_t) * 2);
    Transition(m_statsBuffer.Get(), m_statsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cacheBuiltFrames[m_frameIndex] = true;
    m_cacheValid = true;
}

void Renderer::RecordScene(const RenderSettings& settings, bool cacheRebuiltThisFrame)
{
    const bool naive = settings.mode == RaymarchMode::Naive;
    PIXScopedEvent(m_commandList.Get(),
        naive ? PIX_COLOR(230, 140, 60) : PIX_COLOR(140, 200, 90),
        naive ? "Raymarch (Naive)" : "Raymarch (Cached)");
    const uint32_t queryBase = m_frameIndex * TimestampCount;
    const uint32_t tileCountX = (m_width + 7) / 8;
    const uint32_t tileCountY = (m_height + 7) / 8;

    // ---- Temporal reuse -------------------------------------------------------
    // Compute a hash of every setting that affects the rendered image, INCLUDING
    // camera pose. For volumetric rendering there is no safe per-pixel reprojection:
    // any camera translation or rotation changes every ray's origin/direction and
    // therefore the slice of the volume every pixel integrates. Only frames where
    // the camera is bit-exactly static can be reused without ghosting/smearing.
    // Under this policy the reprojection pass is effectively "copy the whole image
    // when the camera is idle" — still a big win for idle-camera iteration but
    // never produces incorrect content.
    uint64_t settingsHash = 0;
    {
        auto mix = [&](const void* p, size_t n)
        {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i)
                settingsHash = (settingsHash ^ b[i]) * 1099511628211ull;
        };
        settingsHash = 14695981039346656037ull;
        RenderSettings hashCopy = settings;
        hashCopy.rebuildCache = false;      // transient
        hashCopy.debugTemporalReuse = false; // pure overlay; doesn't affect main image
        mix(&hashCopy, sizeof(hashCopy));
        uint32_t dims[3] = { m_width, m_height, m_gridResolution };
        mix(dims, sizeof(dims));
    }
    const bool globalInvalidate = !m_temporalHistoryValid
        || cacheRebuiltThisFrame
        || settingsHash != m_lastSettingsHash
        || !settings.useTemporalReuse
        || settings.debugVoxels
        || settings.debugTiles
        || settings.debugDistanceField
        || settings.showDensityHeatmap // heatmap must recount every frame
        // Time-driven motions change the effective camera / light every frame even
        // when the hashed settings don't move, so reuse would silently stall them.
        || settings.autoOrbit
        || settings.rotateLight;

    // Always clear the reuse-valid mask; TemporalReprojectCS only writes 1's where it
    // successfully sampled history. Cleared unconditionally so the main render's
    // early-out works even when reprojection is skipped.
    Transition(m_reuseValidTexture.Get(), m_reuseValidState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {
        const UINT clearVals[4] = { 0, 0, 0, 0 };
        m_commandList->ClearUnorderedAccessViewUint(
            GpuHandle(ReuseValidUav), CpuHandle(ReuseValidUav),
            m_reuseValidTexture.Get(), clearVals, 0, nullptr);
        UavBarrier(m_reuseValidTexture.Get());
    }

    if (settings.useTemporalReuse && !globalInvalidate)
    {
        // Camera and every setting matched last frame → the correct image is exactly
        // last frame's Output. Blit history back into Output and mark every pixel as
        // reused so the main render's early-out skips every trace. This is the only
        // safe reuse policy for a volumetric render: no per-pixel reprojection can
        // approximate integrated-along-ray content without ghosting.
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(120, 200, 220), "TemporalReuseCopy");
        Transition(m_historyColorTexture.Get(), m_historyColorState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(m_outputTexture.Get(), m_outputState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyResource(m_outputTexture.Get(), m_historyColorTexture.Get());
        Transition(m_outputTexture.Get(), m_outputState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Fill the reuse mask with 1 so every pixel of the main render early-outs.
        const UINT ones[4] = { 1, 1, 1, 1 };
        m_commandList->ClearUnorderedAccessViewUint(
            GpuHandle(ReuseValidUav), CpuHandle(ReuseValidUav),
            m_reuseValidTexture.Get(), ones, 0, nullptr);
        UavBarrier(m_reuseValidTexture.Get());
    }

    if (settings.enableTileClassify)
    {
        Transition(m_tileClassificationBuffer.Get(), m_tileClassificationState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        {
            PIXScopedEvent(m_commandList.Get(), PIX_COLOR(200, 180, 120), "ClassifyTiles");
            m_commandList->SetPipelineState(m_classifyTilesPso.Get());
            m_commandList->Dispatch((tileCountX + 7) / 8, (tileCountY + 7) / 8, 1);
        }
        UavBarrier(m_tileClassificationBuffer.Get());
        Transition(m_tileClassificationBuffer.Get(), m_tileClassificationState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else if (m_tileClassificationState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Transition(m_tileClassificationBuffer.Get(), m_tileClassificationState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // Two-LUT sub-Nyquist scheme: descriptor slots A/B now point at the two
    // displayed physical caches, which may be any two of A/B/C after an amortized
    // handoff. Bake only the displayed LUT(s) whose physical cache was just replaced.
    const bool useNormalLUT = !naive && settings.useNormalLUT;
    const uint32_t displayASlot = static_cast<uint32_t>(m_slotDisplayA);
    const uint32_t displayBSlot = m_cacheBothPopulated
        ? static_cast<uint32_t>(m_slotDisplayB)
        : displayASlot;
    const bool wroteDisplayedA = cacheRebuiltThisFrame &&
        (m_lastCacheRebuildPhysicalSlot == displayASlot);
    const bool wroteDisplayedB = cacheRebuiltThisFrame && m_cacheHasValidB &&
        (m_lastCacheRebuildPhysicalSlot == displayBSlot);
    const bool bakeNormalLUTA = useNormalLUT &&
        (wroteDisplayedA || !m_normalLUTBakedSlots[displayASlot] || m_forceBakeLUTABBoth);
    const bool bakeNormalLUTB = useNormalLUT && m_cacheHasValidB &&
        (wroteDisplayedB || !m_normalLUTBakedSlots[displayBSlot] || m_forceBakeLUTABBoth);

    ID3D12Resource* lutA = PhysicalNormalLUT(displayASlot);
    ID3D12Resource* lutB = PhysicalNormalLUT(displayBSlot);
    D3D12_RESOURCE_STATES& lutAState = PhysicalNormalLUTState(displayASlot);
    D3D12_RESOURCE_STATES& lutBState = PhysicalNormalLUTState(displayBSlot);
    if (bakeNormalLUTA)
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(180, 140, 220), "BakeNormalLUT");
        Transition(lutA, lutAState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->SetPipelineState(m_bakeNormalLUTPso.Get());
        const uint32_t nlutGroups = (m_normalLUTResolution + 3) / 4;
        m_commandList->Dispatch(nlutGroups, nlutGroups, nlutGroups);
        UavBarrier(lutA);
        Transition(lutA, lutAState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_normalLUTBakedSlots[displayASlot] = true;
        m_normalLUTBaked = (displayASlot == 0) ? true : m_normalLUTBaked;
    }
    else if (lutAState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Transition(lutA, lutAState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (bakeNormalLUTB)
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(180, 140, 220), "BakeNormalLUTB");
        Transition(lutB, lutBState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->SetPipelineState(m_bakeNormalLUTBPso.Get());
        const uint32_t nlutGroups = (m_normalLUTResolution + 3) / 4;
        m_commandList->Dispatch(nlutGroups, nlutGroups, nlutGroups);
        UavBarrier(lutB);
        Transition(lutB, lutBState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_normalLUTBakedSlots[displayBSlot] = true;
        m_normalLUTBBaked = (displayBSlot == 1) ? true : m_normalLUTBBaked;
        m_normalLUTBValid = true;
    }
    else if (lutBState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Transition(lutB, lutBState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // One-shot flag consumed by this frame's bake decisions.
    m_forceBakeLUTABBoth = false;
    const bool useVariant = settings.useSplitTrace && settings.useTileVariantDispatch
        && settings.enableTileClassify;
    const uint32_t dispatchParams[4] = {
        settings.enableTileClassify ? 1u : 0u,
        useVariant ? 1u : 0u,
        useNormalLUT ? 1u : 0u,
        settings.useDistanceField ? 1u : 0u
    };
    m_commandList->SetComputeRoot32BitConstants(3, 4, dispatchParams, 0);
    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 8);
    // Heatmap uses ENABLE_DENSITY_COUNTING baked into the non-split render entry points,
    // so when the toggle is on we skip the split path and take the plain PSO branch.
    if (!naive && settings.useSplitTrace && !settings.showDensityHeatmap)
    {
        if (useVariant)
        {
            PIXScopedEvent(m_commandList.Get(), PIX_COLOR(220, 220, 100), "ClearTileDispatch");
            m_commandList->SetPipelineState(m_clearTileDispatchPso.Get());
            m_commandList->Dispatch(1, 1, 1);
            UavBarrier(m_indirectDispatchArgs.Get());
        }
        {
            PIXScopedEvent(m_commandList.Get(), PIX_COLOR(160, 220, 160), "FindHitInterval");
            m_commandList->SetPipelineState(m_findHitIntervalPso.Get());
            m_commandList->Dispatch(tileCountX, tileCountY, 1);
        }
        UavBarrier(m_hitIntervalBuffer.Get());
        if (useVariant)
        {
            // FindHitIntervalCS also atomic-appended non-sky tiles to m_tileList and
            // bumped the counter at m_indirectDispatchArgs[0].
            UavBarrier(m_indirectDispatchArgs.Get());
            UavBarrier(m_tileList.Get());
            Transition(m_indirectDispatchArgs.Get(), m_indirectDispatchArgsState,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            {
                PIXScopedEvent(m_commandList.Get(), PIX_COLOR(180, 140, 220), "SplitSky");
                m_commandList->SetPipelineState(m_splitCachedSkyPso.Get());
                m_commandList->Dispatch(tileCountX, tileCountY, 1);
            }
            {
                PIXScopedEvent(m_commandList.Get(), PIX_COLOR(240, 180, 100), "SplitIndirect");
                m_commandList->SetPipelineState(m_splitCachedIndirectPso.Get());
                m_commandList->ExecuteIndirect(m_dispatchSignature.Get(), 1,
                    m_indirectDispatchArgs.Get(), 0, nullptr, 0);
            }
            Transition(m_indirectDispatchArgs.Get(), m_indirectDispatchArgsState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            PIXScopedEvent(m_commandList.Get(), PIX_COLOR(200, 220, 160), "SplitCachedRender");
            m_commandList->SetPipelineState(m_splitCachedPso.Get());
            m_commandList->Dispatch(tileCountX, tileCountY, 1);
        }
    }
    else
    {
        // Non-naive path selects between (LUT × DDA × Surface-Normals) cached variants.
        // See PersistentCachedRender for the register-pressure rationale. LUT-on
        // variants strip the finite-diff normal path; NoSN variants strip the entire
        // surface-normal sampler subtree so DXC doesn't reserve registers for
        // CacheGradientDotLight when the user has the toggle off. The debug-voxel-
        // wireframe toggle takes a dedicated PSO so the fast variants never spend
        // registers on VoxelEdgeMask / CameraTargetMode.w gating.
        ID3D12PipelineState* cached;
        if (settings.debugVoxels)
            cached = m_cachedDebugPso.Get();
        else if (settings.showDensityHeatmap)
        {
            // Mirror the non-count PSO selection (LUT × DF × SN × ReducedTap) so the
            // heatmap overlay accurately reflects the cost profile of every toggle.
            // debugVoxels skipped above; all other axes honoured.
            if (settings.enableSurfaceNormals)
            {
                // Match the shipping non-count selection: reduced-tap only meaningful
                // when LUT is off (LUT path is 1 tap, unaffected by USE_REDUCED_TAP_NORMAL).
                const bool useReduced = settings.useReducedTapNormals && !useNormalLUT;
                if (settings.useDistanceField)
                {
                    if (useNormalLUT)     cached = m_cachedDfLutCountPso.Get();
                    else if (useReduced)  cached = m_cachedDfReducedTapCountPso.Get();
                    else                  cached = m_cachedDfCountPso.Get();
                }
                else
                {
                    if (useNormalLUT)     cached = m_cachedLutCountPso.Get();
                    else if (useReduced)  cached = m_cachedReducedTapCountPso.Get();
                    else                  cached = m_cachedCountPso.Get();
                }
            }
            else
            {
                if (settings.useDistanceField)
                    cached = useNormalLUT ? m_cachedDfLutNoSNCountPso.Get() : m_cachedDfNoSNCountPso.Get();
                else
                    cached = useNormalLUT ? m_cachedLutNoSNCountPso.Get()   : m_cachedNoSNCountPso.Get();
            }
        }
        else if (settings.enableSurfaceNormals)
        {
            // Reduced-tap normals only meaningful when the LUT is off (LUT path is
            // already 1 tap, unaffected by USE_REDUCED_TAP_NORMAL).
            const bool useReduced = settings.useReducedTapNormals && !useNormalLUT;
            // Sub-Nyquist-off specialisation: when the tickbox is off, dispatch a
            // PSO compiled without USE_SUB_NYQUIST so DXC has already dead-stripped
            // the B-cache taps / LUT_B sample / gradB blend from the shader.
            // Density-heatmap Count PSOs and Reduced-tap PSOs still use the SubN
            // variant (only 4 primary combos have a NoSubN specialisation today).
            const bool noSubN = !settings.useLightCacheSubNyquist
                && !settings.showDensityHeatmap && !useReduced;
            if (settings.useDistanceField)
            {
                if (noSubN && useNormalLUT)     cached = m_cachedDfLutNoSubNPso.Get();
                else if (noSubN)                cached = m_cachedDfNoSubNPso.Get();
                else if (useNormalLUT)          cached = m_cachedDfLutPso.Get();
                else if (useReduced)            cached = m_cachedDfReducedTapPso.Get();
                else                            cached = m_cachedDfPso.Get();
            }
            else
            {
                if (noSubN && useNormalLUT)     cached = m_cachedLutNoSubNPso.Get();
                else if (noSubN)                cached = m_cachedNoSubNPso.Get();
                else if (useNormalLUT)          cached = m_cachedLutPso.Get();
                else if (useReduced)            cached = m_cachedReducedTapPso.Get();
                else                            cached = m_cachedPso.Get();
            }
        }
        else
        {
            if (settings.useDistanceField)
                cached = useNormalLUT ? m_cachedDfLutNoSNPso.Get() : m_cachedDfNoSNPso.Get();
            else
                cached = useNormalLUT ? m_cachedLutNoSNPso.Get()   : m_cachedNoSNPso.Get();
        }
        m_commandList->SetPipelineState(naive
            ? (settings.showDensityHeatmap ? m_naiveCountPso.Get() : m_naivePso.Get())
            : cached);
        m_commandList->Dispatch(tileCountX, tileCountY, 1);
    }
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 9);
    UavBarrier(m_outputTexture.Get());

    // Snapshot the CLEAN raymarched Output into HistoryColor before any debug
    // overlay writes to it. If we snapshotted at end-of-frame, debug tints
    // (reuse heatmap, tile overlay, DDA overlay, density heatmap) would leak into
    // history and next-frame reprojection would sample those tints — turning the
    // screen the debug colour forever until a global invalidate.
    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(m_historyColorTexture.Get(), m_historyColorState, D3D12_RESOURCE_STATE_COPY_DEST);
    m_commandList->CopyResource(m_historyColorTexture.Get(), m_outputTexture.Get());
    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Heatmap display pass: takes SampleCountWrite (u14) populated by the *_CountCS
    // dispatch and blends a false-colour count overlay over Output. Uses
    // DispatchParameters.z to carry heatmapMaxSamples (temporarily overrides the
    // useNormalLUT flag that .z normally carries — safe because Output is only read
    // for existing colour, and the shader ignores every other DispatchParameters channel).
    if (settings.showDensityHeatmap)
    {
        UavBarrier(m_sampleCountTexture.Get());
        const uint32_t heatParams[4] = { 0u,
            static_cast<uint32_t>(std::clamp(settings.heatmapCounterMode, 0, 3)),
            static_cast<uint32_t>(std::max(1, settings.heatmapMaxSamples)), 0u };
        m_commandList->SetComputeRoot32BitConstants(3, 4, heatParams, 0);
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(255, 140, 60), "HeatmapDisplay");
        m_commandList->SetPipelineState(m_heatmapDisplayPso.Get());
        m_commandList->Dispatch(tileCountX, tileCountY, 1);
        UavBarrier(m_outputTexture.Get());
    }
    if (settings.enableTileClassify && settings.debugTiles)
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(220, 160, 220), "DebugTileOverlay");
        m_commandList->SetPipelineState(m_debugTileOverlayPso.Get());
        m_commandList->Dispatch(tileCountX, tileCountY, 1);
        UavBarrier(m_outputTexture.Get());
    }
    if (settings.debugDistanceField)
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(140, 200, 240), "DebugDistanceField");
        m_commandList->SetPipelineState(m_debugDistanceFieldPso.Get());
        m_commandList->Dispatch(tileCountX, tileCountY, 1);
        UavBarrier(m_outputTexture.Get());
    }

    // Temporal-reuse debug overlay: green = pixel filled from history, red = pixel
    // marched this frame. Runs whenever the toggle is on (even on globally-invalidated
    // frames) so the user can see the moments where reuse fell through — the mask is
    // all-zero on invalidated frames so those show up entirely red.
    if (settings.useTemporalReuse && settings.debugTemporalReuse)
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(120, 220, 120), "TemporalReuseDebug");
        m_commandList->SetPipelineState(m_temporalReuseDebugPso.Get());
        m_commandList->Dispatch(tileCountX, tileCountY, 1);
        UavBarrier(m_outputTexture.Get());
    }

    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // History was already snapshotted before the overlays. Update tracking state.
    m_lastSettingsHash = settingsHash;
    m_temporalHistoryValid = true;
}

void Renderer::Render(RenderSettings& settings, ImDrawData* drawData, float timeSeconds)
{
    const auto cpuActiveStart = std::chrono::steady_clock::now();
    PIXBeginEvent(m_queue.Get(), PIX_COLOR(200, 200, 240), "Frame %llu", m_fenceValue + 1);
    PIXScopedEvent(m_commandList.Get(), PIX_COLOR(200, 200, 240), "Frame");
    const uint32_t queryBase = m_frameIndex * TimestampCount;
    if (static_cast<uint32_t>(settings.gridResolution) != m_gridResolution)
    {
        CreateVolumeResources(static_cast<uint32_t>(settings.gridResolution));
        settings.rebuildCache = true;
    }
    if (static_cast<uint32_t>(settings.normalLUTResolution) != m_normalLUTResolution)
    {
        CreateNormalLUT(static_cast<uint32_t>(settings.normalLUTResolution));
    }
    m_cacheBuiltFrames[m_frameIndex] = false;
    if (settings.rotateLight && !settings.useLightCacheSubNyquist)
        settings.rebuildCache = true;
    if (settings.densityThreshold != m_bakedDensityThreshold ||
        settings.densityGamma != m_bakedDensityGamma ||
        settings.densityScale != m_bakedDensityScale)
        m_bakeVolumesPending = true;
    PopulateConstants(settings, timeSeconds);
    RebindCacheDescriptors();

    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);
    if (m_bakeVolumesPending)
    {
        RecordBakeVolumes(settings);
        settings.rebuildCache = true;
    }
    bool cacheRebuiltThisFrame = m_pendingCacheSwap;
    if (m_pendingCacheRebuild || settings.rebuildCache)
    {
        const uint32_t writeSlot = m_pendingCacheWriteSlot;
        const bool predictive = m_pendingPredictiveBuild;
        const uint64_t sceneCbv =
            m_constantBuffer->GetGPUVirtualAddress() + m_frameIndex * ConstantStride * 3;
        const uint64_t buildCbv = sceneCbv + ConstantStride;
        const uint64_t seedBBuildCbv = sceneCbv + 2u * ConstantStride;
        bool boundNonSceneCbv = false;
        if (predictive)
        {
            // Point b0 at the build-constants slot so ResolveColumnsCS reads the
            // predictive-target light frame instead of the current-frame one.
            m_commandList->SetComputeRootConstantBufferView(2, buildCbv);
            boundNonSceneCbv = true;
        }
        RecordCacheBuild(settings, writeSlot);
        m_lastCacheRebuildWroteToB = (writeSlot == 1);
        m_lastCacheRebuildPhysicalSlot = writeSlot;
        if (m_pendingSubNSeedDualBuild)
        {
            // Sub-Nyquist seed-dual: after the primary writeToA build (current
            // light dir), immediately fire a SECOND build into physical B using
            // the look-ahead direction stored in m_cacheLightNew. Switch b0 to
            // the seed-B build CBV so ResolveColumnsCS uses that direction's
            // frame axes. This populates both physicals in one frame so the
            // first rendered frame after the toggle already has valid two-LUT
            // interpolation data — no stale/empty B, no visible seam.
            m_commandList->SetComputeRootConstantBufferView(2, seedBBuildCbv);
            m_pendingCacheBuildRangeStart = 0;
            m_pendingCacheBuildRangeEnd = std::max(m_gridResolution * m_gridResolution, 1u);
            RecordCacheBuild(settings, 1);
            m_lastCacheRebuildWroteToB = true;
            m_lastCacheRebuildPhysicalSlot = 1;
            m_pendingSubNSeedDualBuild = false;
            boundNonSceneCbv = true;
        }
        if (boundNonSceneCbv)
        {
            // Restore scene CBV for the rest of the frame's dispatches (bake,
            // RecordScene, temporal, etc.).
            m_commandList->SetComputeRootConstantBufferView(2, sceneCbv);
        }
        settings.rebuildCache = false;
        cacheRebuiltThisFrame = true;
        m_cacheValid = true;
        m_pendingCacheRebuild = false;
        m_pendingFirstBWrite = false;
    }
    if (m_pendingProgressiveBuild)
    {
        const uint64_t sceneCbv =
            m_constantBuffer->GetGPUVirtualAddress() + m_frameIndex * ConstantStride * 3;
        const uint64_t buildCbv = sceneCbv + ConstantStride;
        m_commandList->SetComputeRootConstantBufferView(2, buildCbv);
        RecordCacheBuild(settings, static_cast<uint32_t>(m_slotBuild));
        m_commandList->SetComputeRootConstantBufferView(2, sceneCbv);
        m_buildColumnCursor = static_cast<int>(m_pendingCacheBuildRangeEnd);
        if (m_buildColumnCursor >= static_cast<int>(m_gridResolution * m_gridResolution))
        {
            m_buildColumnCursor = static_cast<int>(m_gridResolution * m_gridResolution);
            m_buildComplete = true;
        }
        m_pendingProgressiveBuild = false;
    }
    RecordScene(settings, cacheRebuiltThisFrame);

    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(180, 120, 220), "Present+ImGui");
        D3D12_RESOURCE_BARRIER toRenderTarget{};
        toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Transition.pResource = m_backBuffers[m_frameIndex].Get();
        toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toRenderTarget);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
        const float clearColor[4] = { 0.012f, 0.015f, 0.022f, 1.0f };
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        {
            PIXScopedEvent(m_commandList.Get(), PIX_COLOR(220, 160, 240), "ImGui");
            ImGui_ImplDX12_RenderDrawData(drawData, m_commandList.Get());
        }

        std::swap(toRenderTarget.Transition.StateBefore, toRenderTarget.Transition.StateAfter);
        m_commandList->ResourceBarrier(1, &toRenderTarget);
    }
    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);
    m_commandList->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase,
        TimestampCount, m_timestampReadbacks[m_frameIndex].Get(), 0);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    const auto cpuActiveEnd = std::chrono::steady_clock::now();
    const float cpuActiveMs = std::chrono::duration<float, std::milli>(
        cpuActiveEnd - cpuActiveStart).count();
    constexpr float smoothing = 0.08f;
    m_stats.cpuActiveMs = m_stats.cpuActiveMs == 0.0f
        ? cpuActiveMs
        : m_stats.cpuActiveMs + smoothing * (cpuActiveMs - m_stats.cpuActiveMs);
    const UINT syncInterval = 0;
    m_swapchain->Present(syncInterval, 0);
    PIXEndEvent(m_queue.Get());

    const uint64_t signalValue = ++m_fenceValue;
    m_queue->Signal(m_fence.Get(), signalValue);
    m_frames[m_frameIndex].fenceValue = signalValue;
    m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
}

void Renderer::ReadCompletedTimings(uint32_t frameIndex)
{
    if (m_frames[frameIndex].fenceValue == 0)
        return;
    uint64_t* timestamps = nullptr;
    D3D12_RANGE range{ 0, TimestampCount * sizeof(uint64_t) };
    if (SUCCEEDED(m_timestampReadbacks[frameIndex]->Map(0, &range, reinterpret_cast<void**>(&timestamps))))
    {
        const double scale = 1000.0 / static_cast<double>(m_timestampFrequency);
        constexpr float smoothing = 0.08f;
        auto smooth = [](float current, float sample, float alpha)
        {
            return current == 0.0f ? sample : current + alpha * (sample - current);
        };
        const float totalSample = static_cast<float>((timestamps[1] - timestamps[0]) * scale);
        const float renderSample = static_cast<float>((timestamps[9] - timestamps[8]) * scale);
        m_stats.gpuRenderSampleMs = renderSample;
        m_stats.gpuTotalSampleMs = totalSample;
        m_stats.gpuTotalMs = smooth(m_stats.gpuTotalMs, totalSample, smoothing);
        m_stats.gpuRenderMs = smooth(m_stats.gpuRenderMs, renderSample, smoothing);
        if (m_cacheBuiltFrames[frameIndex])
        {
            m_stats.gpuVoxelizeMs = static_cast<float>((timestamps[3] - timestamps[2]) * scale);
            m_stats.gpuLinkMs = static_cast<float>((timestamps[5] - timestamps[4]) * scale);
            m_stats.gpuResolveMs = static_cast<float>((timestamps[7] - timestamps[6]) * scale);
            m_stats.lastCacheBuildMs = m_stats.gpuVoxelizeMs + m_stats.gpuLinkMs + m_stats.gpuResolveMs;
        }
        m_timestampReadbacks[frameIndex]->Unmap(0, nullptr);
        // Store raw per-frame samples (not smoothed) so graph min/max reflect the
        // true extremes — including cache-rebuild spikes on the total series.
        std::move(m_stats.gpuHistory.begin() + 1, m_stats.gpuHistory.end(), m_stats.gpuHistory.begin());
        m_stats.gpuHistory.back() = renderSample;
        std::move(m_stats.gpuTotalHistory.begin() + 1, m_stats.gpuTotalHistory.end(), m_stats.gpuTotalHistory.begin());
        m_stats.gpuTotalHistory.back() = totalSample;
    }

    if (m_cacheBuiltFrames[frameIndex])
    {
        uint32_t* occupied = nullptr;
        D3D12_RANGE statsRange{ 0, sizeof(uint32_t) * 2 };
        if (SUCCEEDED(m_statsReadbacks[frameIndex]->Map(0, &statsRange, reinterpret_cast<void**>(&occupied))))
        {
            m_stats.occupiedCells = *occupied;
            m_stats.compactCacheCells = occupied[1];
            m_statsReadbacks[frameIndex]->Unmap(0, nullptr);
        }
    }
}

bool Renderer::CaptureOutput(std::vector<uint8_t>& rgbaPixels)
{
    if (!m_outputTexture || !m_device || !m_queue)
        return false;

    WaitForGpu();

    D3D12_RESOURCE_DESC textureDescription = m_outputTexture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint32_t rowCount = 0;
    uint64_t rowSize = 0;
    uint64_t requiredSize = 0;
    m_device->GetCopyableFootprints(&textureDescription, 0, 1, 0,
        &footprint, &rowCount, &rowSize, &requiredSize);

    auto readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    auto readbackDescription = BufferDescription(requiredSize);
    ComPtr<ID3D12Resource> readback;
    if (Failed(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
        &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        SetError("Failed to create output readback buffer.");
        return false;
    }

    FrameContext& frame = m_frames[m_frameIndex];
    frame.allocator->Reset();
    m_commandList->Reset(frame.allocator.Get(), nullptr);
    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_outputTexture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    uint8_t* mapped = nullptr;
    D3D12_RANGE range{ 0, static_cast<SIZE_T>(requiredSize) };
    if (Failed(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
    {
        SetError("Failed to map output readback buffer.");
        return false;
    }

    const size_t outputRowBytes = static_cast<size_t>(m_width) * 4;
    rgbaPixels.resize(outputRowBytes * m_height);
    for (uint32_t y = 0; y < m_height; ++y)
    {
        std::memcpy(rgbaPixels.data() + static_cast<size_t>(y) * outputRowBytes,
            mapped + static_cast<size_t>(y) * footprint.Footprint.RowPitch, outputRowBytes);
    }
    readback->Unmap(0, nullptr);
    return true;
}

void Renderer::WaitForGpu()
{
    if (!m_queue || !m_fence)
        return;
    const uint64_t value = ++m_fenceValue;
    if (SUCCEEDED(m_queue->Signal(m_fence.Get(), value)) && m_fence->GetCompletedValue() < value)
    {
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::DestroySizeDependentResources()
{
    m_outputTexture.Reset();
    m_hitIntervalBuffer.Reset();
    m_tileList.Reset();
    m_indirectDispatchArgs.Reset();
    m_indirectDispatchArgsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_sampleCountTexture.Reset();
    m_sampleCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_reuseValidTexture.Reset();
    m_reuseValidState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_historyColorTexture.Reset();
    m_historyColorState = D3D12_RESOURCE_STATE_COPY_DEST;
    m_temporalHistoryValid = false;
    for (auto& buffer : m_backBuffers)
        buffer.Reset();
}

void Renderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_initialized || width == 0 || height == 0 || (width == m_width && height == m_height))
        return;
    WaitForGpu();
    DestroySizeDependentResources();
    for (FrameContext& frame : m_frames)
        frame.fenceValue = 0;
    m_width = width;
    m_height = height;
    if (SUCCEEDED(m_swapchain->ResizeBuffers(FrameCount, width, height, BackBufferFormat, 0)))
    {
        m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
        CreateSizeDependentResources();
    }
    else
        SetError("Swapchain resize failed.");
}

bool Renderer::LoadVolume(const std::filesystem::path& path)
{
    if (!m_initialized)
    {
        SetError("LoadVolume called before Initialize.");
        return false;
    }
    // Ensure no in-flight work references the resources we're about to reset. Frame
    // fence values are cleared so the next Signal/Wait pair in Render() starts from
    // a clean slate rather than comparing against stale values.
    WaitForGpu();
    for (FrameContext& frame : m_frames)
        frame.fenceValue = 0;

    // Drop VDB-owned GPU resources. Descriptor heap slots (DenseVolumeSrv, ...) will
    // be overwritten by CreateVdbResources — no need to null them explicitly.
    m_denseVolumeTexture.Reset();
    m_sparsePageTexture.Reset();
    m_sparseBrickBuffer.Reset();
    m_remappedVolumeTexture.Reset();
    m_remappedVolumeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_distanceFieldTextureA.Reset();
    m_distanceFieldTextureB.Reset();
    m_distanceFieldAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_distanceFieldBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Force the normal LUT to re-bake against the new density field. CreateNormalLUT
    // early-outs when the resolution matches, so the resource itself may be reused —
    // but the *contents* are stale until re-baked.
    m_normalLUTBaked = false;
    m_normalLUTBBaked = false;
    m_normalLUTBakedSlots = {};

    // LoadVdbVolume appends to brickPool/pageTable without pre-clearing; use a fresh
    // instance so residual data from the previous volume can't leak into the upload.
    m_volumeData = VolumeData{};

    if (!LoadVdbVolume(path, m_volumeData, m_lastError))
        return false;

    if (!CreateVdbResources())
        return false;

    // Invalidate everything derived from the previous volume so the next Render()
    // frame rebuilds the transmittance cache, coarse depth, and temporal history
    // against the new density field.
    m_cacheValid = false;
    ResetAmortizedCacheState();
    m_temporalHistoryValid = false;
    m_bakeVolumesPending = true;
    return true;
}

void Renderer::Shutdown()
{
    if (m_initialized)
    {
        WaitForGpu();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }
    if (m_constantBuffer && m_constantData)
        m_constantBuffer->Unmap(0, nullptr);
    m_constantData = nullptr;
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
    m_initialized = false;
}
