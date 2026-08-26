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
    auto constantsDescription = BufferDescription(FrameCount * ConstantStride);
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

    m_densityState = m_linksState = m_transmittanceState = m_statsState =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_stats.totalCells = cells;
    m_stats.cacheBytes = cellCount * sizeof(uint16_t) +
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

    // Empty-space distance field: two 128^3 R16_FLOAT textures for ping-pong propagation.
    // R16_FLOAT (vs R8_UNORM) gives ~65k quantisation levels over the [0, horizon] range,
    // eliminating the ~1-stepLength jumps between adjacent texels that were producing
    // faint wave-boundary artifacts in mid-tone cloud regions.
    const uint32_t dfRes = 128;
    if (!createUavTexture3D(dfRes, dfRes, dfRes, DXGI_FORMAT_R16_FLOAT, m_distanceFieldTextureA) ||
        !createUavTexture3D(dfRes, dfRes, dfRes, DXGI_FORMAT_R16_FLOAT, m_distanceFieldTextureB))
    {
        SetError("Failed to create distance-field textures.");
        return false;
    }
    m_distanceFieldAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_distanceFieldBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC dfUav{};
        dfUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        dfUav.Format = DXGI_FORMAT_R16_FLOAT;
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
        dfSrv.Format = DXGI_FORMAT_R16_FLOAT;
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
    m_normalLUTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_normalLUTResolution = clamped;
    m_normalLUTBaked = false;
    m_stats.normalLUTBytes =
        static_cast<uint64_t>(clamped) * clamped * clamped * 4ull; // R8G8B8A8_SNORM

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    uav.Texture3D.MipSlice = 0;
    uav.Texture3D.WSize = clamped;
    m_device->CreateUnorderedAccessView(m_normalLUTTexture.Get(), nullptr, &uav,
        CpuHandle(NormalLUTUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = 1;
    srv.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
    m_device->CreateShaderResourceView(m_normalLUTTexture.Get(), &srv,
        CpuHandle(NormalLUTSrv));
    return true;
}

bool Renderer::CreatePipelines()
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 15;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 10;
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
        { "BakeRemappedVolumeCS", &m_bakeRemappedPso },
        { "BakeOccupancyCS", &m_bakeOccupancyPso },
        { "ClassifyTilesCS", &m_classifyTilesPso },
        { "DebugTileOverlayCS", &m_debugTileOverlayPso },
        { "BakeNormalLUTCS", &m_bakeNormalLUTPso },
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

void Renderer::PopulateConstants(const RenderSettings& settings, float timeSeconds)
{
    float yaw = settings.cameraYaw + (settings.autoOrbit ? timeSeconds * 0.18f : 0.0f);
    float cp = std::cos(settings.cameraPitch);
    const float effectiveDistance = std::max(settings.cameraDistance, 0.001f);
    DirectX::XMFLOAT3 camera{
        std::sin(yaw) * cp * effectiveDistance,
        std::sin(settings.cameraPitch) * effectiveDistance,
        std::cos(yaw) * cp * effectiveDistance
    };

    DirectX::XMVECTOR light = DirectX::XMLoadFloat3(&settings.lightDirection);
    if (settings.rotateLight)
    {
        const float angle = timeSeconds * settings.lightRotationSpeed;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const DirectX::XMFLOAT3 base = settings.lightDirection;
        light = DirectX::XMVectorSet(
            base.x * cosine + base.z * sine,
            base.y,
            -base.x * sine + base.z * cosine,
            0.0f);
    }
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(light)) < 1e-5f)
        light = DirectX::XMVectorSet(0.45f, 0.8f, -0.35f, 0.0f);
    light = DirectX::XMVector3Normalize(light);
    DirectX::XMFLOAT3 normalizedLight;
    DirectX::XMStoreFloat3(&normalizedLight, light);
    m_effectiveLightDirection = normalizedLight;

    ShaderConstants constants{};
    constants.cameraPositionTime = { camera.x, camera.y, camera.z, timeSeconds };
    constants.cameraTargetMode = { 0.0f, 0.0f, 0.0f, settings.debugVoxels ? 1.0f : 0.0f };
    constants.resolutionSteps = { static_cast<float>(m_width), static_cast<float>(m_height),
        static_cast<float>(settings.primarySteps), static_cast<float>(settings.shadowSteps) };
    constants.volumeParameters = { settings.densityScale, settings.extinction,
        settings.scattering, settings.stepScale };
    constants.lightDirectionGrid = { normalizedLight.x, normalizedLight.y, normalizedLight.z,
        static_cast<float>(m_gridResolution) };
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
    constants.cacheParameters = { 1.0f / sizeX, 1.0f / sizeY, 1.0f / sizeZ, 0.0f };

    // ---- Precompute light-cache frame ------------------------------------------
    // Matches LightCacheFrame() in shaders/Volume.hlsl. Doing this on CPU strips
    // ~9 scalars of live registers from LightCacheUVW / LightCacheUVWAndFrame
    // (previously kept right/up/forward + volumeCenter + volumeHalfExtent alive).
    {
        DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(
            DirectX::XMVectorSet(normalizedLight.x, normalizedLight.y, normalizedLight.z, 0.0f));
        DirectX::XMFLOAT3 fwdF; DirectX::XMStoreFloat3(&fwdF, fwd);
        DirectX::XMVECTOR reference = (std::fabs(fwdF.y) < 0.99f)
            ? DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
            : DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(reference, fwd));
        DirectX::XMVECTOR up    = DirectX::XMVector3Cross(fwd, right);

        DirectX::XMFLOAT3 rightF, upF;
        DirectX::XMStoreFloat3(&rightF, right);
        DirectX::XMStoreFloat3(&upF, up);

        const float halfX = (m_volumeData.boundsMax.x - m_volumeData.boundsMin.x) * 0.5f;
        const float halfY = (m_volumeData.boundsMax.y - m_volumeData.boundsMin.y) * 0.5f;
        const float halfZ = (m_volumeData.boundsMax.z - m_volumeData.boundsMin.z) * 0.5f;
        const float centerX = (m_volumeData.boundsMin.x + m_volumeData.boundsMax.x) * 0.5f;
        const float centerY = (m_volumeData.boundsMin.y + m_volumeData.boundsMax.y) * 0.5f;
        const float centerZ = (m_volumeData.boundsMin.z + m_volumeData.boundsMax.z) * 0.5f;

        auto pack = [&](const DirectX::XMFLOAT3& axis, DirectX::XMFLOAT4& scaled, DirectX::XMFLOAT4& plain)
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
    }

    std::memcpy(m_constantData + m_frameIndex * ConstantStride, &constants, sizeof(constants));
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
        m_constantBuffer->GetGPUVirtualAddress() + m_frameIndex * ConstantStride);
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
}

void Renderer::RecordCacheBuild(const RenderSettings& settings)
{
    PIXScopedEvent(m_commandList.Get(), PIX_COLOR(80, 180, 220), "CacheBuild");
    const uint32_t queryBase = m_frameIndex * TimestampCount;
    Transition(m_densityBuffer.Get(), m_densityState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(m_linksBuffer.Get(), m_linksState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(m_transmittanceBuffer.Get(), m_transmittanceState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
        const uint32_t wavesPerGroup = 256u / m_waveLaneCount;
        m_commandList->Dispatch((columnCount + wavesPerGroup - 1) / wavesPerGroup, 1, 1);
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
    Transition(m_transmittanceBuffer.Get(), m_transmittanceState,
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
    const bool useNormalLUT = !naive && settings.useNormalLUT;
    // The normal LUT is derived from the transmittance cache — only rebake it when the
    // cache itself was rebuilt this frame. Previously this dispatched every frame and
    // dominated the profile (~21% of self samples in look-up-from-below shots).
    const bool bakeNormalLUT = useNormalLUT && (cacheRebuiltThisFrame || !m_normalLUTBaked);
    if (bakeNormalLUT)
    {
        PIXScopedEvent(m_commandList.Get(), PIX_COLOR(180, 140, 220), "BakeNormalLUT");
        Transition(m_normalLUTTexture.Get(), m_normalLUTState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->SetPipelineState(m_bakeNormalLUTPso.Get());
            const uint32_t nlutGroups = (m_normalLUTResolution + 3) / 4;
            m_commandList->Dispatch(nlutGroups, nlutGroups, nlutGroups);
        UavBarrier(m_normalLUTTexture.Get());
        Transition(m_normalLUTTexture.Get(), m_normalLUTState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_normalLUTBaked = true;
    }
    else if (useNormalLUT && m_normalLUTState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Transition(m_normalLUTTexture.Get(), m_normalLUTState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else if (!useNormalLUT && m_normalLUTState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Transition(m_normalLUTTexture.Get(), m_normalLUTState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
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
            if (settings.useDistanceField)
            {
                if (useNormalLUT)          cached = m_cachedDfLutPso.Get();
                else if (useReduced)       cached = m_cachedDfReducedTapPso.Get();
                else                       cached = m_cachedDfPso.Get();
            }
            else
            {
                if (useNormalLUT)          cached = m_cachedLutPso.Get();
                else if (useReduced)       cached = m_cachedReducedTapPso.Get();
                else                       cached = m_cachedPso.Get();
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
    Transition(m_outputTexture.Get(), m_outputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
    if (settings.rotateLight)
        settings.rebuildCache = true;
    if (settings.densityThreshold != m_bakedDensityThreshold ||
        settings.densityGamma != m_bakedDensityGamma ||
        settings.densityScale != m_bakedDensityScale)
        m_bakeVolumesPending = true;
    PopulateConstants(settings, timeSeconds);

    m_commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);
    if (m_bakeVolumesPending)
    {
        RecordBakeVolumes(settings);
        settings.rebuildCache = true;
    }
    bool cacheRebuiltThisFrame = false;
    if (settings.rebuildCache || !m_cacheValid)
    {
        RecordCacheBuild(settings);
        settings.rebuildCache = false;
        cacheRebuiltThisFrame = true;
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
        std::move(m_stats.gpuHistory.begin() + 1, m_stats.gpuHistory.end(), m_stats.gpuHistory.begin());
        m_stats.gpuHistory.back() = m_stats.gpuRenderMs;
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
