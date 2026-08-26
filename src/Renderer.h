#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include "VolumeData.h"

struct ImDrawData;

enum class RaymarchMode : int { Naive, Cached };

struct RenderSettings
{
    RaymarchMode mode = RaymarchMode::Naive;
    int primarySteps = 180;
    int shadowSteps = 55;
    int gridResolution = 256;
    float densityScale = 18.948f;
    float extinction = 3.0f;
    float scattering = 0.557f;
    float stepScale = 1.0f;
    float densityThreshold = 0.054f;
    float densityGamma = 0.8f;
    float shadowDensityScale = 1.093f;
    float phaseAnisotropy = 0.516f;
    float directLightIntensity = 1.007f;
    float multipleScattering = 0.452f;
    float lightingSkipEpsilon = 0.0018f;
    float powderStrength = 0.690f;
    float ambientLight = 0.155f;
    float exposure = 1.25f;
    float contrast = 1.08f;
    float displayGamma = 2.2f;
    float backgroundIntensity = 0.987f;
    DirectX::XMFLOAT3 lightColor{ 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 ambientColor{ 0.702f, 0.702f, 0.702f };
    DirectX::XMFLOAT3 lightDirection{ 0.277f, 0.417f, 0.168f };
    float cameraYaw = -1.0297f;
    float cameraPitch = -0.1920f;
    float cameraDistance = 4.025f;
    float lightRotationSpeed = 0.25f;
    bool enableSurfaceNormals = true;
    bool enableTileClassify = false;
    bool useNormalLUT = true;
    // When true, uses 4-tap forward-diff normals instead of 6-tap central. Only takes
    // effect when useNormalLUT is off (LUT path is already 1 tap).
    bool useReducedTapNormals = false;
    // When true, dispatches the *_CountCS variants that write per-pixel
    // DenseDensityAt sample counts into m_sampleCountTexture, then runs
    // HeatmapDisplayCS to false-colour the count on top of the main output.
    bool showDensityHeatmap = false;
    // Which sample counter the heatmap visualises.
    //   0 = DenseVolume (DenseDensityAt)   — main density fetch cost.
    //   1 = CachedDepth (CacheNormalDotLight & finite-diff normal taps).
    //   2 = both (sum) — combined texture-fetch pressure.
    int heatmapCounterMode = 0;
    int heatmapMaxSamples = 256;
    int normalLUTResolution = 192;
    bool useDistanceField = false;
    bool debugDistanceField = false;
    bool useSplitTrace = false;
    bool useTileVariantDispatch = false;
    bool debugTiles = false;
    bool autoOrbit = false;
    bool rotateLight = false;
    bool debugVoxels = false;
    bool rebuildCache = true;
    int vsyncMode = 1;
};

struct RendererStats
{
    float gpuTotalMs = 0.0f;
    float gpuRenderMs = 0.0f;
    float gpuRenderSampleMs = 0.0f;
    float gpuVoxelizeMs = 0.0f;
    float gpuLinkMs = 0.0f;
    float gpuResolveMs = 0.0f;
    float lastCacheBuildMs = 0.0f;
    float cpuActiveMs = 0.0f;
    uint32_t occupiedCells = 0;
    uint32_t compactCacheCells = 0;
    uint32_t totalCells = 0;
    uint32_t volumeWidth = 0;
    uint32_t volumeHeight = 0;
    uint32_t volumeDepth = 0;
    uint32_t activeBricks = 0;
    uint64_t sourceActiveVoxels = 0;
    uint64_t denseVolumeBytes = 0;
    uint64_t sparseVolumeBytes = 0;
    uint64_t normalLUTBytes = 0;
    uint64_t cacheBytes = 0;
    std::array<float, 180> gpuHistory{};
};

class Renderer
{
public:
    ~Renderer();
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);
    void BeginFrame();
    void Render(RenderSettings& settings, ImDrawData* drawData, float timeSeconds);
    void WaitForGpu();
    bool CaptureOutput(std::vector<uint8_t>& rgbaPixels);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv() const;
    const RendererStats& GetStats() const { return m_stats; }
    const std::string& GetLastError() const { return m_lastError; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    static constexpr uint32_t FrameCount = 2;
    static constexpr uint32_t TimestampCount = 10;

    enum DescriptorIndex : uint32_t
    {
        ImGuiFont, OutputUav, DensityUav, LinksUav, TransmittanceUav, StatsUav,
        RemappedVolumeUav, RemappedVolumeMip3Uav, TileClassificationUav,
        NormalLUTUav,
        DistanceFieldAUav, DistanceFieldBUav,
        HitIntervalUav,
        TileListUav, IndirectDispatchArgsUav,
        SampleCountUav,
        OutputSrv, DensitySrv, LinksSrv, TransmittanceSrv, DenseVolumeSrv,
        SparsePageSrv, SparseBrickSrv,
        RemappedVolumeSrv, TileClassificationSrv, NormalLUTSrv, DistanceFieldSrv,
        DescriptorCount
    };

    struct FrameContext
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
    };

    struct alignas(16) ShaderConstants
    {
        DirectX::XMFLOAT4 cameraPositionTime;
        DirectX::XMFLOAT4 cameraTargetMode;
        DirectX::XMFLOAT4 resolutionSteps;
        DirectX::XMFLOAT4 volumeParameters;
        DirectX::XMFLOAT4 lightDirectionGrid;
        DirectX::XMFLOAT4 volumeMin;
        DirectX::XMFLOAT4 volumeMax;
        DirectX::XMFLOAT4 cloudParameters;
        DirectX::XMFLOAT4 volumeGridParameters;
        DirectX::XMFLOAT4 sparseGridParameters;
        DirectX::XMFLOAT4 densityTransferParameters;
        DirectX::XMFLOAT4 lightingParameters;
        DirectX::XMFLOAT4 lightColorParameters;
        DirectX::XMFLOAT4 ambientColorParameters;
        DirectX::XMFLOAT4 cacheParameters;
        // Precomputed cache-frame axes so shaders don't recompute normalize/cross/dot
        // every LightCacheUVW call. See PopulateConstants for derivation.
        // *Scaled entries: xyz = axis / (2 * dot(abs(axis), volumeHalfExtent)),
        //                  w  = 0.5 - dot(volumeCenter, axis) / (2 * dot(abs(axis), volumeHalfExtent))
        // So uvw_i = saturate(dot(p, *Scaled.xyz) + *Scaled.w).
        // *Axis entries hold the plain unit vector for callers that need the frame.
        DirectX::XMFLOAT4 lightCacheRightScaled;
        DirectX::XMFLOAT4 lightCacheUpScaled;
        DirectX::XMFLOAT4 lightCacheForwardScaled;
        DirectX::XMFLOAT4 lightCacheRightAxis;
        DirectX::XMFLOAT4 lightCacheUpAxis;
        DirectX::XMFLOAT4 lightCacheForwardAxis;
    };

    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateCoreResources();
    bool CreateSizeDependentResources();
    bool CreateVolumeResources(uint32_t resolution);
    bool CreateNormalLUT(uint32_t resolution);
    bool CreateVdbResources();
    bool CreatePipelines();
    void DestroySizeDependentResources();
    void RecordCacheBuild(const RenderSettings& settings);
    void RecordBakeVolumes(const RenderSettings& settings);
    void RecordScene(const RenderSettings& settings, bool cacheRebuiltThisFrame);
    void PopulateConstants(const RenderSettings& settings, float timeSeconds);
    void ReadCompletedTimings(uint32_t frameIndex);
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES next);
    void UavBarrier(ID3D12Resource* resource);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t index) const;
    void SetError(const std::string& message);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0, m_frameIndex = 0, m_gridResolution = 0;
    uint32_t m_descriptorSize = 0, m_rtvDescriptorSize = 0;
    uint64_t m_fenceValue = 0, m_timestampFrequency = 1;
    uint32_t m_waveLaneCount = 32;
    HANDLE m_fenceEvent = nullptr;
    bool m_initialized = false, m_cacheValid = false;

    std::array<FrameContext, FrameCount> m_frames;
    std::array<bool, FrameCount> m_cacheBuiltFrames{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_backBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_timestampReadbacks;
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer, m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_densityBuffer, m_linksBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_transmittanceBuffer, m_statsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_denseVolumeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sparsePageTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sparseBrickBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_remappedVolumeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tileClassificationBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalLUTTexture;
    uint32_t m_normalLUTResolution = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distanceFieldTextureA;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distanceFieldTextureB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hitIntervalBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tileList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indirectDispatchArgs;
    // R32_UINT texture that receives per-pixel DenseDensityAt sample counts when the
    // heatmap debug view is enabled. Sized to render dimensions.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sampleCountTexture;
    D3D12_RESOURCE_STATES m_sampleCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_indirectDispatchArgsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_statsReadbacks;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_resolvePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_naivePso, m_cachedPso, m_cachedDfPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutPso, m_cachedDfLutPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedNoSNPso, m_cachedDfNoSNPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutNoSNPso, m_cachedDfLutNoSNPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedDebugPso;
    // Reduced-tap normal variants (4-tap forward diff instead of 6-tap central).
    // Meaningful only when the normal LUT is off; used via `useReducedTapNormals` UI.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedReducedTapPso, m_cachedDfReducedTapPso;
    // Sample-counting variants of the render entry points + heatmap display pass.
    // Only used when RenderSettings::showDensityHeatmap is set.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedCountPso, m_naiveCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutCountPso, m_cachedDfCountPso, m_cachedDfLutCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedNoSNCountPso, m_cachedLutNoSNCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedDfNoSNCountPso, m_cachedDfLutNoSNCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedReducedTapCountPso, m_cachedDfReducedTapCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_heatmapDisplayPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeRemappedPso, m_bakeOccupancyPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeNormalLUTPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeDistanceFieldInitPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeDistanceFieldPropagatePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugDistanceFieldPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_findHitIntervalPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_splitCachedPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_clearTileDispatchPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_splitCachedIndirectPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_splitCachedSkyPso;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_classifyTilesPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugTileOverlayPso;

    uint8_t* m_constantData = nullptr;
    D3D12_RESOURCE_STATES m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_densityState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_linksState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_transmittanceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_statsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_remappedVolumeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_tileClassificationState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_normalLUTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool m_normalLUTBaked = false;
    D3D12_RESOURCE_STATES m_distanceFieldAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_distanceFieldBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    float m_bakedDensityThreshold = std::numeric_limits<float>::quiet_NaN();
    float m_bakedDensityGamma = std::numeric_limits<float>::quiet_NaN();
    float m_bakedDensityScale = std::numeric_limits<float>::quiet_NaN();
    bool m_bakeVolumesPending = true;
    VolumeData m_volumeData;
    DirectX::XMFLOAT3 m_effectiveLightDirection{};
    RendererStats m_stats;
    std::string m_lastError;
};
